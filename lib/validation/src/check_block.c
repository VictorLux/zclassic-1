/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * CheckBlock, CheckBlockHeader, ContextualCheckBlock[Header]
 * 21 checks matching zclassicd main.cpp:3922-4101 exactly.
 *
 * Uses REJECT_IF / REJECT_UNLESS / REJECT_CORRUPT_IF macros. */

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

/* ── CheckBlockHeader (4 checks) ──────────────────────────────── */

bool check_block_header(const struct block_header *header,
                        struct validation_state *state,
                        const struct chain_params *params,
                        bool check_pow)
{
    REJECT_IF(header->nVersion < MIN_BLOCK_VERSION,
              state, 100, "version-too-low");

    if (check_pow) {
        REJECT_IF(!check_equihash_solution(header, params),
                  state, 100, "invalid-solution");

        struct uint256 hash;
        block_header_get_hash(header, &hash);
        REJECT_IF(!CheckProofOfWork(hash, header->nBits, &params->consensus),
                  state, 50, "high-hash");
    }

    REJECT_INVALID_IF(
        block_header_get_time(header) > GetAdjustedTime() + 2 * 60 * 60,
        state, "time-too-new");

    return true;
}

/* ── CheckBlock (8 checks) ─────────────────────────────────────── */

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
            REJECT_FATAL(state, "out-of-memory");

        for (size_t i = 0; i < block->num_vtx; i++)
            txids[i] = block->vtx[i].hash;

        bool mutated;
        struct uint256 merkle_root =
            compute_merkle_root_mutated(txids, block->num_vtx, &mutated);
        free(txids);

        REJECT_CORRUPT_IF(
            !uint256_eq(&block->header.hashMerkleRoot, &merkle_root),
            state, 100, "bad-txnmrklroot");

        REJECT_CORRUPT_IF(mutated, state, 100, "bad-txns-duplicate");
    }

    if (check_size_limits) {
        const unsigned int GENEROUS_BLOCK_SIZE_LIMIT = 2000000;

        REJECT_IF(block->num_vtx == 0 ||
                  block->num_vtx > GENEROUS_BLOCK_SIZE_LIMIT,
                  state, 100, "bad-blk-length");

        REJECT_IF(!transaction_is_coinbase(&block->vtx[0]),
                  state, 100, "bad-cb-missing");

        for (size_t i = 1; i < block->num_vtx; i++) {
            REJECT_IF(transaction_is_coinbase(&block->vtx[i]),
                      state, 100, "bad-cb-multiple");
        }

        for (size_t i = 0; i < block->num_vtx; i++) {
            if (!check_transaction(&block->vtx[i], state))
                return false;
        }

        unsigned int nSigOps = 0;
        for (size_t i = 0; i < block->num_vtx; i++)
            nSigOps += (unsigned int)get_legacy_sig_op_count(
                &block->vtx[i], SCRIPT_VERIFY_NONE);
        REJECT_CORRUPT_IF(nSigOps > MAX_BLOCK_SIGOPS,
                          state, 100, "bad-blk-sigops");
    }

    return true;
}

/* ── ContextualCheckBlockHeader (6 checks) ─────────────────────── */

bool contextual_check_block_header(const struct block_header *header,
                                   struct validation_state *state,
                                   const struct chain_params *params,
                                   const struct block_index *pindex_prev,
                                   bool checkpoints_enabled)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    /* Genesis: skip all contextual checks */
    if (uint256_eq(&hash, &params->consensus.hashGenesisBlock))
        return true;

    if (!pindex_prev)
        REJECT_FATAL(state, "prev-block-index-null");

    int nHeight = pindex_prev->nHeight + 1;

    /* Equihash solution size for this height's (N,K) params */
    size_t sol_size = header->nSolutionSize;
    if (sol_size > 0) {
        unsigned int n = chain_params_equihash_n(params, nHeight);
        unsigned int k = chain_params_equihash_k(params, nHeight);
        size_t expected = (size_t)((pow(2, k) * ((n / (k + 1)) + 1)) / 8);
        REJECT_IF(sol_size != expected,
                  state, 100, "bad-equihash-solution-size");
    }

    /* Difficulty check — skip during assumed-valid or incomplete windows */
    if (g_assume_valid_height >= 0 && nHeight <= g_assume_valid_height)
        goto skip_diffbits;
    {
        bool window_clean = true;
        const struct block_index *check = pindex_prev;
        for (int w = 0; w < 17 && check; w++) {
            if (check->nBits == 0) { window_clean = false; break; }
            check = check->pprev;
        }
        if (!window_clean) goto skip_diffbits;
    }
    {
        unsigned int expected_bits = GetNextWorkRequired(pindex_prev, header,
                                                         &params->consensus);
        if (header->nBits != expected_bits) {
            printf("bad-diffbits at height %d: header=0x%08x "
                   "expected=0x%08x prev_height=%d prev_bits=0x%08x\n",
                   nHeight, header->nBits, expected_bits,
                   pindex_prev->nHeight, pindex_prev->nBits);
            fflush(stdout);
            REJECT_IF(true, state, 100, "bad-diffbits");
        }
    }
    skip_diffbits:

    /* Timestamp must be after median of previous 11 blocks */
    REJECT_INVALID_IF(
        block_header_get_time(header) <=
            block_index_get_median_time_past(pindex_prev),
        state, "time-too-old");

    /* Checkpoint enforcement */
    if (checkpoints_enabled) {
        const struct checkpoint_data *cpdata = &params->checkpointData;
        for (int i = 0; i < cpdata->nEntries; i++) {
            if (nHeight == cpdata->entries[i].height) {
                REJECT_CHECKPOINT_IF(
                    uint256_cmp(&hash, &cpdata->entries[i].hash) != 0,
                    state, 100, "bad-fork-at-checkpoint");
            }
        }
    }

    REJECT_OBSOLETE_IF(header->nVersion < 4, state, "bad-version");

    return true;
}

/* ── BIP34 coinbase height encoding ────────────────────────────── */

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

    /* Encode height as CScriptNum: minimal signed little-endian */
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

/* ── ContextualCheckBlock (3 checks) ──────────────────────────── */

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
        REJECT_UNLESS(is_final_tx(&block->vtx[i], nHeight, nLockTimeCutoff),
                      state, 10, "bad-txns-nonfinal");
    }

    if (nHeight > 0) {
        REJECT_UNLESS(bip34_check_coinbase_height(&block->vtx[0], nHeight),
                      state, 100, "bad-cb-height");
    }

    return true;
}
