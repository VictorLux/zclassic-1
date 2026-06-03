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
#include "storage/utxo_projection.h"
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
#include "net/snapshot_sync_contract.h"
#include "services/bg_validation_service.h"
#include "services/bg_hash_verification_service.h"
#include "services/block_index_loader.h"
#include "services/chain_state_validator.h"
#include "kernel/service_kernel.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <sqlite3.h>

/* All boot globals remain static in boot.c to avoid LTO conflicts
 * with pointer aliases in wallet_helpers.c. Functions in boot_index.c
 * and boot_services.c receive what they need via parameters. */

/* ── boot_index.c ───────────────────────────────────────────── */

/* Block index load/save moved to services/block_index_loader.{h,c}.
 * Coins/chain validation moved to services/chain_state_validator.{h,c}.
 * Remaining boot_index.c functions: chainstate rebuild, reindex,
 * address backfill, block file scanning. */

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

/* Propagate nChainTx and nChainWork for all blocks in the index.
 * Call after scan_block_files_mark_data or any operation that sets
 * BLOCK_HAVE_DATA. Returns count of blocks updated. */
int propagate_nchaintx(struct main_state *ms);

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
    const struct app_context *app_ctx;
    const struct chain_params *params;
    pthread_t params_thread;
    bool params_thread_started;
    _Atomic bool *params_loaded;
    bool block_tree_open;
    struct block_tree_db *block_tree;
    struct zcl_service_kernel service_kernel;
    struct zcl_service_kernel network_kernel;
    struct zcl_service_kernel runtime_kernel;
    struct zcl_service_kernel frontend_kernel;
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
    pthread_t hodl_history_thread;
    bool hodl_history_thread_started;
    bool hodl_history_thread_stop;
    pthread_t projection_backfill_thread;
    bool projection_backfill_thread_started;
    bool projection_backfill_thread_stop;
    struct snapshot_tx_index_job tx_index_job;
    bool want_address_backfill;
    bool want_snapshot_tx_index;
    bool defer_payment_service;
    bool defer_offer_service;
    struct bg_validation_service bg_validation;
    struct bg_hash_verification_service bg_hash_verify;
    /* sync_watchdog now uses the lib/health periodic ring instead of
     * its own pthread_t — see sync_watchdog_start()/stop(). */
};

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc);
void boot_stop_db_service_kernel(void);

/* Wire the process_block tip-publication hooks + gap-fill kick (defined in
 * boot_tip_hooks.c) into the validation engine. Called once from
 * app_init_services. The teardown counterpart stays inline in app_shutdown_svc
 * since it passes NULLs and references no moved symbol. */
void boot_register_process_block_hooks(struct boot_svc_ctx *svc);

/* Idempotent open of the append-only event_log + utxo_projection (the read
 * authority for the UTXO projection path). Called early from app_init so
 * the coins_tip read view can bind to utxo_projection_get_global() before
 * app_init_services runs, then again (no-op reuse) from the phase-4 fan-out.
 * Returns the published projection or NULL. */
utxo_projection_t *boot_ensure_log_and_utxo_projection(const char *datadir);

/* Idempotent open of the block_index_projection (log-derived source for the
 * event-log boot rebuild). Hoisted so boot.c can open + publish + catch
 * up before the block-index load; the phase-4 fan-out call is a no-op reuse.
 * Requires the event log already published. Returns the projection or NULL. */
struct block_index_projection;
struct block_index_projection *boot_ensure_block_index_projection(
    const char *datadir);

/* Shutdown phase order:
 * 1. stop externally visible services
 * 2. persist fast-restart state
 * 3. quiesce network and flush chainstate
 * 4. persist runtime-owned stores
 * 5. clear runtime registry and free owned resources
 */
void app_shutdown_svc(struct boot_svc_ctx *svc);

#endif
