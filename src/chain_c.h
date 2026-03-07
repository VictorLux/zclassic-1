/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHAIN_C_H
#define ZCL_CHAIN_C_H

#include "arith_uint256.h"
#include "primitives/block_c.h"
#include "uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SPROUT_VALUE_VERSION 1001400
#define SAPLING_VALUE_VERSION 1010100

enum block_status {
    BLOCK_VALID_UNKNOWN      = 0,
    BLOCK_VALID_HEADER       = 1,
    BLOCK_VALID_TREE         = 2,
    BLOCK_VALID_TRANSACTIONS = 3,
    BLOCK_VALID_CHAIN        = 4,
    BLOCK_VALID_SCRIPTS      = 5,
    BLOCK_VALID_MASK         = 7,
    BLOCK_HAVE_DATA          = 8,
    BLOCK_HAVE_UNDO          = 16,
    BLOCK_HAVE_MASK          = 24,
    BLOCK_FAILED_VALID       = 32,
    BLOCK_FAILED_CHILD       = 64,
    BLOCK_FAILED_MASK        = 96,
    BLOCK_ACTIVATES_UPGRADE  = 128,
    BLOCK_PARKED_FLAG        = 256,
    BLOCK_PARKED_PARENT_FLAG = 512,
    BLOCK_PARKED_MASK        = 768,
};

#define BLOCK_VALID_CONSENSUS BLOCK_VALID_SCRIPTS

struct disk_block_pos {
    int nFile;
    unsigned int nPos;
};

static inline void disk_block_pos_init(struct disk_block_pos *p)
{
    p->nFile = -1;
    p->nPos = 0;
}

#define OPTIONAL_NONE (-1)

struct block_index {
    const struct uint256 *phashBlock;
    struct block_index *pprev;
    struct block_index *pskip;
    int nHeight;
    int nFile;
    unsigned int nDataPos;
    unsigned int nUndoPos;
    struct arith_uint256 nChainWork;
    unsigned int nTx;
    unsigned int nChainTx;
    unsigned int nStatus;
    int64_t nCachedBranchId;
    struct uint256 hashSproutAnchor;
    struct uint256 hashFinalSproutRoot;
    int64_t nSproutValue;
    bool has_sprout_value;
    int64_t nChainSproutValue;
    bool has_chain_sprout_value;
    int64_t nSaplingValue;
    int64_t nChainSaplingValue;
    bool has_chain_sapling_value;

    int32_t nVersion;
    struct uint256 hashMerkleRoot;
    struct uint256 hashFinalSaplingRoot;
    uint32_t nTime;
    uint32_t nBits;
    struct uint256 nNonce;
    unsigned char nSolution[MAX_SOLUTION_SIZE];
    size_t nSolutionSize;

    uint32_t nSequenceId;
    uint64_t nTimeReceived;
};

static inline void block_index_init(struct block_index *bi)
{
    memset(bi, 0, sizeof(*bi));
    bi->nCachedBranchId = OPTIONAL_NONE;
    arith_uint256_set_zero(&bi->nChainWork);
}

static inline int64_t block_index_get_time(const struct block_index *bi)
{
    return (int64_t)bi->nTime;
}

#define MEDIAN_TIME_SPAN 11

static inline int64_t block_index_get_median_time_past(const struct block_index *bi)
{
    int64_t pmedian[MEDIAN_TIME_SPAN];
    int count = 0;
    const struct block_index *p = bi;
    for (int i = 0; i < MEDIAN_TIME_SPAN && p; i++, p = p->pprev)
        pmedian[count++] = block_index_get_time(p);

    /* Simple insertion sort */
    for (int i = 1; i < count; i++) {
        int64_t key = pmedian[i];
        int j = i - 1;
        while (j >= 0 && pmedian[j] > key) {
            pmedian[j + 1] = pmedian[j];
            j--;
        }
        pmedian[j + 1] = key;
    }
    return pmedian[count / 2];
}

static inline bool block_index_is_valid(const struct block_index *bi,
                                        enum block_status up_to)
{
    if (bi->nStatus & BLOCK_FAILED_MASK)
        return false;
    return (bi->nStatus & BLOCK_VALID_MASK) >= (unsigned int)up_to;
}

struct block_index *block_index_get_ancestor(struct block_index *bi, int height);
void block_index_build_skip(struct block_index *bi);

#endif
