/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_CONNECT_BLOCK_H
#define ZCL_VALIDATION_CONNECT_BLOCK_H

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "coins/undo.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "core/uint256.h"
#include <stdbool.h>

/* Assumevalid: skip expensive verification below a known-good height.
 * PoW, merkle roots, and UTXO consistency are always checked.
 * Only script and Sapling proof verification are skipped. */
void set_assume_valid(const struct uint256 *hash, int height);
int get_assume_valid_height(void);

bool connect_block(const struct block *block,
                   struct validation_state *state,
                   struct block_index *pindex,
                   struct coins_view_cache *view,
                   const struct chain_params *params,
                   bool just_check);

bool disconnect_block(const struct block *block,
                      struct validation_state *state,
                      struct block_index *pindex,
                      struct coins_view_cache *view,
                      const struct block_undo *blockundo);

#endif
