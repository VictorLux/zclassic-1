/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "application/consensus/validate_block.h"

#include "domain/consensus/verify.h"
#include "ports/utxo_snapshot_port.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/uint256.h"

#include <string.h>

static struct zcl_result check_pow(const struct block *b,
                                   const struct consensus_params *p)
{
    struct uint256 block_hash;
    block_get_hash(b, &block_hash);
    struct zcl_result r = domain_consensus_verify_pow_solution(
            &block_hash, b->header.nBits, p);
    if (!r.ok)
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_POW_INVALID,
                       "validate_block: pow rejected (domain code %d): %s",
                       r.code, r.message);
    return ZCL_OK;
}

static struct zcl_result check_coinbase_shape(const struct block *b)
{
    if (b->num_vtx == 0)
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_NO_TRANSACTIONS,
                       "validate_block: block has zero transactions");
    if (b->vtx == NULL)
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_NULL_INPUT_ARRAY,
                       "validate_block: num_vtx=%zu but vtx is NULL",
                       b->num_vtx);
    if (!transaction_is_coinbase(&b->vtx[0]))
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_MISSING_COINBASE,
                       "validate_block: first transaction is not coinbase");
    for (size_t i = 1; i < b->num_vtx; i++) {
        if (transaction_is_coinbase(&b->vtx[i]))
            return ZCL_ERR(APPLICATION_CONSENSUS_ERR_EXTRA_COINBASE,
                           "validate_block: non-first coinbase at tx index %zu", i);
    }
    return ZCL_OK;
}

static struct zcl_result check_inputs_exist(
        const struct block *b,
        const struct utxo_snapshot_port *utxo)
{
    /* utxo->lookup() is required; the port handle must be fully populated. */
    if (utxo->lookup == NULL)
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_NULL_ARG,
                       "validate_block: utxo_snapshot_port.lookup is NULL");

    for (size_t i = 1; i < b->num_vtx; i++) {  /* skip coinbase at index 0 */
        const struct transaction *tx = &b->vtx[i];
        if (tx->num_vin != 0 && tx->vin == NULL)
            return ZCL_ERR(APPLICATION_CONSENSUS_ERR_NULL_INPUT_ARRAY,
                           "validate_block: tx %zu has num_vin=%zu but vin is NULL",
                           i, tx->num_vin);
        for (size_t j = 0; j < tx->num_vin; j++) {
            struct utxo_outpoint op;
            memcpy(op.txid, tx->vin[j].prevout.hash.data, 32);
            op.vout = tx->vin[j].prevout.n;

            struct utxo_coin coin = {0};
            struct zcl_result r = utxo->lookup(utxo->self, &op, &coin);
            if (!r.ok) {
                if (r.code == UTXO_ERR_NOT_FOUND)
                    return ZCL_ERR(APPLICATION_CONSENSUS_ERR_INPUT_NOT_FOUND,
                                   "validate_block: tx %zu input %zu spends "
                                   "unknown outpoint (vout=%u)",
                                   i, j, op.vout);
                return ZCL_ERR(APPLICATION_CONSENSUS_ERR_INPUT_LOOKUP_FAILED,
                               "validate_block: utxo lookup failed for tx %zu "
                               "input %zu (port code %d): %s",
                               i, j, r.code, r.message);
            }
        }
    }
    return ZCL_OK;
}

struct zcl_result application_consensus_validate_block(
        const struct application_consensus_validate_block_inputs *in)
{
    if (!in || !in->block || !in->params)
        return ZCL_ERR(APPLICATION_CONSENSUS_ERR_NULL_ARG,
                       "validate_block: null inputs / block / params");

    {
        struct zcl_result r = check_pow(in->block, in->params);
        if (!r.ok) return r;
    }
    {
        struct zcl_result r = check_coinbase_shape(in->block);
        if (!r.ok) return r;
    }
    if (in->utxo != NULL) {
        struct zcl_result r = check_inputs_exist(in->block, in->utxo);
        if (!r.ok) return r;
    }
    return ZCL_OK;
}
