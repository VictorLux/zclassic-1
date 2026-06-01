/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal declarations shared across the process_block_* translation
 * units (process_block.c, process_block_core.c,
 * process_block_self_heal.c, process_block_flush_policy.c,
 * process_block_crash_hooks.c). Not intended for use outside this
 * directory; the public surface lives in
 * <validation/process_block.h>. */

#ifndef ZCL_VALIDATION_PROCESS_BLOCK_INTERNAL_H
#define ZCL_VALIDATION_PROCESS_BLOCK_INTERNAL_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "validation/process_block.h"
#include "validation/process_block_internals.h"

struct main_state;
struct coins_view_cache;
struct block_index;
struct transaction;
struct uint256;
struct validation_state;
struct coins_view_sqlite;
struct incremental_merkle_tree;
struct block_tree_db;
struct node_db;
struct wallet;
struct tx_mempool;

/* ── Defaults / tunables ─────────────────────────────────────── */
#define SELF_HEAL_SCAN_DEFAULT_DEPTH 250000
#define SELF_HEAL_MAX_RECOVERY_ATTEMPTS 100
#define ACTIVE_TIP_CHILD_CONNECT_DEFAULT_LIMIT 128

/* ── Shared file-scope globals (own definitions in named .c file)
 * Exposed here as extern so the split modules can read them
 * without going through getter calls in hot paths. */

/* Owner: process_block.c */
extern struct block_tree_db *g_active_block_tree;       /* defined in boot.c */
extern volatile sig_atomic_t g_shutdown_requested;      /* defined in boot.c */
extern _Atomic int g_body_pull_active;                  /* public in process_block.h */

/* Owner: process_block_crash_hooks.c */
extern _Atomic int g_test_crash_stage_storage;

/* Owner: process_block_self_heal.c */
extern int s_utxo_fail_count;
extern int s_utxo_fail_height;
extern int s_utxo_hot_loop_reported_height;
extern int s_utxo_activation_paused_height;
extern _Atomic uint64_t g_self_heal_tx_index_hits;
extern _Atomic uint64_t g_self_heal_scan_hits;
extern _Atomic uint64_t g_self_heal_scan_exhausted;
extern _Atomic uint64_t g_self_heal_scan_blocks_checked_total;

/* ── Internal helpers exposed across split files ───────────── */

/* process_block.c */
struct node_db *process_block_node_db_internal(void);
bool process_block_live_height(int height);
void process_block_log_live_stage(int height, const char *stage,
                                  int64_t elapsed_us);
int active_tip_child_connect_limit(void);
struct wallet *process_block_wallet(void);
struct tx_mempool *process_block_mempool(void);
/* "more pending" signal set/read by activate_best_chain and the public
 * process_block_active_tip_has_pending API. */
void process_block_set_active_tip_more_pending(bool v);

/* process_block_crash_hooks.c */
/* Hot-path stage check. Kept inline so the cost is one atomic load per
 * site; the static inline reads the storage variable owned by
 * process_block_crash_hooks.c. */
static inline void process_block_check_crash_stage(
    enum process_block_crash_stage here)
{
    enum process_block_crash_stage armed =
        (enum process_block_crash_stage)atomic_load_explicit(
            &g_test_crash_stage_storage, memory_order_relaxed);
    if (__builtin_expect(armed == here, 0)) {
        fprintf(stderr,
                "[crash-test] connect_tip: _exit(137) at stage=%s\n",
                process_block_crash_stage_name(here));
        fflush(stderr);
        _exit(137);
    }
}

/* process_block_self_heal.c */
bool process_block_inject_missing_utxo(
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const struct transaction *tx,
    int height,
    const char *source,
    int retry_no);

bool process_block_recover_missing_utxo_from_legacy_rpc(
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    int retry_no);

bool process_block_recover_missing_utxo_from_sqlite_tx_index(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t missing_vout,
    const char *datadir,
    int retry_no);

/* Bounded backward chain scan: walk the active chain from tip down a
 * bounded depth searching each on-disk block for `txid`, inject its
 * outputs into the coins cache, and backfill the LevelDB tx_index.
 * Accounts hits/exhaustion to the g_self_heal_scan_* counters. */
bool process_block_recover_missing_utxo_from_chain_scan(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    const struct uint256 *txid,
    uint32_t vout,
    const char *datadir,
    int retry_no);

bool process_block_is_missing_utxo_failure(
    const struct validation_state *state);

void process_block_note_utxo_failure(struct main_state *ms,
                                     struct coins_view_cache *coins_tip,
                                     int height,
                                     const char *datadir);

/* process_block_flush_policy.c */
struct coins_view_sqlite *process_block_coins_sqlite_ptr(void);
bool flush_coins_if_needed(struct coins_view_cache *coins_tip, bool force);
void sapling_checkpoint_maybe_flush(int height);
bool sapling_tree_persist_once(void);

/* process_block_core.c helpers exposed to sibling .c files in the
 * WS-6 phase 1 split. add_to_block_index used by accept_block_header.c.
 * update_tip used by disconnect_tip.c.
 * find_block_pos + block_index_refresh_header used by accept_block.c.
 * g_last_block_file_size updated by accept_block.c after on-disk write.
 * process_block_should_skip_contextual_header is already declared in
 * <validation/process_block.h> (public). */
struct block_header;
struct disk_block_pos;
struct block_index *add_to_block_index(struct main_state *ms,
                                       const struct block_header *header);
bool update_tip(struct main_state *ms, struct block_index *pindex_new);
bool find_block_pos(struct disk_block_pos *pos, unsigned int block_size,
                    const char *datadir);
void block_index_refresh_header(struct block_index *pindex,
                                const struct block_header *header);
extern unsigned int g_last_block_file_size; /* defined in process_block_core.c */

/* WS-6.4: helpers exposed for connect_tip.c. block_index_hydrate_from_disk
 * is also called by process_block_test_hydrate_index_from_disk in core.c.
 * find_most_work_chain + process_block_commit_tip are called by
 * activate_best_chain (still in core.c) as well. */
struct coins_view_cache;
bool block_index_hydrate_from_disk(struct block_index *pindex,
                                   const char *datadir);
struct block_index *find_most_work_chain(struct main_state *ms);
bool process_block_commit_tip(struct main_state *ms,
                              struct coins_view_cache *coins_tip,
                              struct block_index *new_tip,
                              const char *reason,
                              bool update_header_tip,
                              bool persist_coins_best,
                              const struct process_block_tip_evidence *verified);

/* WS-6.5: helpers exposed for activate_best_chain.c. */
bool process_block_verify_active_tip_child_on_disk(
    const struct block_index *candidate,
    const struct block_index *tip,
    const char *datadir);
struct block_index *find_best_active_tip_child(struct main_state *ms,
                                               struct block_index *tip,
                                               const char *datadir);
struct block_index *find_verified_unlinked_active_tip_child(
    struct main_state *ms,
    struct block_index *tip,
    const char *datadir);

#endif /* ZCL_VALIDATION_PROCESS_BLOCK_INTERNAL_H */
