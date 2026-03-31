/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "chain/pow.h"
#include "util/util.h"
#include <limits.h>

unsigned int IncreaseDifficultyBy(unsigned int nBits, int64_t multiplier,
                                  const struct consensus_params *params)
{
    struct arith_uint256 target;
    arith_uint256_set_compact(&target, nBits, NULL, NULL);

    struct arith_uint256 div;
    arith_uint256_set_u64(&div, (uint64_t)multiplier);
    struct arith_uint256 result;
    arith_uint256_div(&result, &target, &div);
    target = result;

    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &params->powLimit);
    if (arith_uint256_compare(&target, &pow_limit) > 0)
        target = pow_limit;

    return arith_uint256_get_compact(&target, false);
}

unsigned int GetNextWorkRequired(const struct block_index *pindexLast,
                                 const struct block_header *pblock,
                                 const struct consensus_params *params)
{
    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &params->powLimit);
    unsigned int nProofOfWorkLimit = arith_uint256_get_compact(&pow_limit, false);

    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    int nHeight = pindexLast->nHeight + 1;

    if (params->scaleDifficultyAtUpgradeFork &&
        ((nHeight >= params->vUpgrades[UPGRADE_DIFFADJ].nActivationHeight &&
          nHeight < params->vUpgrades[UPGRADE_DIFFADJ].nActivationHeight + params->nPowAveragingWindow) ||
         (nHeight >= params->vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight &&
          nHeight < params->vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight + params->nPowAveragingWindow))) {

        int64_t spacing = consensus_pow_target_spacing(params, nHeight);
        if (pblock && block_header_get_time(pblock) >
            block_index_get_time(pindexLast) + spacing * 12) {
            return nProofOfWorkLimit;
        } else if (pblock && block_header_get_time(pblock) >
                   block_index_get_time(pindexLast) + spacing * 6) {
            return IncreaseDifficultyBy(nProofOfWorkLimit, 128, params);
        } else if (pblock && block_header_get_time(pblock) >
                   block_index_get_time(pindexLast) + spacing * 2) {
            return IncreaseDifficultyBy(nProofOfWorkLimit, 256, params);
        }
    }

    if (params->nPowAllowMinDifficultyEnabled &&
        pindexLast->nHeight >= params->nPowAllowMinDifficultyBlocksAfterHeight) {
        int64_t spacing = consensus_pow_target_spacing(params, pindexLast->nHeight + 1);
        if (pblock && block_header_get_time(pblock) >
            block_index_get_time(pindexLast) + spacing * 6)
            return nProofOfWorkLimit;
    }

    /* Validate averaging window is positive — zero would cause division by zero */
    if (params->nPowAveragingWindow <= 0)
        return nProofOfWorkLimit;

    const struct block_index *pindexFirst = pindexLast;
    struct arith_uint256 bnTot;
    arith_uint256_set_zero(&bnTot);
    for (int i = 0; pindexFirst && i < params->nPowAveragingWindow; i++) {
        struct arith_uint256 bnTmp;
        arith_uint256_set_compact(&bnTmp, pindexFirst->nBits, NULL, NULL);
        struct arith_uint256 sum;
        arith_uint256_add(&sum, &bnTot, &bnTmp);
        bnTot = sum;
        pindexFirst = pindexFirst->pprev;
    }

    if (pindexFirst == NULL)
        return nProofOfWorkLimit;

    struct arith_uint256 bnAvg;
    struct arith_uint256 window;
    arith_uint256_set_u64(&window, (uint64_t)params->nPowAveragingWindow);
    arith_uint256_div(&bnAvg, &bnTot, &window);

    return CalculateNextWorkRequired(bnAvg,
        block_index_get_median_time_past(pindexLast),
        block_index_get_median_time_past(pindexFirst),
        params, pindexLast->nHeight + 1);
}

