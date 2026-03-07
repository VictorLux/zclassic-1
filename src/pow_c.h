/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_POW_C_H
#define ZCL_POW_C_H

#include "arith_uint256.h"
#include "chain_c.h"
#include "consensus/params.h"
#include "uint256.h"
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

#endif
