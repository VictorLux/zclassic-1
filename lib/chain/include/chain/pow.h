/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_POW_H
#define ZCL_POW_H

#include "core/arith_uint256.h"
#include "chain/chain.h"
#include "consensus/params.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

unsigned int IncreaseDifficultyBy(unsigned int nBits, int64_t multiplier,
                                  const struct consensus_params *params);

unsigned int GetNextWorkRequired(const struct block_index *pindexLast,
                                 const struct block_header *pblock,
                                 const struct consensus_params *params);

unsigned int CalculateNextWorkRequired(struct arith_uint256 bnAvg,
                                       int64_t nLastBlockTime,
                                       int64_t nFirstBlockTime,
                                       const struct consensus_params *params,
                                       int nextHeight);

bool CheckProofOfWork(struct uint256 hash, unsigned int nBits,
                      const struct consensus_params *params);

struct arith_uint256 GetBlockProof(const struct block_index *block);

int64_t GetBlockProofEquivalentTime(const struct block_index *to,
                                    const struct block_index *from,
                                    const struct block_index *tip,
                                    const struct consensus_params *params);

/* Human-readable ZClassic difficulty from compact nBits representation.
 *
 * ZClassic inherited Zcash's Equihash difficulty baseline: the reference
 * target mantissa is 0x07ffff, not Bitcoin's 0x00ffff.  Keeping this
 * centralized avoids RPC/explorer surfaces silently drifting from
 * legacy zclassicd for the same nBits value.
 */
static inline double difficulty_from_bits(uint32_t bits)
{
    if (bits == 0) return 1.0;
    int shift = (int)((bits >> 24) & 0xff) - 29;
    double diff = (double)0x0007ffff / (double)(bits & 0x00ffffff);
    while (shift < 0) { diff /= 256.0; shift++; }
    while (shift > 0) { diff *= 256.0; shift--; }
    return diff;
}

/* Difficulty for a block index, NULL-safe (returns 1.0 for a NULL/genesis
 * baseline). Single source for the RPC + explorer "difficulty" surfaces,
 * replacing the per-controller get_difficulty / explorer_get_difficulty
 * duplicates. */
static inline double difficulty_from_index(const struct block_index *bi)
{
    return bi ? difficulty_from_bits(bi->nBits) : 1.0;
}

#endif
