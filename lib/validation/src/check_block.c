/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/check_block.h"
#include "bloom/merkle.h"
#include "chain/chainparams.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "validation/check_transaction.h"
#include "validation/contextual_check_tx.h"
#include "validation/main_constants.h"
#include "validation/sigops.h"
#include "script/script_flags.h"
#include "util/timedata.h"
#include <assert.h>
#include <math.h>
#include <string.h>

bool check_block_header(const struct block_header *header,
                        struct validation_state *state,
                        const struct chain_params *params,
                        bool check_pow)
{
    if (header->nVersion < MIN_BLOCK_VERSION)
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "version-too-low", false, NULL);

    if (check_pow && !check_equihash_solution(header, params))
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "invalid-solution", false, NULL);

    if (check_pow) {
        struct uint256 hash;
        block_header_get_hash(header, &hash);
        if (!CheckProofOfWork(hash, header->nBits, &params->consensus))
            return validation_state_dos(state, 50, false, REJECT_INVALID,
                                        "high-hash", false, NULL);
    }

    if (block_header_get_time(header) > GetAdjustedTime() + 2 * 60 * 60)
        return validation_state_invalid(state, false, REJECT_INVALID,
                                        "time-too-new", NULL);

    return true;
}

bool check_block(const struct block *block,
                 struct validation_state *state,
                 const struct chain_params *params,
                 bool check_pow,
                 bool check_merkle_root,
                 bool check_size_limits)
{
    if (!check_block_header(&block->header, state, params, check_pow))
        return false;

    if (check_merkle_root) {
        struct uint256 *txids = malloc(block->num_vtx * sizeof(struct uint256));
        if (!txids && block->num_vtx > 0)
            return validation_state_error(state, "out-of-memory");

        for (size_t i = 0; i < block->num_vtx; i++)
            txids[i] = block->vtx[i].hash;

        bool mutated;
        struct uint256 merkle_root =
            compute_merkle_root_mutated(txids, block->num_vtx, &mutated);
        free(txids);

        if (!uint256_eq(&block->header.hashMerkleRoot, &merkle_root))
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txnmrklroot", true, NULL);

        if (mutated)
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-txns-duplicate", true, NULL);
    }

    if (check_size_limits) {
        const unsigned int GENEROUS_BLOCK_SIZE_LIMIT = 2000000;
        if (block->num_vtx == 0 || block->num_vtx > GENEROUS_BLOCK_SIZE_LIMIT)
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-blk-length", false, NULL);

        if (!transaction_is_coinbase(&block->vtx[0]))
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-missing", false, NULL);
        for (size_t i = 1; i < block->num_vtx; i++) {
            if (transaction_is_coinbase(&block->vtx[i]))
                return validation_state_dos(state, 100, false, REJECT_INVALID,
                                            "bad-cb-multiple", false, NULL);
        }

        for (size_t i = 0; i < block->num_vtx; i++) {
            if (!check_transaction(&block->vtx[i], state))
                return false;
        }

        unsigned int nSigOps = 0;
        for (size_t i = 0; i < block->num_vtx; i++)
            nSigOps += (unsigned int)get_legacy_sig_op_count(
                &block->vtx[i], SCRIPT_VERIFY_NONE);
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-blk-sigops", true, NULL);
    }

    return true;
}

bool contextual_check_block_header(const struct block_header *header,
                                   struct validation_state *state,
                                   const struct chain_params *params,
                                   const struct block_index *pindex_prev,
                                   bool checkpoints_enabled)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);
    if (uint256_eq(&hash, &params->consensus.hashGenesisBlock))
        return true;

    assert(pindex_prev);

    int nHeight = pindex_prev->nHeight + 1;

    size_t sol_size = header->nSolutionSize;
    if (sol_size > 0) {
        unsigned int n = chain_params_equihash_n(params, nHeight);
        unsigned int k = chain_params_equihash_k(params, nHeight);
        size_t expected = (size_t)((pow(2, k) * ((n / (k + 1)) + 1)) / 8);
        if (sol_size != expected)
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-equihash-solution-size",
                                        false, NULL);
    }

    if (header->nBits != GetNextWorkRequired(pindex_prev, header,
                                             &params->consensus))
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-diffbits", false, NULL);

    if (block_header_get_time(header) <=
        block_index_get_median_time_past(pindex_prev))
        return validation_state_invalid(state, false, REJECT_INVALID,
                                        "time-too-old", NULL);

    if (checkpoints_enabled) {
        const struct checkpoint_data *cpdata = &params->checkpointData;
        for (int i = 0; i < cpdata->nEntries; i++) {
            if (nHeight == cpdata->entries[i].height) {
                if (uint256_cmp(&hash, &cpdata->entries[i].hash) != 0)
                    return validation_state_dos(state, 100, false,
                        REJECT_CHECKPOINT, "bad-fork-at-checkpoint",
                        false, NULL);
            }
        }
    }

    if (header->nVersion < 4)
        return validation_state_invalid(state, false, REJECT_OBSOLETE,
                                        "bad-version", NULL);

    return true;
}

static bool bip34_check_coinbase_height(const struct transaction *coinbase,
                                        int nHeight)
{
    if (coinbase->num_vin == 0)
        return false;

    const struct script *sig = &coinbase->vin[0].script_sig;
    if (nHeight <= 0)
        return true;
    if (sig->size == 0)
        return false;

    /* Early blocks (height 1-16) may use OP_N (0x51-0x60) encoding */
    if (nHeight >= 1 && nHeight <= 16) {
        unsigned char op_n = 0x50 + (unsigned char)nHeight;
        if (sig->data[0] == op_n)
            return true;
    }

    /* Encode height as CScriptNum: minimal signed little-endian with length prefix.
     * Values with high bit set need an extra 0x00 sign byte. */
    unsigned char expect[6];
    size_t expect_len = 0;
    {
        int h = nHeight;
        unsigned char num[4];
        size_t num_len = 0;
        while (h > 0) {
            num[num_len++] = (unsigned char)(h & 0xff);
            h >>= 8;
        }
        if (num_len > 0 && (num[num_len - 1] & 0x80))
            num[num_len++] = 0x00;
        expect[0] = (unsigned char)num_len;
        memcpy(expect + 1, num, num_len);
        expect_len = 1 + num_len;
    }

    if (sig->size < expect_len)
        return false;
    return memcmp(sig->data, expect, expect_len) == 0;
}

bool contextual_check_block(const struct block *block,
                            struct validation_state *state,
                            const struct chain_params *params,
                            const struct block_index *pindex_prev)
{
    int nHeight = pindex_prev == NULL ? 0 : pindex_prev->nHeight + 1;

    for (size_t i = 0; i < block->num_vtx; i++) {
        if (!contextual_check_transaction(&block->vtx[i], state,
                                          &params->consensus, nHeight, 100))
            return false;

        int64_t nLockTimeCutoff = block_header_get_time(&block->header);
        if (!is_final_tx(&block->vtx[i], nHeight, nLockTimeCutoff))
            return validation_state_dos(state, 10, false, REJECT_INVALID,
                                        "bad-txns-nonfinal", false, NULL);
    }

    if (nHeight > 0) {
        if (!bip34_check_coinbase_height(&block->vtx[0], nHeight))
            return validation_state_dos(state, 100, false, REJECT_INVALID,
                                        "bad-cb-height", false, NULL);
    }

    return true;
}
