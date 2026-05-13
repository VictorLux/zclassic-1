/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal helpers exported from lib/validation/src/process_block.c for
 * the chain_advance atomic protocol in app/services/src/chain_advance.c.
 *
 * Not for general use. These are the file-static helpers connect_tip
 * used to call directly; chain_advance now composes them in the
 * explicit 9-step protocol described in services/chain_advance.h. */

#ifndef ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H
#define ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H

#include <stdbool.h>

struct main_state;
struct coins_view_cache;
struct block_index;
struct coins_view_sqlite;
struct block_tree_db;
struct node_db;

/* Accessors for file-static globals/handles. */
struct coins_view_sqlite *process_block_get_coins_sqlite(void);
struct block_tree_db *process_block_get_block_tree(void);
struct node_db *process_block_get_node_db(void);

/* csr_commit_tip wrapper used internally by update_tip + the
 * chain_advance protocol. Returns true on CSR_OK or the test-harness
 * fallback path; false on any real CSR rejection (caller must abort
 * the in-flight chain advance). */
bool process_block_commit_tip_ext(struct main_state *ms,
                                  struct coins_view_cache *coins_tip,
                                  struct block_index *new_tip,
                                  const char *reason,
                                  bool update_header_tip);

/* Force a coins flush (force=true) or a policy-gated flush
 * (force=false). Returns false on a hard flush failure that the
 * caller must surface. */
bool process_block_flush_coins(struct coins_view_cache *coins_tip,
                               bool force);

/* Persist the Sapling commitment tree row through node_db's currently
 * open transaction (or autocommit if none). Best-effort; failures are
 * logged but never abort the chain advance. */
bool process_block_persist_sapling_tree(void);

/* Per-block stage logger forwarded for chain_advance's tracing. */
void process_block_log_live_stage_ext(int height, const char *stage,
                                      long long elapsed_us);

/* Test crash-injection check. Inlined in process_block.c via
 * process_block_check_crash_stage; chain_advance.c calls this
 * out-of-line variant. */
#include "validation/process_block.h"
void process_block_check_crash_stage_ext(enum process_block_crash_stage here);

#endif /* ZCL_VALIDATION_PROCESS_BLOCK_INTERNALS_H */
