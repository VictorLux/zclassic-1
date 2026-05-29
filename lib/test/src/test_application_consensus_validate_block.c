/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Unit tests for application/consensus/validate_block.
 *
 * The use case is exercised entirely through its public surface:
 *  - a synthetic struct block (constructed in-process, never on disk)
 *  - synthetic consensus_params with a maximally permissive powLimit
 *    and an nBits chosen so PoW trivially passes (so the failures we
 *    observe are the ones we *intended* to test)
 *  - an in-memory fake utxo_snapshot_port (see fake_utxo_*)
 *
 * The fake snapshot lives entirely in this file. It only implements
 * the `lookup` method — that's the only port hook the use case calls.
 * This is the canonical pattern for use-case tests under the new
 * architecture: the test owns the port impl, the use case sees only
 * the port interface.
 */

#include "test/test_helpers.h"

#include "application/consensus/validate_block.h"
#include "ports/utxo_snapshot_port.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "consensus/params.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVB_CHECK(name, expr) do {                              \
    printf("application_consensus_validate_block: %s... ", (name)); \
    if ((expr)) { printf("OK\n"); }                             \
    else { printf("FAIL\n"); failures++; }                      \
} while (0)

/* ---- fake utxo_snapshot_port ----------------------------------- */

struct fake_utxo_entry {
    struct utxo_outpoint op;
};

struct fake_utxo_state {
    struct fake_utxo_entry *entries;
    size_t count;
    size_t capacity;
};

static struct zcl_result fake_utxo_lookup(void *self_v,
                                          const struct utxo_outpoint *op,
                                          struct utxo_coin *coin_out)
{
    struct fake_utxo_state *s = (struct fake_utxo_state *)self_v;
    for (size_t i = 0; i < s->count; i++) {
        if (s->entries[i].op.vout == op->vout &&
            memcmp(s->entries[i].op.txid, op->txid, 32) == 0) {
            if (coin_out) {
                memset(coin_out, 0, sizeof(*coin_out));
                coin_out->value_zat = 1000;
                coin_out->height = 1;
                coin_out->is_coinbase = false;
            }
            return ZCL_OK;
        }
    }
    return ZCL_ERR(UTXO_ERR_NOT_FOUND, "fake_utxo: outpoint absent");
}

static void fake_utxo_insert(struct fake_utxo_state *s,
                             const struct utxo_outpoint *op)
{
    if (s->count == s->capacity) {
        size_t ncap = s->capacity ? s->capacity * 2 : 8;
        s->entries = realloc(s->entries, ncap * sizeof(*s->entries));
        s->capacity = ncap;
    }
    s->entries[s->count].op = *op;
    s->count++;
}

static void fake_utxo_init(struct fake_utxo_state *s,
                           struct utxo_snapshot_port *port)
{
    memset(s, 0, sizeof(*s));
    memset(port, 0, sizeof(*port));
    port->self = s;
    port->lookup = fake_utxo_lookup;
    /* apply_diff / revert_tip / tip_height / tip_hash / sha3_commitment
     * are not exercised by validate_block; leaving them NULL is fine. */
}

static void fake_utxo_free(struct fake_utxo_state *s)
{
    free(s->entries);
    memset(s, 0, sizeof(*s));
}

/* ---- params + block builders ----------------------------------- */

/* test_make_easy_consensus_params() sets powLimit to all-ones so any
 * target derived from a legal nBits is at or below it. With
 * nBits = 0x207fffff (regtest-style "trivially easy" target ~ 2^255)
 * and a small mining loop on nNonce.data[0], we deterministically find
 * a header whose hash satisfies PoW in ~1-2 iterations. */

/* Mine the header by bumping nNonce until block_get_hash <= target.
 * Bounded to 1024 iterations — at the easy target the loop exits in
 * 1-2 iterations on average. If the bound is exceeded the test will
 * report PoW failure, which is what we want (the synthetic header
 * builder is wrong). */
static void mine_easy_pow(struct block *b)
{
    bool neg = false, ov = false;
    struct arith_uint256 target;
    arith_uint256_set_compact(&target, b->header.nBits, &neg, &ov);
    for (uint32_t i = 0; i < 1024; i++) {
        b->header.nNonce.data[0] = (uint8_t)(i & 0xff);
        b->header.nNonce.data[1] = (uint8_t)((i >> 8) & 0xff);
        struct uint256 h;
        block_get_hash(b, &h);
        struct arith_uint256 ha;
        uint256_to_arith(&ha, &h);
        if (arith_uint256_compare(&ha, &target) <= 0) return;
    }
}

static void minimal_coinbase(struct transaction *tx)
{
    transaction_init(tx);
    transaction_alloc(tx, 1, 1);
    /* coinbase: single input whose prevout is null */
    outpoint_set_null(&tx->vin[0].prevout);
    tx->vin[0].sequence = UINT32_MAX;
    tx->vout[0].value = 1250000000;  /* placeholder subsidy */
    tx->vout[0].script_pub_key.size = 0;
}