unsigned int CalculateNextWorkRequired(struct arith_uint256 bnAvg,
                                       int64_t nLastBlockTime,
                                       int64_t nFirstBlockTime,
                                       const struct consensus_params *params,
                                       int nextHeight)
{
    int64_t avgTimespan = consensus_averaging_window_timespan(params, nextHeight);
    int64_t minTimespan = consensus_min_actual_timespan(params, nextHeight);
    int64_t maxTimespan = consensus_max_actual_timespan(params, nextHeight);

    int64_t nActualTimespan = nLastBlockTime - nFirstBlockTime;
    nActualTimespan = avgTimespan + (nActualTimespan - avgTimespan) / 4;

    if (nActualTimespan < minTimespan)
        nActualTimespan = minTimespan;
    if (nActualTimespan > maxTimespan)
        nActualTimespan = maxTimespan;

    struct arith_uint256 bnPowLimit;
    uint256_to_arith(&bnPowLimit, &params->powLimit);

    struct arith_uint256 avgTs, actTs;
    arith_uint256_set_u64(&avgTs, (uint64_t)avgTimespan);
    arith_uint256_set_u64(&actTs, (uint64_t)nActualTimespan);

    struct arith_uint256 bnNew;
    arith_uint256_div(&bnNew, &bnAvg, &avgTs);
    struct arith_uint256 bnResult;
    arith_uint256_mul(&bnResult, &bnNew, &actTs);
    bnNew = bnResult;

    if (arith_uint256_compare(&bnNew, &bnPowLimit) > 0)
        bnNew = bnPowLimit;

    return arith_uint256_get_compact(&bnNew, false);
}

bool CheckProofOfWork(struct uint256 hash, unsigned int nBits,
                      const struct consensus_params *params)
{
    bool fNegative = false;
    bool fOverflow = false;
    struct arith_uint256 bnTarget;
    arith_uint256_set_compact(&bnTarget, nBits, &fNegative, &fOverflow);

    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &params->powLimit);

    if (fNegative || arith_uint256_is_zero(&bnTarget) || fOverflow ||
        arith_uint256_compare(&bnTarget, &pow_limit) > 0) {
        LogPrintf("CheckProofOfWork(): nBits below minimum work\n");
        return false;
    }

    struct arith_uint256 hash_arith;
    uint256_to_arith(&hash_arith, &hash);
    if (arith_uint256_compare(&hash_arith, &bnTarget) > 0) {
        LogPrintf("CheckProofOfWork(): hash doesn't match nBits\n");
        return false;
    }

    return true;
}

struct arith_uint256 GetBlockProof(const struct block_index *block)
{
    bool fNegative, fOverflow;
    struct arith_uint256 bnTarget;
    arith_uint256_set_compact(&bnTarget, block->nBits, &fNegative, &fOverflow);

    struct arith_uint256 zero;
    arith_uint256_set_zero(&zero);

    if (fNegative || fOverflow || arith_uint256_is_zero(&bnTarget))
        return zero;

    /* 2**256 / (bnTarget+1) == ~bnTarget / (bnTarget+1) + 1 */
    struct arith_uint256 notTarget;
    arith_uint256_complement(&notTarget, &bnTarget);

    struct arith_uint256 one;
    arith_uint256_set_u64(&one, 1);

    struct arith_uint256 targetPlusOne;
    arith_uint256_add(&targetPlusOne, &bnTarget, &one);

    struct arith_uint256 result;
    arith_uint256_div(&result, &notTarget, &targetPlusOne);
    arith_uint256_add(&result, &result, &one);

    return result;
}

int64_t GetBlockProofEquivalentTime(const struct block_index *to,
                                    const struct block_index *from,
                                    const struct block_index *tip,
                                    const struct consensus_params *params)
{
    struct arith_uint256 r;
    int sign = 1;
    if (arith_uint256_compare(&to->nChainWork, &from->nChainWork) > 0) {
        arith_uint256_sub(&r, &to->nChainWork, &from->nChainWork);
    } else {
        arith_uint256_sub(&r, &from->nChainWork, &to->nChainWork);
        sign = -1;
    }

    struct arith_uint256 spacing;
    arith_uint256_set_u64(&spacing,
        (uint64_t)consensus_pow_target_spacing(params, tip->nHeight));

    struct arith_uint256 tmp;
    arith_uint256_mul(&tmp, &r, &spacing);

    struct arith_uint256 proof = GetBlockProof(tip);
    arith_uint256_div(&r, &tmp, &proof);

    if (arith_uint256_bits(&r) > 63)
        return sign * INT64_MAX;

    return sign * (int64_t)arith_uint256_get_low64(&r);
}
