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

/* Configure the flat-file sapling checkpoint path (P12.1). Call once
 * from boot.c with the node's datadir; the helper derives
 * `<datadir>/sapling_tree_ckpt.dat`. After this is set, the commit
 * path flushes the checkpoint every `SAPLING_CHECKPOINT_BLOCK_INTERVAL`
 * blocks so crash-recovery replays ≤ that many blocks instead of the
 * full 2.6M-block Sapling history. Passing NULL disables the
 * checkpoint (used by unit tests). */
void set_sapling_checkpoint_datadir(const char *datadir);

bool test_block_validity(struct validation_state *state,
                         const struct chain_params *params,
                         struct coins_view_cache *coins_tip,
                         const struct block *block,
                         struct block_index *pindex_prev);

/* P7.1 test-only surface: drives update_tip directly so a unit test
 * can verify csr_commit_tip rejection propagates to the caller.
 * Returns false if the csr refused the commit; returns true if the
 * tip was advanced (or cleared, when pindex_new == NULL). Do NOT
 * call from production code — go through connect_tip / disconnect_tip. */
bool process_block_test_update_tip(struct main_state *ms,
                                    struct block_index *pindex_new);

/* P14.7 test-only surface: drives the stale-FAILED-mark clear logic
 * that accept_block_header uses when a header re-arrives for an
 * existing pindex. Caller owns last_retry_clear — passing 0 forces
 * a fresh rate-limit window. Returns true if nStatus was modified.
 *
 * Rules:
 *  - if pindex is near tip (height >= tip_h - 100): clear FAILED_MASK
 *  - else if ONLY FAILED_CHILD is set (no FAILED_VALID): clear
 *    FAILED_CHILD without rate-limit — propagation marks are stale
 *    once their root FAILED_VALID is gone, and clearing does not
 *    trigger re-validation
 *  - else (FAILED_VALID present, far from tip): rate-limited clear
 *    of FAILED_MASK once per 300s */
bool process_block_try_clear_stale_failed(struct block_index *pindex,
                                           int tip_h,
                                           time_t now,
                                           time_t *last_retry_clear);

/* P14.6: result codes for process_block_propagate_failed_child. Values
 * are stable and tested directly; add new codes at the end. */
enum propagate_failed_child_result {
    PROPAGATE_FAILED_CHILD_OK                 =  0, /* walk ran; propagated_out set */
    PROPAGATE_FAILED_CHILD_SKIP_PARENT_FAILED =  1, /* OOM guard (see below) */
    PROPAGATE_FAILED_CHILD_SKIP_RATE_LIMITED  =  2, /* OOM guard (see below) */
    PROPAGATE_FAILED_CHILD_MALLOC_FAILED      = -1, /* allocator returned NULL */
};

/* P14.6: minimum wall-clock interval between full propagation walks
 * when the caller opts into rate-limiting (non-NULL last_propagate_sec).
 * At a live-tip block_map size of ~3M entries, each walk is ~24 MB of
 * scratch + an O(N log N) qsort; firing once per FSM flap event can
 * pin the node under sustained RSS + CPU pressure (see
 * docs/postmortems/2026-04-19-bip30-stall.md). Ten seconds lets
 * genuine back-to-back validation failures still propagate without
 * amplifying a stall into resource exhaustion. */
#define PROPAGATE_FAILED_CHILD_MIN_INTERVAL_SEC 10

/* P14.6 test-only surface: propagate BLOCK_FAILED_CHILD from a failed
 * `pindex_root` through all descendants recorded in `map`. Caller
 * MUST have set a BLOCK_FAILED_MASK bit on pindex_root itself before
 * invoking.
 *
 * Guards (prevent the 2026-04-19 OOM amplifier):
 *   - SKIP_PARENT_FAILED when pindex_root->pprev is itself already in
 *     BLOCK_FAILED_MASK. The prior propagation from the ancestor
 *     already covered this subtree; re-walking the block_map would
 *     burn ~24 MB + O(N log N) to accomplish nothing.
 *   - SKIP_RATE_LIMITED when last_propagate_sec is non-NULL AND
 *     now_sec - *last_propagate_sec < PROPAGATE_FAILED_CHILD_MIN_INTERVAL_SEC.
 *     Callers that need an unconditional walk (tests, explicit flush
 *     paths) pass last_propagate_sec=NULL. On OK return with a non-NULL
 *     pointer, *last_propagate_sec is updated to now_sec.
 *
 * On OK return, *propagated_out (may be NULL) receives the count of
 * descendants newly marked; unchanged on SKIP or MALLOC_FAILED. */
enum propagate_failed_child_result
process_block_propagate_failed_child(struct block_map *map,
                                      const struct block_index *pindex_root,
                                      time_t now_sec,
                                      time_t *last_propagate_sec,
                                      size_t *propagated_out);

#endif