static void spending_tx(struct transaction *tx,
                        const struct utxo_outpoint *spend)
{
    transaction_init(tx);
    transaction_alloc(tx, 1, 1);
    memcpy(tx->vin[0].prevout.hash.data, spend->txid, 32);
    tx->vin[0].prevout.n = spend->vout;
    tx->vin[0].sequence = UINT32_MAX;
    tx->vout[0].value = 500;
    tx->vout[0].script_pub_key.size = 0;
}

/* ---- the test --------------------------------------------------- */

int test_application_consensus_validate_block(void)
{
    int failures = 0;

    /* 1. NULL inputs guard. */
    {
        struct zcl_result r = application_consensus_validate_block(NULL);
        AVB_CHECK("null inputs -> ERR_NULL_ARG",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_NULL_ARG);
    }

    /* 2. Block with zero transactions. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        mine_easy_pow(&b);
        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = NULL,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("zero txs -> ERR_NO_TRANSACTIONS",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_NO_TRANSACTIONS);
        block_free(&b);
    }

    /* 3. Coinbase-only block, no UTXO port — should pass. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(1, sizeof(struct transaction));
        b.num_vtx = 1;
        minimal_coinbase(&b.vtx[0]);
        mine_easy_pow(&b);

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = NULL,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("coinbase-only, no utxo -> OK", r.ok);
        block_free(&b);
    }

    /* 4. First tx is NOT coinbase. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(1, sizeof(struct transaction));
        b.num_vtx = 1;
        struct utxo_outpoint spent = {0};
        spent.vout = 0;
        spending_tx(&b.vtx[0], &spent);
        mine_easy_pow(&b);

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = NULL,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("first tx not coinbase -> ERR_MISSING_COINBASE",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_MISSING_COINBASE);
        block_free(&b);
    }

    /* 5. Two coinbases (extra coinbase) — reject. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(2, sizeof(struct transaction));
        b.num_vtx = 2;
        minimal_coinbase(&b.vtx[0]);
        minimal_coinbase(&b.vtx[1]);
        mine_easy_pow(&b);

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = NULL,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("extra coinbase -> ERR_EXTRA_COINBASE",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_EXTRA_COINBASE);
        block_free(&b);
    }

    /* 6. Non-coinbase input not in UTXO -> ERR_INPUT_NOT_FOUND. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);

        struct fake_utxo_state s;
        struct utxo_snapshot_port port;
        fake_utxo_init(&s, &port);

        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(2, sizeof(struct transaction));
        b.num_vtx = 2;
        minimal_coinbase(&b.vtx[0]);
        struct utxo_outpoint spent = {0};
        spent.txid[0] = 0xab;
        spent.vout = 7;
        spending_tx(&b.vtx[1], &spent);
        mine_easy_pow(&b);
        /* deliberately do NOT insert into snapshot */

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = &port,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("input absent from utxo -> ERR_INPUT_NOT_FOUND",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_INPUT_NOT_FOUND);
        block_free(&b);
        fake_utxo_free(&s);
    }

    /* 7. Non-coinbase input PRESENT in UTXO -> OK. */
    {
        struct consensus_params p; test_make_easy_consensus_params(&p);

        struct fake_utxo_state s;
        struct utxo_snapshot_port port;
        fake_utxo_init(&s, &port);

        struct utxo_outpoint spent = {0};
        spent.txid[0] = 0xcd;
        spent.vout = 3;
        fake_utxo_insert(&s, &spent);

        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(2, sizeof(struct transaction));
        b.num_vtx = 2;
        minimal_coinbase(&b.vtx[0]);
        spending_tx(&b.vtx[1], &spent);
        mine_easy_pow(&b);

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = &port,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("input present in utxo -> OK", r.ok);
        block_free(&b);
        fake_utxo_free(&s);
    }

    /* 8. PoW rejection propagates as ERR_POW_INVALID with the
     *    domain code surfaced in the message. */
    {
        struct consensus_params p;
        memset(&p, 0, sizeof(p));
        p.powLimit.data[0] = 0x01;  /* powLimit = 1 — impossible floor */
        struct block b; block_init(&b);
        b.header.nBits = 0x1d00ffff;  /* normal mainnet-ish nBits */
        b.vtx = calloc(1, sizeof(struct transaction));
        b.num_vtx = 1;
        minimal_coinbase(&b.vtx[0]);

        struct application_consensus_validate_block_inputs in = {
            .block = &b, .params = &p, .utxo = NULL,
        };
        struct zcl_result r = application_consensus_validate_block(&in);
        AVB_CHECK("pow rejected -> ERR_POW_INVALID",
                  !r.ok && r.code == APPLICATION_CONSENSUS_ERR_POW_INVALID);
        block_free(&b);
    }

    return failures;
}
