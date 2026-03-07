/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_CHECK_BLOCK_H
#define ZCL_VALIDATION_CHECK_BLOCK_H

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include <stdbool.h>

#define MIN_BLOCK_VERSION 4

bool check_block_header(const struct block_header *header,
                        struct validation_state *state,
                        const struct chain_params *params,
                        bool check_pow);

bool check_block(const struct block *block,
                 struct validation_state *state,
                 const struct chain_params *params,
                 bool check_pow,
                 bool check_merkle_root,
                 bool check_size_limits);

bool contextual_check_block_header(const struct block_header *header,
                                   struct validation_state *state,
                                   const struct chain_params *params,
                                   const struct block_index *pindex_prev,
                                   bool checkpoints_enabled);

bool contextual_check_block(const struct block *block,
                            struct validation_state *state,
                            const struct chain_params *params,
                            const struct block_index *pindex_prev);

#endif
