/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Internal header shared between boot.c, boot_index.c, boot_services.c.
 * Not for use outside config/src/. */

#ifndef ZCL_BOOT_INTERNAL_H
#define ZCL_BOOT_INTERNAL_H

#include "config/boot.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "validation/main_state.h"
#include "storage/coins_view_sqlite.h"
#include "coins/coins_view.h"
#include "validation/txmempool.h"
#include "net/connman.h"
#include "net/msgprocessor.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"
#include "mining/gen.h"
#include "metrics/metrics.h"
#include "storage/block_index_db.h"
#include "models/database.h"
#include "controllers/sync_controller.h"
#include "controllers/snapshot_controller.h"
#include "services/snapshot_sync_service.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <sqlite3.h>

/* All boot globals remain static in boot.c to avoid LTO conflicts
 * with pointer aliases in wallet_helpers.c. Functions in boot_index.c
 * and boot_services.c receive what they need via parameters. */

/* ── boot_index.c ───────────────────────────────────────────── */

void save_block_index_flat(const char *datadir, struct main_state *ms);
bool load_block_index_flat(const char *datadir, struct main_state *ms);
void save_block_index_recent(struct node_db *ndb, struct main_state *ms);
bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms);
bool load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open);
bool fast_rebuild_chainstate(struct coins_view_sqlite *cvs,
                              struct coins_view_cache *cvtip,
                              const char *datadir);
bool reindex_chainstate(struct main_state *ms,
                          struct coins_view_sqlite *cvs,
                          struct coins_view_cache *cvtip,
                          struct node_db *ndb,
                          const char *datadir);
void *backfill_addresses_thread(void *arg);

/* Scan block files (blk*.dat), parse ZClassic block headers,
 * create block_index entries for unknown blocks, mark BLOCK_HAVE_DATA,
 * set nTx, and propagate nChainTx. If params is NULL, only marks
 * blocks already in the index (no creation). */
int scan_block_files_mark_data(struct main_state *ms, const char *datadir,
                                const struct chain_params *params);

/* ── Boot-time Validation ────────────────────────────────────── */

/* ActiveRecord-style validation for coins/chain agreement at boot.
 * Detects mismatch between coins_best_block and active chain tip,
 * returns the appropriate recovery action. Emits EV_BOOT_VALIDATION_FAILED. */

enum boot_recovery_action {
    BOOT_OK = 0,               /* coins and chain agree */
    BOOT_RECOVER_REIMPORT,     /* LevelDB chainstate exists, reimport */
    BOOT_RECOVER_WIPE_WAIT,    /* wipe UTXOs, wait for P2P snapshot */
    BOOT_RECOVER_RESET_CHAIN,  /* coins behind chain, reset chain tip */
};

struct boot_validation_result {
    enum boot_recovery_action action;
    int chain_height;
    int coins_height;      /* -1 if coins_best_block not found in index */
    struct uint256 coins_hash;
};

struct boot_validation_result validate_coins_chain_agreement(
    struct main_state *ms,
    struct coins_view_cache *cvtip,
    const char *datadir);

/* ── boot_services.c ────────────────────────────────────────── */

struct boot_svc_ctx {
    struct main_state *state;
    struct coins_view_sqlite *coins_sqlite;
    struct coins_view_cache *coins_tip;
    struct tx_mempool *mempool;
    struct rpc_table *rpc_table;
    struct msg_processor *msg_processor;
    struct connman *connman;
    struct wallet *wallet;
    struct gen_context *gen;
    struct wallet_sqlite *wallet_sqlite;
    struct node_db *node_db;
    struct db_service *db_service;
    struct metrics_context *metrics;
    _Atomic bool *running;
    const char *datadir;
    pthread_t params_thread;
    bool params_thread_started;
    _Atomic bool *params_loaded;
    bool block_tree_open;
    struct block_tree_db *block_tree;
    /* Composition-owned runtime passed into long-lived services. */
    struct app_runtime_context runtime;
    struct snapshot_sync_service snapshot_sync;
    struct node_db_sync_catchup_job catchup_job;
    pthread_t payment_thread;
    bool payment_thread_started;
    pthread_t replay_thread;
    bool replay_thread_started;
    pthread_t offer_thread;
    bool offer_thread_started;
    pthread_t address_backfill_thread;
    bool address_backfill_thread_started;
    struct snapshot_tx_index_job tx_index_job;
    bool want_address_backfill;
    bool want_snapshot_tx_index;
};

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc);

/* Shutdown phase order:
 * 1. stop externally visible services
 * 2. persist fast-restart state
 * 3. quiesce network and flush chainstate
 * 4. persist runtime-owned stores
 * 5. clear runtime registry and free owned resources
 */
void app_shutdown_svc(struct boot_svc_ctx *svc);

#endif
