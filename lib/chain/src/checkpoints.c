/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/checkpoints.h"
#include "util/log_macros.h"
#include <stddef.h>
#include <time.h>

static const double SIGCHECK_VERIFICATION_FACTOR = 5.0;

int checkpoints_get_total_blocks_estimate(const struct checkpoint_data *data)
{
    if (data->nEntries == 0)
        return 0;
    return data->entries[data->nEntries - 1].height;
}

double checkpoints_guess_verification_progress(
    const struct checkpoint_data *data,
    const struct block_index *pindex, bool fSigchecks)
{
    if (pindex == NULL)
        return 0.0;

    int64_t nNow = time(NULL);
    double fSigcheckFactor = fSigchecks ? SIGCHECK_VERIFICATION_FACTOR : 1.0;
    double fWorkBefore = 0.0;
    double fWorkAfter = 0.0;

    if ((int64_t)pindex->nChainTx <= data->nTransactionsLastCheckpoint) {
        double nCheapBefore = (double)pindex->nChainTx;
        double nCheapAfter = (double)data->nTransactionsLastCheckpoint - nCheapBefore;
        double nExpensiveAfter = (double)(nNow - data->nTimeLastCheckpoint) /
                                 86400.0 * data->fTransactionsPerDay;
        fWorkBefore = nCheapBefore;
        fWorkAfter = nCheapAfter + nExpensiveAfter * fSigcheckFactor;
    } else {
        double nCheapBefore = (double)data->nTransactionsLastCheckpoint;
        double nExpensiveBefore = (double)pindex->nChainTx - nCheapBefore;
        double nExpensiveAfter = (double)(nNow - block_index_get_time(pindex)) /
                                 86400.0 * data->fTransactionsPerDay;
        fWorkBefore = nCheapBefore + nExpensiveBefore * fSigcheckFactor;
        fWorkAfter = nExpensiveAfter * fSigcheckFactor;
    }

    return fWorkBefore / (fWorkBefore + fWorkAfter);
}

/* ── Enforcement helpers ───────────────────────────────────
 *
 * The active checkpoint list is tiny (≤10 entries on mainnet)
 * so linear scans are fine here. If that ever grows, convert
 * to a binary search keyed on `height`.
 */

bool checkpoints_hash_at_height(const struct checkpoint_data *data,
                                 int height,
                                 struct uint256 *out_hash)
{
    if (!data || !out_hash)
        LOG_FAIL("checkpoints", "hash_at_height: NULL argument (data=%p, out_hash=%p)",
                 (const void *)data, (const void *)out_hash);
    for (int i = 0; i < data->nEntries; i++) {
        if (data->entries[i].height == height) {
            *out_hash = data->entries[i].hash;
            return true;
        }
    }
    /* No checkpoint at this height — this is normal for 99.99% of heights.
     * Only checkpoint violations (hash mismatch) are worth logging. */
    return false;
}

int checkpoints_last_height(const struct checkpoint_data *data)
{
    if (!data || data->nEntries == 0)
        LOG_ERR("checkpoints", "last_height: no checkpoint data (data=%p)", (const void *)data);
    /* The list is ascending-height in practice (see
     * chainparams.c), so the final entry is the deepest.
     * Defensive scan in case that ever gets shuffled. */
    int best = -1;
    for (int i = 0; i < data->nEntries; i++) {
        if (data->entries[i].height > best)
            best = data->entries[i].height;
    }
    return best;
}

bool checkpoints_validate_header(const struct checkpoint_data *data,
                                  int height,
                                  const struct uint256 *hash)
{
    if (!data || !hash) return true;  /* degenerate: nothing to check */
    struct uint256 expected;
    if (!checkpoints_hash_at_height(data, height, &expected))
        return true;  /* no checkpoint at this height */
    return uint256_cmp(hash, &expected) == 0;
}

/* ── SHA3 UTXO checkpoint ──────────────────────────────── */
/* Verified bit-for-bit against zclassicd (ZclassicCommunity/zclassic)
 * at height 3,056,758 on 2026-03-26.
 *
 * Verification method:
 *   1. Both nodes at height 3,056,758, same bestblockhash
 *   2. gettxoutsetinfo: txouts=1,354,771, total=10364138.33747381 ZCL
 *   3. Confirmed PERFECT MATCH at height 3,056,763 (zero delta)
 *   4. SHA3-256 computed over all UTXOs in (txid,vout) canonical order
 *      including full scriptPubKey data
 *
 * A new node reaching this height MUST produce the same SHA3 hash.
 * If not, its UTXO set is corrupted and cannot be trusted. */

static const struct sha3_utxo_checkpoint g_sha3_checkpoint = {
    .height = 3056758,
    .block_hash = {
        /* 000002979090fba9da6cdc140d050245c1b637480609510922662407855bd653 */
        0x53, 0xd6, 0x5b, 0x85, 0x07, 0x24, 0x66, 0x22,
        0x09, 0x51, 0x09, 0x06, 0x48, 0x37, 0xb6, 0xc1,
        0x45, 0x02, 0x05, 0x0d, 0x14, 0xdc, 0x6c, 0xda,
        0xa9, 0xfb, 0x90, 0x90, 0x97, 0x02, 0x00, 0x00,
    },
    .sha3_hash = {
        /* 00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0 */
        0x00, 0xe9, 0x5d, 0xbd, 0x54, 0xa7, 0x91, 0xa5,
        0x14, 0x33, 0xd6, 0x81, 0x27, 0xf9, 0x97, 0x5a,
        0x3b, 0x1d, 0x6f, 0x8e, 0x90, 0x02, 0xb1, 0x09,
        0x64, 0x73, 0x43, 0xba, 0x0c, 0x83, 0xc3, 0xe0,
    },
    .utxo_count = 1354771,
    .total_supply = 1036413833747381LL,  /* 10364138.33747381 ZCL */
};

const struct sha3_utxo_checkpoint *get_sha3_utxo_checkpoint(void)
{
    return &g_sha3_checkpoint;
}
