/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_PROCESS_BLOCK_H
#define ZCL_VALIDATION_PROCESS_BLOCK_H

#include "validation/main_state.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include <stdbool.h>

#define MIN_BLOCKS_TO_KEEP 288

bool accept_block_header(const struct block_header *header,
                         struct validation_state *state,
                         struct main_state *ms,
                         const struct chain_params *params,
                         struct block_index **ppindex);

bool accept_block(struct block *block,
                  struct validation_state *state,
                  struct main_state *ms,
                  const struct chain_params *params,
                  struct block_index **ppindex,
                  bool requested,
                  const char *datadir);

bool connect_tip(struct validation_state *state,
                 struct main_state *ms,
                 struct coins_view_cache *coins_tip,
                 struct block_index *pindex_new,
                 struct block *pblock,
                 const struct chain_params *params,
                 const char *datadir);

bool disconnect_tip(struct validation_state *state,
                    struct main_state *ms,
                    struct coins_view_cache *coins_tip,
                    const char *datadir);

bool activate_best_chain(struct validation_state *state,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         struct block *pblock,
                         const char *datadir);

bool process_new_block(struct validation_state *state,
                       struct main_state *ms,
                       struct coins_view_cache *coins_tip,
                       const struct chain_params *params,
                       struct block *pblock,
                       bool force_processing,
                       const char *datadir);

/* Configure the coins flush policy (short-term → long-term layer bridge).
 * block_interval=0 disables block-based flushing (default).
 * During IBD, set block_interval=1000 for aggressive batching. */
void set_flush_policy(int64_t interval_secs, size_t max_entries,
                      int block_interval);

/* Set the SQLite handle for UTXO commitment persistence.
 * Must be called before any blocks are processed. */
struct coins_view_sqlite;
void set_coins_sqlite_for_commitment(struct coins_view_sqlite *cvs);
void set_sapling_tree_for_flush(struct incremental_merkle_tree *tree);

bool test_block_validity(struct validation_state *state,
                         const struct chain_params *params,
                         struct coins_view_cache *coins_tip,
                         const struct block *block,
                         struct block_index *pindex_prev);

#endif
