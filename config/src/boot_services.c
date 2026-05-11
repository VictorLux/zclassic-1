/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Runtime service initialization: mempool, P2P, RPC, Tor, HTTPS,
 * mining, wallet sync, shutdown, and utility functions. */

#include "config/boot_internal.h"
#include "services/chain_activation_controller.h"
#include "services/chain_tip.h"
#include "services/gap_fill_service.h"
#include "storage/disk_block_io.h"
#include "models/utxo.h"
#include "models/mmb_leaf_store.h"
#include "chain/chainparams.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "chain/subsidy.h"
#include "coins/coins_view.h"
#include "controllers/blockchain_controller.h"
#include "controllers/hodl_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/network_controller.h"
#include "controllers/mining_controller.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include "controllers/transaction_controller.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_controller.h"
#include "controllers/wallet_controller.h"
#include "controllers/zslp_controller.h"
#include "controllers/sync_controller.h"
#include "controllers/event_controller.h"
#include "controllers/snapshot_controller.h"
#include "controllers/game_controller.h"
#include "controllers/health_controller.h"
#include "controllers/file_market_controller.h"
#include "controllers/name_controller.h"
#include "controllers/messaging_controller.h"
#include "controllers/swap_controller.h"
#include "rpc/httpserver.h"
#include "rpc/server.h"
#include "net/https_server.h"
#include "net/fast_sync.h"
#include "net/peer_strategy.h"
#include "net/tor_integration.h"
#include "validation/process_block.h"
#include "event/event.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "sapling/params_init.h"
#include <netdb.h>

/* msg_version.c — external IP advertisement to peers */
extern void msg_version_set_external_ip(const char *ip_str, uint16_t port);
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include "services/mempool_limits.h"
#include "services/wallet_backup_service.h"
#include "services/disk_monitor.h"
#include "services/ibd_throttle.h"
#include "services/sync_watchdog_service.h"
#include "net/download.h"
#include "services/db_maintenance.h"
#include "mcp/metrics.h"

extern int g_assume_valid_height;

/* Module-local pointer to boot context (set once by app_init_services) */
static struct boot_svc_ctx *S;

static struct app_runtime_context *boot_runtime(void)
{
    if (!S)
        return NULL;
    return &S->runtime;
}

static struct node_db *boot_node_db(void)
{
    struct app_runtime_context *runtime = boot_runtime();
    if (!runtime || !runtime->db_service)
        return NULL;
    return db_service_node_db(runtime->db_service);
}

static struct db_service *boot_db_service(void)
{
    struct app_runtime_context *runtime = boot_runtime();
    if (!runtime)
        return NULL;
    return runtime->db_service;
}

static struct wallet *boot_wallet(void)
{
    struct app_runtime_context *runtime = boot_runtime();
    if (!runtime)
        return NULL;
    return runtime->wallet;
}

static void *payment_processor_thread(void *arg);
static void *background_utxo_replay(void *arg);
static void *build_snapshot_offer_thread(void *arg);
static void *address_backfill_service_thread(void *arg);

static bool boot_running(const struct boot_svc_ctx *svc)
{
    return svc && svc->running && atomic_load(svc->running);
}

static bool boot_start_thread_service(pthread_t *thread,
                                      bool *started,
                                      void *(*entry)(void *),
                                      void *arg)
{
    if (!thread || !started || !entry || *started)
        return false;
    if (pthread_create(thread, NULL, entry, arg) != 0)
        return false;
    *started = true;
    return true;
}

static void boot_join_thread_service(pthread_t *thread, bool *started)
{
    if (!thread || !started || !*started)
        return;
    pthread_join(*thread, NULL);
    *started = false;
}

static bool boot_start_catchup_service(struct boot_svc_ctx *svc,
                                       const char *datadir)
{
    if (!svc || node_db_sync_catchup_job_is_started(&svc->catchup_job))
        return false;

    return node_db_sync_catchup_job_start(&svc->catchup_job, boot_node_db(),
                                          &svc->state->chain_active,
                                          svc->wallet, datadir);
}

static void boot_join_catchup_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    node_db_sync_catchup_job_join(&svc->catchup_job, NULL);
}

static bool boot_start_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->payment_thread,
                                     &svc->payment_thread_started,
                                     payment_processor_thread, svc);
}

static void boot_join_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service(&svc->payment_thread,
                             &svc->payment_thread_started);
}

static bool boot_start_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->replay_thread,
                                     &svc->replay_thread_started,
                                     background_utxo_replay, svc);
}

static void boot_join_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service(&svc->replay_thread,
                             &svc->replay_thread_started);
}

static bool boot_start_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->offer_thread,
                                     &svc->offer_thread_started,
                                     build_snapshot_offer_thread, svc);
}

static void boot_join_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service(&svc->offer_thread,
                             &svc->offer_thread_started);
}

static bool boot_start_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir)
        return false;
    return boot_start_thread_service(&svc->address_backfill_thread,
                                     &svc->address_backfill_thread_started,
                                     address_backfill_service_thread, svc);
}

static void boot_join_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service(&svc->address_backfill_thread,
                             &svc->address_backfill_thread_started);
}

static bool boot_start_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir ||
        snapshot_tx_index_job_is_started(&svc->tx_index_job))
        return false;
    return snapshot_tx_index_job_start(&svc->tx_index_job, svc->datadir);
}

static void boot_join_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    snapshot_tx_index_job_join(&svc->tx_index_job, NULL);
}

/* ── Helper threads ────────────────────────────────────────── */

extern void store_process_payments(const char *datadir);

/* Watchdog: detect stuck chain and clear BLOCK_FAILED to allow retry. */
static void watchdog_check_stuck(struct boot_svc_ctx *svc)
{
    static int64_t last_height_change = 0;
    static int last_height = -1;

    int h = active_chain_height(&svc->state->chain_active);
    int64_t now = (int64_t)time(NULL);

    if (h != last_height) {
        last_height = h;
        last_height_change = now;
        return;
    }

    /* No progress for 5 minutes — try clearing BLOCK_FAILED on next block */
    if (last_height_change > 0 && now - last_height_change > 300 && h > 100) {
        fprintf(stderr, "WATCHDOG: stuck at h=%d for %lld seconds. "
                "Clearing BLOCK_FAILED on h=%d to retry.\n",
                h, (long long)(now - last_height_change), h + 1);

        struct block_index *next = active_chain_at(
            &svc->state->chain_active, h + 1);
        if (!next) {
            /* Not in active chain — scan block map for height h+1 */
            size_t iter = 0;
            struct block_index *bi = NULL;
            while (block_map_next(&svc->state->map_block_index,
                                   &iter, NULL, &bi)) {
                if (bi && bi->nHeight == h + 1 &&
                    (bi->nStatus & BLOCK_FAILED_MASK)) {
                    bi->nStatus &= ~BLOCK_FAILED_MASK;
                    fprintf(stderr, "WATCHDOG: cleared BLOCK_FAILED on "
                            "h=%d\n", bi->nHeight);
                }
            }
        } else if (next->nStatus & BLOCK_FAILED_MASK) {
            next->nStatus &= ~BLOCK_FAILED_MASK;
            fprintf(stderr, "WATCHDOG: cleared BLOCK_FAILED on h=%d\n",
                    next->nHeight);
        }
        last_height_change = now; /* don't spam — wait another 5 min */
    }
}

static void *payment_processor_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;

    while (boot_running(svc)) {
        for (int i = 0; i < 30 && boot_running(svc); i++)
            sleep(1);
        if (!boot_running(svc))
            break;
        store_process_payments(svc->datadir);
        watchdog_check_stuck(svc);
    }
    return NULL;
}

static void *address_backfill_service_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    char *db_path;

    if (!svc || !svc->datadir)
        return NULL;

    db_path = malloc(1024);
    if (!db_path)
        return NULL;
    snprintf(db_path, 1024, "%s/node.db", svc->datadir);
    backfill_addresses_thread(db_path);
    free(db_path);
    return NULL;
}

/* ── Global MMB leaf store for FlyClient proofs ─────────────── */
struct mmb_leaf_store g_mmb_leaf_store = {0};

/* ── Background UTXO replay ───────────────────────────────── */
/* After file sync, replay blocks to build UTXO set in background.
 * Node serves data immediately; UTXO set builds while running. */

_Atomic bool g_utxo_replay_active = false;
_Atomic int g_utxo_replay_height = 0;

static void *background_utxo_replay(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const struct chain_params *params = chain_params_get();

    if (!svc || !svc->state || !svc->coins_tip || !params || !svc->datadir)
        return NULL;

    atomic_store(&g_utxo_replay_active, true);
    int64_t t0 = (int64_t)time(NULL);

    printf("UTXO replay: starting background chain validation...\n");
    fflush(stdout);

    /* ── Restore chain state from coins_best_block ──────────────
     * After snapshot import (file or P2P), coins_best_block in SQLite
     * points to the snapshot height, but the in-memory g_coins_tip and
     * active chain are still at genesis. We must advance both so that
     * activate_best_chain starts from the snapshot height, not genesis.
     * Without this, connect_block fails at height 1 with BIP30 because
     * the snapshot's UTXOs include block 1's unspent coinbase. */
    struct node_db *ndb_restore = boot_node_db();
    if (ndb_restore && ndb_restore->open) {
        uint8_t cb_buf[32] = {0};
        size_t cb_len = 0;
        if (node_db_state_get(ndb_restore, "coins_best_block",
                              cb_buf, sizeof(cb_buf), &cb_len) && cb_len == 32) {
            struct uint256 cb_hash;
            memcpy(cb_hash.data, cb_buf, 32);
            if (!uint256_is_null(&cb_hash)) {
                struct block_index *snap_block = block_map_find(
                    &svc->state->map_block_index, &cb_hash);
                if (snap_block && snap_block->nHeight > 0) {
                    /* Set in-memory coins view best block */
                    coins_view_cache_set_best_block(svc->coins_tip, &cb_hash);
                    /* Advance active chain tip to snapshot height */
                    chain_set_active_tip(svc->state, snap_block,
                                          TIP_FROM_SNAPSHOT,
                                          "utxo_replay_snapshot_restore");
                    printf("UTXO replay: restored chain state from snapshot "
                           "at h=%d\n", snap_block->nHeight);
                } else if (!snap_block) {
                    printf("UTXO replay: coins_best_block not in index "
                           "(waiting for P2P headers)\n");
                }
            }
        }
    }

    /* IBD turbo: skip non-essential work during replay */
    struct db_service *dbsvc = boot_db_service();
    struct node_db *ndb = boot_node_db();
    if (dbsvc) {
        db_service_ibd_turbo_mode(dbsvc);
        db_service_set_sync_batch_size(dbsvc, 1000);
    } else if (ndb && ndb->open) {
        node_db_ibd_turbo_mode(ndb);
        node_db_set_sync_batch_size(ndb, 1000);
    }
    /* Flush every 500 blocks even during IBD to limit UTXO loss on
     * SIGKILL. Previous value of 100000 meant a SIGKILL during boot
     * could lose 100K blocks of UTXO state, requiring full re-sync. */
    set_flush_policy(3600, 1000000, 500);

    {
        struct activation_exec_outcome outcome;
        activation_request_connect(boot_activation_controller(),
                                   ACTIVATION_SRC_UTXO_REPLAY,
                                   NULL, &outcome);
    }

    /* Restore normal flush policy */
    set_flush_policy(3600, 500000, 500);
    if (dbsvc) {
        if (!db_service_flush_write(dbsvc))
            fprintf(stderr, "UTXO replay: flush before normal mode failed\n");
        db_service_normal_mode(dbsvc);
        db_service_set_sync_batch_size(dbsvc, 100);
    } else if (ndb && ndb->open) {
        if (!node_db_sync_flush(ndb))
            fprintf(stderr, "UTXO replay: flush before normal mode failed\n");
        node_db_normal_mode(ndb);
        node_db_set_sync_batch_size(ndb, 100);
    }

    int tip = active_chain_height(&svc->state->chain_active);
    int64_t elapsed = (int64_t)time(NULL) - t0;
    atomic_store(&g_utxo_replay_height, tip);
    atomic_store(&g_utxo_replay_active, false);

    printf("=== UTXO replay complete: tip=%d in %llds "
           "(%.0f blocks/sec) ===\n",
           tip, (long long)elapsed,
           elapsed > 0 ? (double)tip / (double)elapsed : 0);
    fflush(stdout);

    event_emitf(EV_NODE_READY, 0, "utxo_replay_done height=%d secs=%lld",
                tip, (long long)elapsed);
    return NULL;
}

extern size_t onion_service_handle_request(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t);

static size_t onion_request_adapter(const char *method, const char *path,
    const uint8_t *req_data, size_t req_len,
    uint8_t *resp, size_t resp_max, void *ctx)
{
    (void)ctx;
    return onion_service_handle_request(method, path,
        req_data, req_len, resp, resp_max);
}

static void *build_snapshot_offer_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const char *datadir = svc ? svc->datadir : NULL;

    if (!datadir || datadir[0] == '\0')
        return NULL;

    /* Export consensus snapshot for file service (no wallet data).
     * This runs first so new peers can get it immediately. */
    printf("Exporting consensus snapshot (no wallet data)...\n");
    if (file_export_consensus_snapshot(datadir)) {
        file_controller_refresh_manifest();
        fs_server_refresh_manifest();
        printf("Consensus snapshot ready for file service\n");
    }

    printf("Building fast sync snapshot offer...\n");

    struct snapshot_offer offer;
    if (fast_sync_build_offer(datadir, &offer)) {
        /* Embed MMR root — cryptographically binds UTXO snapshot to PoW chain */
        struct mmr *m = rpc_blockchain_get_mmr();
        if (m && m->num_leaves > 0) {
            mmr_root(m, offer.mmr_root);
        } else {
            memset(offer.mmr_root, 0, 32);
        }

        /* Embed MMB root — replayed from leaf hash store via
         * mmb_append_hash(), guaranteeing identical merge logic
         * as mmb_prove() uses internally. */
        if (g_mmb_leaf_store.open && g_mmb_leaf_store.num_leaves > 0) {
            const uint8_t (*lh)[32] = mmb_leaf_store_all(&g_mmb_leaf_store);
            /* Replay only up to snapshot height — the FlyClient
             * challenge uses offer.height as chain_length, so the
             * MMB root must match that exact leaf count. */
            uint64_t replay_count = (uint64_t)offer.height;
            if (replay_count > g_mmb_leaf_store.num_leaves)
                replay_count = g_mmb_leaf_store.num_leaves;
            if (lh && replay_count > 0) {
                struct mmb replay;
                mmb_init(&replay);
                for (uint64_t i = 0; i < replay_count; i++)
                    mmb_append_hash(&replay, lh[i]);
                mmb_root(&replay, offer.mmb_root);
                printf("Fast sync: MMB root at h=%d (%llu/%llu leaves, "
                       "%u peaks)\n", offer.height,
                       (unsigned long long)replay.num_leaves,
                       (unsigned long long)g_mmb_leaf_store.num_leaves,
                       replay.num_mountains);
            } else {
                memset(offer.mmb_root, 0, 32);
            }
        } else {
            memset(offer.mmb_root, 0, 32);
            printf("Fast sync: WARNING — no MMB leaf store\n");
        }
        /* Pre-serialize snapshot file for zero-copy serving.
         * MUST happen before updating offer — the SHA3 from pre-serialization
         * is used as the offer's utxo_root to guarantee hash/file match. */
        struct node_db *ndb = boot_node_db();
        if (ndb && ndb->open) {
            int64_t snap_count = fast_sync_prebuild_snapshot(
                ndb, datadir);
            if (snap_count > 0) {
                /* Use the SHA3 computed during serialization */
                uint8_t snap_sha3[32];
                uint64_t snap_n = 0;
                if (fast_sync_get_snapshot_sha3(snap_sha3, &snap_n)) {
                    memcpy(offer.utxo_root, snap_sha3, 32);
                    offer.num_utxos = snap_n;
                    printf("Snapshot: %llu UTXOs, SHA3 from file\n",
                           (unsigned long long)snap_n);
                }
            }
        }

        msg_processor_update_offer(&offer);
        printf("Fast sync ready: h=%d, %llu UTXOs, MMB+MMR secured\n",
               offer.height, (unsigned long long)offer.num_utxos);
    } else {
        printf("Fast sync: no snapshot available yet\n");
    }

    printf("Building chunk sync manifest...\n");
    struct sync_manifest chunk_manifest;
    memset(&chunk_manifest, 0, sizeof(chunk_manifest));
    if (fast_sync_build_manifest(datadir, &chunk_manifest)) {
        int32_t manifest_height = chunk_manifest.height;
        uint32_t num_chunks = chunk_manifest.num_chunks;
        uint64_t num_utxos = chunk_manifest.num_utxos;
        msg_processor_publish_manifest(&chunk_manifest);
        printf("Chunk manifest ready: h=%d, %u chunks (%llu UTXOs)\n",
               manifest_height, num_chunks, (unsigned long long)num_utxos);
    } else {
        printf("Chunk manifest: not available yet\n");
    }

    int32_t tip_h = offer.height;

    if (tip_h > BLOCKS_PER_PIECE) {
        printf("Building block piece manifest...\n");
        struct block_piece_manifest block_manifest;
        memset(&block_manifest, 0, sizeof(block_manifest));
        if (block_piece_manifest_build(datadir, 1, tip_h,
                                        &block_manifest)) {
            int32_t start_height = block_manifest.start_height;
            int32_t end_height = block_manifest.end_height;
            uint32_t num_pieces = block_manifest.num_pieces;
            msg_processor_publish_block_manifest(&block_manifest, tip_h);
            printf("Block manifest ready: h=%d..%d, %u pieces\n",
                   start_height, end_height, num_pieces);
        } else {
            printf("Block manifest: build failed\n");
        }
    }

    return NULL;
}

/* ── Runtime service startup (called from app_init) ────────── */

bool app_init_services(struct app_context *ctx,
                        const struct chain_params *params,
                        struct boot_svc_ctx *svc)
{
    S = svc;
    node_db_sync_catchup_job_init(&svc->catchup_job);
    snapshot_tx_index_job_init(&svc->tx_index_job);
    snapsync_init(&svc->snapshot_sync, svc->node_db);
    if (svc->db_service) {
        db_service_attach(svc->db_service, svc->node_db);
        db_service_start(svc->db_service);
    }
    svc->runtime.db_service = svc->db_service;
    svc->runtime.snapshot_sync = &svc->snapshot_sync;
    svc->runtime.mempool = svc->mempool;
    svc->runtime.wallet = svc->wallet;
    app_runtime_set_current(&svc->runtime);

    /* ── Register sync state observer ──────────────────────────── *
     * Logs every sync state transition via the event system.
     * Registered as async observer so it never blocks P2P threads. */
    extern void boot_sync_state_logger(enum event_type, uint32_t,
                                        const void *, uint32_t, void *);
    event_observe_async(EV_SYNC_STATE_CHANGE, boot_sync_state_logger, NULL);
    event_observe_async(EV_TIP_UPDATED, boot_sync_state_logger, NULL);
    event_observe_async(EV_BLOCK_CONNECTED, boot_sync_state_logger, NULL);
    event_observe_async(EV_REORG_START, boot_sync_state_logger, NULL);

    /* Initialize mempool */
    tx_mempool_init(svc->mempool, 1000);

    /* Mempool limits — enforce size caps, fee floors, expiry.
     * Registers a post-add hook on tx_mempool so enforcement
     * happens automatically — no call sites to change. */
    {
        struct mempool_limits_config ml_cfg;
        mempool_limits_config_defaults(&ml_cfg);
        if (mempool_limits_start(svc->mempool, &ml_cfg))
            printf("Mempool limits started (max=%lldMB max_tx=%lld)\n",
                   (long long)(ml_cfg.max_bytes >> 20),
                   (long long)ml_cfg.max_tx_count);
    }

    if (boot_node_db())
        node_db_sync_mempool_load(boot_node_db(), svc->mempool);

    /* Rescan blockchain for wallet transactions if wallet is behind chain tip */
    {
        struct block_index *chain_tip = active_chain_tip(&svc->state->chain_active);
        int tip_height = active_chain_height(&svc->state->chain_active);
        if (chain_tip && svc->wallet->best_block_height < tip_height) {
            int scan_from = svc->wallet->best_block_height > 0
                ? svc->wallet->best_block_height + 1 : 0;
            if (svc->wallet->time_first_key > 0 && scan_from == 0) {
                int64_t scan_time = svc->wallet->time_first_key - 7200;
                for (int h = tip_height; h >= 0; h--) {
                    struct block_index *bi = active_chain_at(
                        &svc->state->chain_active, h);
                    if (bi && (int64_t)bi->nTime < scan_time) {
                        scan_from = h + 1;
                        break;
                    }
                }
            }
            if (scan_from == 0 && svc->wallet->best_block_height == 0 &&
                tip_height > 1000) {
                printf("Wallet scan height is 0 with %d blocks. "
                       "Use rescanblockchain RPC for targeted rescan.\n",
                       tip_height);
            } else if (tip_height - scan_from < 50000) {
                wallet_rescan(svc->wallet, &svc->state->chain_active,
                              scan_from, tip_height, ctx->datadir);
            } else {
                printf("Wallet needs rescan from %d to %d (%d blocks). "
                       "Deferring — use rescanblockchain RPC.\n",
                       scan_from, tip_height, tip_height - scan_from);
            }
        }
    }

    wallet_verify_utxos(svc->wallet, svc->coins_tip);

    /* Rebuild wallet_utxos from ground truth ONLY if empty */
    {
        struct node_db *ndb = boot_node_db();
        if (ndb && ndb->open) {
            int64_t t0 = (int64_t)time(NULL);
            sqlite3_stmt *chk = NULL;
            int existing = 0;
            if (sqlite3_prepare_v2(ndb->db,
                    "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
                    -1, &chk, NULL) == SQLITE_OK) {
                if (sqlite3_step(chk) == SQLITE_ROW)
                    existing = sqlite3_column_int(chk, 0);
                sqlite3_finalize(chk);
            }
            if (existing > 0) {
                printf("wallet_utxos: keeping %d existing UTXOs (synced from zclassicd)\n",
                    existing);
            } else {
                char *err = NULL;
                int rc = sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
                if (rc != SQLITE_OK) {
                    fprintf(stderr, "wallet_utxos: BEGIN failed: %s\n",
                            sqlite3_errmsg(ndb->db));
                } else {
                    rc = sqlite3_exec(ndb->db,
                    "INSERT OR IGNORE INTO wallet_utxos "
                    "(txid, vout, value, address_hash, script, height, is_coinbase) "
                    "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
                    "u.height, u.is_coinbase "
                    "FROM utxos u INNER JOIN wallet_keys wk "
                    "ON u.address_hash = wk.pubkey_hash",
                    NULL, NULL, &err);
                }
                if (err) {
                    printf("wallet_utxos INSERT: %s\n", err);
                    sqlite3_free(err);
                    err = NULL;
                }
                if (rc != SQLITE_OK) {
                    if (sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
                        fprintf(stderr, "wallet_utxos: ROLLBACK failed: %s\n",
                                sqlite3_errmsg(ndb->db));
                    }
                } else if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
                    fprintf(stderr, "wallet_utxos: COMMIT failed: %s\n",
                            sqlite3_errmsg(ndb->db));
                    if (sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
                        fprintf(stderr, "wallet_utxos: ROLLBACK after COMMIT failure failed: %s\n",
                                sqlite3_errmsg(ndb->db));
                    }
                }
            }
            int64_t bal = 0;
            sqlite3_stmt *s = NULL;
            sqlite3_prepare_v2(ndb->db,
                "SELECT COALESCE(sum(value),0) FROM wallet_utxos "
                "WHERE spent_txid IS NULL", -1, &s, NULL);
            if (sqlite3_step(s) == SQLITE_ROW)
                bal = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
            int cnt = 0;
            sqlite3_prepare_v2(ndb->db,
                "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
                -1, &s, NULL);
            if (sqlite3_step(s) == SQLITE_ROW)
                cnt = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
            printf("Wallet: %.8f ZCL (%d UTXOs, %lldms)\n",
                   (double)bal / 1e8, cnt,
                   (long long)((int64_t)time(NULL) - t0) * 1000);
        }
    }

    /* Sync wallet keys to SQLite */
    if (boot_node_db())
        node_db_sync_wallet_keys(boot_node_db(), boot_wallet());

    /* Initialize message processor */
    msg_processor_init(svc->msg_processor, svc->state, svc->mempool,
                       svc->coins_tip, params, ctx->datadir,
                       &svc->connman->manager, &svc->runtime);

    /* Initialize P2P connection manager */
    struct node_signals signals = {
        .get_height = msg_get_height,
        .process_messages = msg_process_messages,
        .send_messages = msg_send_messages,
        .initialize_node = NULL,
        .finalize_node = NULL,
        .ctx = svc->msg_processor,
    };
    connman_init(svc->connman, params, &signals);
    svc->connman->datadir = ctx->datadir;

    /* Load persisted peer addresses from previous session */
    connman_load_addrman(svc->connman);

    addr_db_read(&svc->connman->manager, ctx->datadir);

    if (ctx->listen) {
        struct net_service bind4;
        net_service_init(&bind4);
        unsigned char any4[4] = {0, 0, 0, 0};
        net_addr_set_ipv4(&bind4.addr, any4);
        bind4.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind4, false))
            printf("P2P listening on 0.0.0.0:%d\n", ctx->p2p_port);
        struct net_service bind6;
        net_service_init(&bind6);
        bind6.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&svc->connman->manager, &bind6, false))
            printf("P2P listening on [::]:%d\n", ctx->p2p_port);
    }

    /* Wait for ZK params before P2P (needed for block verification) */
    if (ctx->params_dir) {
        if (svc->params_thread_started) {
            pthread_join(svc->params_thread, NULL);
            svc->params_thread_started = false;
        }
        if (!atomic_load(svc->params_loaded))
            fprintf(stderr, "Warning: ZK params not loaded\n");
    }

    /* File sync BEFORE P2P — download block files first, then start P2P.
     * This prevents concurrent writes to block files (file sync + P2P
     * both writing to blk*.dat caused crashes). */
    {
        int chain_height = active_chain_height(&svc->state->chain_active);
        if (chain_height <= 0 && ctx->no_file_sync) {
            printf("=== Fresh node — file sync disabled (-nofilesync), "
                   "using P2P snapshot sync ===\n");
            goto skip_file_sync;
        }
        if (chain_height <= 0) {
            printf("=== Fresh node — trying fast file sync ===\n");
            uint8_t utxo_root[32];
            memset(utxo_root, 0, 32);
            bool file_sync_ok = false;

            /* Try -fileservice= peer first (e.g., localhost speedrun) */
            if (ctx->file_service_peer && !file_sync_ok) {
                printf("Trying file service at %s:%d "
                       "(from -fileservice=)...\n",
                       ctx->file_service_peer, FS_PORT);
                int64_t t0 = (int64_t)time(NULL);
                if (fs_client_sync(ctx->file_service_peer, FS_PORT,
                                    ctx->datadir, utxo_root)) {
                    int64_t elapsed = (int64_t)time(NULL) - t0;
                    printf("=== File sync complete from %s: %llds ===\n",
                           ctx->file_service_peer, (long long)elapsed);
                    file_sync_ok = true;
                }
            }

            /* Fall back to hardcoded seeds (skip in connect-only mode) */
            const char *file_seeds[] = {
                "74.50.74.102",
                "205.209.104.118",
                "140.174.189.3",
                NULL
            };
            /* File service seeds always active — even in connect-only mode,
             * block data comes from file service, not P2P */
            for (int round = 0; round < 3 && !file_sync_ok; round++) {
                if (round > 0) {
                    printf("File sync: retrying in 10s (round %d/3)...\n",
                           round + 1);
                    sleep(10);
                }
                for (int i = 0; file_seeds[i] && !file_sync_ok; i++) {
                    printf("Trying file service at %s:%d...\n",
                           file_seeds[i], FS_PORT);
                    int64_t t0 = (int64_t)time(NULL);
                    if (fs_client_sync(file_seeds[i], FS_PORT,
                                        ctx->datadir, utxo_root)) {
                        int64_t elapsed = (int64_t)time(NULL) - t0;
                        printf("=== File sync complete from %s: %llds ===\n",
                               file_seeds[i], (long long)elapsed);
                        file_sync_ok = true;
                    }
                }
            }
            /* After file download: scan block files to populate block
             * index with BLOCK_HAVE_DATA + nChainTx. Without this,
             * 6 GB of blocks sit unused on disk.
             *
             * NOTE: blocks cannot be connected (activate_best_chain)
             * without a UTXO set. The file service only downloads block
             * files, not chainstate. Blocks are indexed so they don't
             * need to be re-downloaded via P2P — once headers arrive
             * and a UTXO snapshot is received, the blocks on disk will
             * be used automatically. */
            /* Run scan even on partial downloads — 94% of blocks is
             * still useful. Blocks on disk can serve headers + delta sync. */
            {
                /* Check if we have any block files at all */
                char blk0[576];
                snprintf(blk0, sizeof(blk0), "%s/blocks/blk00000.dat",
                         ctx->datadir);
                struct stat blk0_st;
                bool have_blocks = (stat(blk0, &blk0_st) == 0 &&
                                    blk0_st.st_size > 100000);
                if (!have_blocks && !file_sync_ok) goto skip_block_scan;
            }
            {
                /* Load block index from flat file if downloaded */
                char dl_flat[576];
                snprintf(dl_flat, sizeof(dl_flat), "%s/block_index.bin",
                         ctx->datadir);
                struct stat flat_st;
                if (stat(dl_flat, &flat_st) == 0 &&
                    flat_st.st_size > 1000000) {
                    printf("Loading downloaded block_index.bin...\n");
                    fflush(stdout);
                    load_block_index_flat(ctx->datadir, svc->state);
                }

                /* Validate block file references — clear HAVE_DATA for
                 * entries pointing to empty/missing block files. The flat
                 * file from the server may reference blk00000.dat which
                 * is empty (genesis has no on-disk data). */
                if (svc->state->map_block_index.size > 1000) {
                    int cleared = 0;
                    /* Build a quick lookup: which block files exist+nonempty */
                    bool file_ok[256] = {false};
                    for (int fi = 0; fi < 256; fi++) {
                        char bp[576];
                        snprintf(bp, sizeof(bp), "%s/blocks/blk%05d.dat",
                                 ctx->datadir, fi);
                        struct stat bst;
                        if (stat(bp, &bst) == 0 && bst.st_size > 0)
                            file_ok[fi] = true;
                    }
                    size_t vi = 0;
                    struct block_index *vp;
                    while (block_map_next(&svc->state->map_block_index,
                                           &vi, NULL, &vp)) {
                        if (!vp) continue;
                        if (!(vp->nStatus & BLOCK_HAVE_DATA)) continue;
                        if (vp->nFile >= 0 && vp->nFile < 256 &&
                            !file_ok[vp->nFile]) {
                            vp->nStatus &= ~BLOCK_HAVE_DATA;
                            cleared++;
                        }
                    }
                    if (cleared > 0)
                        printf("Cleared HAVE_DATA from %d entries "
                               "referencing empty block files\n", cleared);
                }

                /* If no flat file, scan block files from disk */
                if (svc->state->map_block_index.size < 1000) {
                    printf("Scanning downloaded block files...\n");
                    fflush(stdout);
                    int marked = scan_block_files_mark_data(svc->state,
                        ctx->datadir, params);
                    if (marked > 0) {
                        printf("Block file scan: %d blocks indexed\n",
                               marked);
                        save_block_index_flat(ctx->datadir, svc->state);
                    } else {
                        fprintf(stderr, "Block file scan: 0 blocks\n");
                    }
                }

                /* Check if we received node.db (file_index=254).
                 * If so, the UTXO set is already on disk — SHA3 verified.
                 * We just need to open it and replay delta blocks from
                 * the snapshot height to tip. No full replay needed.
                 *
                 * If node.db was NOT received, fall back to full replay
                 * in background (still fast with assumevalid). */
                bool has_utxo_snapshot = false;
                if (svc->state->map_block_index.size > 1000) {
                    char db_check[576];
                    snprintf(db_check, sizeof(db_check),
                             "%s/consensus_snapshot.db", ctx->datadir);
                    struct stat db_st;
                    if (stat(db_check, &db_st) == 0 &&
                        db_st.st_size > 10000000) {
                        /* consensus_snapshot.db was downloaded — but verify
                         * it actually has UTXOs. After a stale UTXO wipe
                         * the file remains on disk but UTXOs are gone. */
                        int64_t snap_utxo_count = 0;
                        if (svc->node_db && svc->node_db->open &&
                            svc->node_db->db) {
                            sqlite3_stmt *sc = NULL;
                            if (sqlite3_prepare_v2(svc->node_db->db,
                                    "SELECT COUNT(*) FROM utxos", -1,
                                    &sc, NULL) == SQLITE_OK && sc) {
                                if (sqlite3_step(sc) == SQLITE_ROW)
                                    snap_utxo_count =
                                        sqlite3_column_int64(sc, 0);
                                sqlite3_finalize(sc);
                            }
                        }
                        if (snap_utxo_count > 0) {
                            printf("=== UTXO snapshot received: %.0f MB "
                                   "(%lld UTXOs, SHA3 verified) ===\n",
                                   (double)db_st.st_size / (1024.0*1024.0),
                                   (long long)snap_utxo_count);
                            has_utxo_snapshot = true;
                        } else {
                            printf("=== consensus_snapshot.db exists "
                                   "(%.0f MB) but UTXOs empty — waiting "
                                   "for P2P snapshot ===\n",
                                   (double)db_st.st_size / (1024.0*1024.0));
                        }
                    }
                }

                if (svc->state->map_block_index.size > 1000) {
                    printf("=== Data synced: %zu blocks on disk "
                           "(SHA3 verified) ===\n",
                           svc->state->map_block_index.size);

                    if (has_utxo_snapshot) {
                        /* UTXO set already on disk from power node.
                         * Only need delta replay from snapshot height
                         * to current tip. This is fast — typically
                         * just the last few hundred blocks. */
                        printf("=== UTXO snapshot imported — "
                               "delta replay only ===\n");
                        /* Fresh receivers should not also start the store
                         * payment scanner. It opens a second node.db handle
                         * and can race the secure snapshot receive path. */
                        svc->defer_payment_service = true;
                        /* Fresh receivers should not also start the local
                         * snapshot/export builder on the shared DB during
                         * bootstrap. That work contends with secure
                         * snapshot receive and can lock the node DB right
                         * when FlyClient verification hands off to receive. */
                        svc->defer_offer_service = true;
                        /* Address aggregation is advisory and can be
                         * rebuilt later; snapshot receive is on the critical
                         * path. Keep bootstrap receivers single-writer until
                         * secure snapshot handoff completes. */
                        svc->want_address_backfill = false;
                    } else {
                        /* Skip full replay — ZCL23 peers will provide
                         * a UTXO snapshot in ~6 seconds. Replaying 3M
                         * blocks would take ~10 min and starve the P2P
                         * socket, preventing snapshot receipt. */
                        printf("=== No UTXO snapshot — waiting for P2P "
                               "snapshot from ZCL23 peers ===\n");
                    }
                    fflush(stdout);

                    /* Only replay if we have a UTXO snapshot from file
                     * service (delta replay). Otherwise, wait for P2P
                     * snapshot which is much faster than full replay. */
                    if (has_utxo_snapshot) {
                        if (!boot_start_replay_service(svc)) {
                            fprintf(stderr,
                                    "WARNING: failed to start tracked UTXO replay thread\n");
                        }
                    }
                } else if (active_chain_height(&svc->state->chain_active) <= 1) {
                    /* Fresh bootstrap receivers with no usable local chain
                     * data should consume secure sync, not waste startup time
                     * building local export/serve state they cannot use yet. */
                    svc->defer_payment_service = true;
                    svc->defer_offer_service = true;
                    svc->want_address_backfill = false;
                    printf("Fresh bootstrap receiver mode: deferring local serve/build work\n");
                }
            }
        skip_block_scan: ;
        }
    }
    skip_file_sync: ;

    if (!connman_start(svc->connman)) {
        fprintf(stderr, "FATAL: failed to start P2P threads\n");
        return false;
    }
    sync_set_state(SYNC_FINDING_PEERS, "P2P started");

    /* Advertise our external IP in version messages so peers relay us */
    if (ctx->external_ip)
        msg_version_set_external_ip(ctx->external_ip,
                                    (uint16_t)ctx->p2p_port);

    /* Initialize RPC */
    rpc_table_init(svc->rpc_table);
    rpc_blockchain_set_state(svc->state, svc->mempool, ctx->datadir);
    rpc_blockchain_set_coins_db(NULL, svc->coins_tip);
    rpc_blockchain_set_node_db(boot_node_db());
    rpc_blockchain_mmr_init_from_state(boot_node_db());
    rpc_blockchain_mmr_catchup(svc->state);
    rpc_blockchain_mmb_init_from_state(boot_node_db());
    rpc_blockchain_mmb_catchup(svc->state);

    /* Build MMB leaf hash store for FlyClient proof serving.
     * Stored as flat file: 32 bytes per leaf, mmap'd for O(1) access. */
    {
        char leaf_path[512];
        snprintf(leaf_path, sizeof(leaf_path), "%s/mmb_leaves.bin",
                 ctx->datadir);
        mmb_leaf_store_open(&g_mmb_leaf_store, leaf_path);
        struct mmb *mmb = rpc_blockchain_get_mmb();
        if (mmb && g_mmb_leaf_store.num_leaves < mmb->num_leaves) {
            printf("[FlyClient] Rebuilding MMB leaf store (%llu → %llu)...\n",
                   (unsigned long long)g_mmb_leaf_store.num_leaves,
                   (unsigned long long)mmb->num_leaves);
            mmb_leaf_store_rebuild(&g_mmb_leaf_store,
                                  &svc->state->chain_active);
        } else {
            printf("[FlyClient] MMB leaf store: %llu hashes ready\n",
                   (unsigned long long)g_mmb_leaf_store.num_leaves);
        }
    }

    rpc_blockchain_commitment_mmr_init_from_state(boot_node_db());

    /* Bootstrap commitment MMR if empty but chain is at height.
     * After legacy import, we have the UTXO set at tip but no
     * commitment history. Compute one commitment at current height
     * as the starting trust anchor. Full history gets built during
     * reindexchainstate (full block replay). */
    {
        struct mmr *cm = rpc_blockchain_get_commitment_mmr();
        int chain_h = active_chain_height(&svc->state->chain_active);
        if (cm->num_leaves == 0 && chain_h > 1000 &&
            boot_node_db() && boot_node_db()->open) {
            printf("Commitment MMR empty at height %d — computing "
                   "bootstrap commitment...\n", chain_h);

            /* Round down to nearest commitment interval */
            int commit_h = (chain_h / MMR_COMMITMENT_INTERVAL) *
                            MMR_COMMITMENT_INTERVAL;

            /* Get block hash at commit height */
            const struct block_index *tip =
                active_chain_tip(&svc->state->chain_active);
            const struct block_index *bi = tip;
            /* Round 5 Part 3: monotonicity + step-cap guard. */
            int bi_steps = 0;
            while (bi && bi->nHeight > commit_h) {
                const struct block_index *prev = bi->pprev;
                if (!prev || prev->nHeight >= bi->nHeight ||
                    bi_steps++ > 5000000) {
                    bi = NULL; /* corrupt chain — bail */
                    break;
                }
                bi = prev;
            }

            if (bi && bi->phashBlock && bi->nHeight == commit_h) {
                rpc_blockchain_maybe_commit(
                    commit_h, bi->phashBlock->data,
                    svc->coins_tip->commitment.accumulator,
                    svc->coins_tip->commitment.count);
                rpc_blockchain_commitment_mmr_save(boot_node_db());
                printf("Bootstrap commitment at height %d saved\n",
                       commit_h);
            }
        }
    }
    register_blockchain_rpc_commands(svc->rpc_table);

    rpc_hodl_set_state(svc->state, svc->coins_tip, boot_node_db(),
                        ctx->datadir);
    register_hodl_rpc_commands(svc->rpc_table);

    rpc_repair_set_state(svc->state, svc->coins_tip, boot_node_db(),
                         ctx->datadir, params);
    register_repair_rpc_commands(svc->rpc_table);

    rpc_chain_inspect_set_state(svc->state, ctx->datadir,
                                 NULL, svc->coins_tip, boot_node_db());
    register_chain_inspect_rpc_commands(svc->rpc_table);

    explorer_set_state(svc->state, svc->mempool, svc->coins_tip,
                        boot_node_db(), ctx->datadir);

    api_set_state(svc->state, svc->mempool, svc->coins_tip,
                   boot_node_db(), ctx->datadir);

    rpc_rawtx_set_state(svc->state, svc->mempool, svc->coins_tip, ctx->datadir);
    rpc_rawtx_set_keystore(&svc->wallet->keystore);
    rpc_rawtx_set_connman(svc->connman);
    register_rawtransaction_rpc_commands(svc->rpc_table);

    rpc_mining_set_state(svc->state, svc->mempool, svc->coins_tip, ctx->datadir);
    register_mining_rpc_commands(svc->rpc_table);

    rpc_misc_set_state(svc->state);
    rpc_misc_set_wallet(svc->wallet);
    register_misc_rpc_commands(svc->rpc_table);
    rpc_net_set_connman(svc->connman);
    register_net_rpc_commands(svc->rpc_table);

    /* Game platform RPC — latency measurement, game types */
    rpc_game_set_connman(svc->connman);
    register_game_rpc_commands(svc->rpc_table);

    /* Sync watchdog — initialize state here, but defer thread start
     * until after `atomic_store(svc->running, true)` below. Starting
     * the thread now would read svc->running as false (still in boot)
     * and the thread loop would exit on its very first iteration. */
    sync_watchdog_init();

    /* Service health and sync detail RPCs */
    rpc_health_set_state(svc->state, &svc->bg_validation,
                         &svc->bg_hash_verify, svc->connman);
    register_health_rpc_commands(svc->rpc_table);

    /* File transfer service — SHA3-verified chunk serving */
    file_controller_init(ctx->datadir);
    register_file_rpc_commands(svc->rpc_table);

    /* ZCL Market — crypto-incentivized file sharing */
    rpc_market_set_state(boot_node_db());
    register_market_rpc_commands(svc->rpc_table);

    /* ZCL Names — on-chain name registry */
    rpc_name_set_state(boot_node_db());
    rpc_name_set_wallet(svc->wallet, svc->mempool);
    register_name_rpc_commands(svc->rpc_table);

    /* ZCL Messaging — encrypted P2P messages */
    rpc_msg_set_state(boot_node_db(), svc->connman);
    register_msg_rpc_commands(svc->rpc_table);

    /* Atomic Swaps — HTLC contracts for BTC/LTC/DOGE */
    rpc_swap_set_state(boot_node_db());
    register_swap_rpc_commands(svc->rpc_table);

    /* blk_sync.dat from file service is on disk. P2P will re-request
     * blocks it needs — the OS disk cache serves them fast since the
     * data is already in memory from the recent file sync download.
     * The deferred scanner was causing crashes (SIGABRT from concurrent
     * block_index access) and isn't worth the complexity. */

    /* Start file service server only once we have meaningful local chain data.
     * Fresh bootstrap receivers are consumers first; serving can wait. */
    if (svc->defer_offer_service) {
        printf("File service server deferred during fresh bootstrap receiver mode\n");
    } else {
        fs_server_start(ctx->datadir, (uint16_t)ctx->fs_port);
    }

    rpc_wallet_set_state(svc->wallet, svc->state, ctx->datadir, svc->wallet_sqlite,
                         svc->mempool, svc->connman);
    rpc_wallet_set_coins_tip(svc->coins_tip);
    rpc_wallet_set_node_db(boot_node_db());
    register_wallet_rpc_commands(svc->rpc_table);
    register_event_rpc_commands(svc->rpc_table);

    zslp_rpc_set_datadir(ctx->datadir);
    register_zslp_rpc_commands(svc->rpc_table);

    /* Pre-compute fast sync snapshot offer in background */
    {
        int chain_tip_h = active_chain_height(&svc->state->chain_active);
        int best_header = svc->state->pindex_best_header ?
            svc->state->pindex_best_header->nHeight : chain_tip_h;
        bool behind_ibd = (best_header - chain_tip_h) > 1000;

        if (svc->defer_offer_service) {
            printf("Fast sync offer build deferred during bootstrap receiver mode\n");
        } else if (behind_ibd) {
            printf("Fast sync offer build deferred during IBD "
                   "(chain=%d, headers=%d, behind=%d)\n",
                   chain_tip_h, best_header, best_header - chain_tip_h);
        } else if (!boot_start_offer_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start tracked snapshot-offer thread\n");
        }
    }

    /* Wave 26b: initialize metrics observers for Prometheus /metrics */
    mcp_metrics_init();

    /* Start RPC HTTP server */
    set_rpc_warmup_finished();
    rpc_http_start(svc->rpc_table, (uint16_t)ctx->rpc_port,
                    ctx->rpc_user, ctx->rpc_password, ctx->datadir);

    /* Configure API + explorer RPC backends */
    {
        char cookie_path[1024], cookie[256] = "";
        snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", ctx->datadir);
        FILE *cf = fopen(cookie_path, "r");
        if (cf) {
            size_t n = fread(cookie, 1, sizeof(cookie) - 1, cf);
            fclose(cf);
            cookie[n] = '\0';
            char *nl = strchr(cookie, '\n');
            if (nl) *nl = '\0';
            char *colon = strchr(cookie, ':');
            if (colon) {
                *colon = '\0';
                api_set_rpc_backend(cookie, colon + 1, ctx->rpc_port);
                explorer_set_rpc(cookie, colon + 1, ctx->rpc_port);
            }
        } else if (ctx->rpc_user && ctx->rpc_password) {
            api_set_rpc_backend(ctx->rpc_user, ctx->rpc_password,
                                ctx->rpc_port);
            explorer_set_rpc(ctx->rpc_user, ctx->rpc_password,
                             ctx->rpc_port);
        }
    }

    {
        int chain_tip_h = active_chain_height(&svc->state->chain_active);
        int best_header = svc->state->pindex_best_header ?
            svc->state->pindex_best_header->nHeight : chain_tip_h;
        if (best_header - chain_tip_h > 1000) {
            printf("API cache refresh deferred during IBD "
                   "(chain=%d, headers=%d, behind=%d)\n",
                   chain_tip_h, best_header, best_header - chain_tip_h);
        } else {
            api_start_cache();
        }
    }

    /* Start HTTPS block explorer (deferred during IBD) */
    {
        char cert_path[1024], key_path[1024];
        snprintf(cert_path, sizeof(cert_path), "%s/ssl/fullchain.pem",
                 ctx->datadir);
        snprintf(key_path, sizeof(key_path), "%s/ssl/privkey.pem",
                 ctx->datadir);
        if (access(cert_path, R_OK) == 0 && access(key_path, R_OK) == 0) {
            int chain_tip_h = active_chain_height(&svc->state->chain_active);
            int best_header = svc->state->pindex_best_header ?
                svc->state->pindex_best_header->nHeight : chain_tip_h;
            bool near_tip = (best_header - chain_tip_h < 1000) &&
                            (chain_tip_h > g_assume_valid_height - 10000);
            if (near_tip) {
                https_server_start_on_port(cert_path, key_path, "zclnet.net",
                                            ctx->https_port, ctx->https_port - 363);
            } else {
                printf("HTTPS: deferred during IBD (chain=%d, headers=%d, "
                       "behind=%d). Will start when near tip.\n",
                       chain_tip_h, best_header, best_header - chain_tip_h);
                static char s_cert[1024], s_key[1024];
                strncpy(s_cert, cert_path, sizeof(s_cert) - 1);
                strncpy(s_key, key_path, sizeof(s_key) - 1);
                extern void https_deferred_set(const char *cert, const char *key);
                https_deferred_set(s_cert, s_key);
            }
        } else {
            printf("HTTPS: no cert at %s — block explorer not on clearnet\n",
                   cert_path);
        }
    }

    /* Start miner if -gen */
    if (ctx->gen) {
        svc->gen->ms = svc->state;
        svc->gen->coins_tip = svc->coins_tip;
        svc->gen->mempool = svc->mempool;
        svc->gen->params = params;
        svc->gen->datadir = ctx->datadir;
        svc->gen->num_threads = ctx->gen_threads > 0 ? ctx->gen_threads : 1;
        svc->gen->coinbase_script.size = 0;

        if (ctx->miner_address) {
            size_t pk_pfx_len, sc_pfx_len;
            const unsigned char *pk_pfx = chain_params_base58_prefix(
                params, B58_PUBKEY_ADDRESS, &pk_pfx_len);
            const unsigned char *sc_pfx = chain_params_base58_prefix(
                params, B58_SCRIPT_ADDRESS, &sc_pfx_len);
            struct tx_destination dest;
            if (decode_destination(ctx->miner_address, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, &dest))
                script_for_destination(&svc->gen->coinbase_script, &dest);
        }

        gen_start(svc->gen);
    }

    /* Start embedded Tor only if explicitly requested (-tor flag)
     * or if the node has a previous .onion key (returning node).
     * Fresh nodes skip Tor to avoid SIGABRT from bad torrc configs.
     * Clearnet P2P + file service works fine without Tor. */
    {
        char onion_dir[512];
        snprintf(onion_dir, sizeof(onion_dir), "%s/onion-keys", ctx->datadir);
        struct stat onion_st;
        bool has_onion_keys = (stat(onion_dir, &onion_st) == 0);

        if (ctx->tor || has_onion_keys) {
            extern const char *onion_service_start(const char *);
            onion_service_start(ctx->datadir);
            tor_integration_set_handler(onion_request_adapter, NULL);
            printf("Starting embedded Tor...\n");
            if (!tor_integration_start(ctx->datadir, (uint16_t)ctx->p2p_port))
                fprintf(stderr, "Warning: Tor failed to start\n");
            else {
                const char *onion = tor_integration_get_onion_address();
                if (onion)
                    printf("Tor .onion address: %s\n", onion);
                else
                    printf("Tor: bootstrapping...\n");
            }
        } else {
            printf("Tor: skipped (use -tor to enable)\n");
        }
    }

    /* Discover peer reachability */
    {
        static struct node_profile g_node_profile;
        peer_strategy_discover_self(&g_node_profile,
                                    (uint16_t)ctx->p2p_port);

        const char *cn = g_node_profile.has_public_ip ? "yes" : "no";
        const char *method = "";
        if (g_node_profile.nat_pmp_available)
            method = " (NAT-PMP)";
        else if (g_node_profile.upnp_available)
            method = " (UPnP)";
        const char *tor = g_node_profile.tor_available ? "yes" : "no";
        printf("Reachability: clearnet=%s%s tor=%s\n", cn, method, tor);

        char addrs[4][68];
        int n = peer_strategy_get_addresses(&g_node_profile, addrs, 4);
        if (n > 0) {
            printf("Addresses:");
            for (int i = 0; i < n; i++)
                printf(" %s", addrs[i]);
            printf("\n");
        }
    }

    /* Start store payment processor */
    {
        if (svc->defer_payment_service) {
            printf("Store payment processor deferred during bootstrap receiver mode\n");
        } else if (!boot_start_payment_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start tracked payment processor thread\n");
        }
    }

    if (svc->want_address_backfill) {
        /* Re-enabled: SIGSEGV was caused by SQLite memory pressure from
         * a single massive GROUP BY over 1.3M UTXOs with 256MB mmap.
         * Fixed by batching per-address with bounded memory. */
        if (boot_start_address_backfill_service(svc)) {
            printf("Address backfill: started in tracked background thread\n");
            fflush(stdout);
        } else {
            fprintf(stderr,
                    "WARNING: failed to start tracked address backfill thread\n");
        }
    }

    if (svc->want_snapshot_tx_index) {
        if (!boot_start_tx_index_service(svc)) {
            fprintf(stderr,
                    "WARNING: failed to start tracked tx-index build thread\n");
        }
    }

    /* Background full validation — verify every historical signature/proof
     * in a low-priority thread after fast sync. Resumes across restarts.
     * Skip with -nobgvalidation (FlyClient+SHA3 already provides ≥150-bit
     * security for the chain binding; this is belt-and-suspenders). */
    bg_validation_init(&svc->bg_validation, svc->state, svc->node_db,
                       ctx->datadir, params);
    g_bg_validation = &svc->bg_validation;
    if (ctx->no_bg_validation) {
        printf("[bg-valid] Disabled via -nobgvalidation\n");
    } else if (bg_validation_start(&svc->bg_validation)) {
        printf("[bg-valid] Started background full validation\n");
    } else {
        printf("[bg-valid] Deferred — already complete or chain not ready\n");
    }

    /* Background block hash verification — recomputes SHA256d from disk
     * and compares against stored hashes. */
    bg_hash_verify_init(&svc->bg_hash_verify, svc->state, svc->node_db,
                        ctx->datadir, params);
    if (ctx->no_bg_validation) {
        printf("[bg-hash] Disabled via -nobgvalidation\n");
    } else if (bg_hash_verify_start(&svc->bg_hash_verify)) {
        printf("[bg-hash] Started background hash verification\n");
    } else {
        printf("[bg-hash] Deferred — already complete or chain not ready\n");
    }

    atomic_store(svc->running, true);

    /* Sync watchdog thread — must start AFTER atomic_store(running, true).
     * Runs on its own 30s tick, independent of the msg loop (which was
     * gated on peer id==0 and silently disabled once peer ids rotated
     * past zero — left a node 22k blocks behind for >9h with
     * checks_run=1). */
    sync_watchdog_thread_start(&svc->watchdog_thread,
                               &svc->watchdog_thread_started,
                               svc->running,
                               svc->connman,
                               msg_get_download_mgr(),
                               svc->state);

    /* Gap-fill service — sequential block-gap filler. While tip <
     * best_header, walks pprev from best_header and queues any
     * blocks lacking BLOCK_HAVE_DATA. Fixes the "defer far-ahead
     * live block" loop where only the far block was ever requested
     * and the 2000+ intermediate blocks were never downloaded. */
    if (gap_fill_start(svc->state, msg_get_download_mgr())) {
        printf("[gap-fill] background gap-fill service started\n");
    } else {
        fprintf(stderr, "WARNING: gap_fill_start failed\n");
    }

    {
        struct block_index *tip = active_chain_tip(&svc->state->chain_active);
        int h = tip ? tip->nHeight : 0;
        event_emitf(EV_NODE_READY, 0, "height=%d peers=%zu",
                    h, svc->connman->manager.num_nodes);
    }
    printf("ZClassic C23 node initialized.\n");

    /* SQLite catchup — skip when no UTXO set (P2P snapshot incoming).
     * Running catchup during snapshot receive causes DB lock contention
     * that stalls the snapshot data flow. */
    if (boot_node_db()) {
        int64_t utxo_count = db_utxo_count(boot_node_db());
        if (utxo_count == 0 && !ctx->legacy_import_dir) {
            printf("SQLite catchup: skipped (no UTXOs, waiting for P2P snapshot)\n");
        } else if (ctx->legacy_import_dir) {
            struct block_index *fs_tip = active_chain_tip(&svc->state->chain_active);
            struct node_db catchup_db;
            printf("=== SQLite Indexing (%d blocks) ===\n",
                   fs_tip ? fs_tip->nHeight : 0);
            int64_t t_import = (int64_t)time(NULL);
            if (node_db_sync_open_private_db_like(boot_node_db(), &catchup_db)) {
                node_db_sync_catchup(&catchup_db,
                                     &svc->state->chain_active,
                                     svc->wallet, ctx->datadir);
                node_db_close(&catchup_db);
            } else {
                node_db_sync_catchup(boot_node_db(),
                                     &svc->state->chain_active,
                                     svc->wallet, ctx->datadir);
            }
            int64_t t_idx_done = (int64_t)time(NULL);
            printf("Block index: %llds\n", (long long)(t_idx_done - t_import));
            printf("=== SQLite complete in %llds ===\n",
                   (long long)(t_idx_done - t_import));
        } else {
            if (!boot_start_catchup_service(svc, ctx->datadir)) {
                fprintf(stderr,
                        "WARNING: failed to start tracked SQLite catchup thread\n");
            }
        }
    }

    return true;
}

/* ── Shutdown ──────────────────────────────────────────────── */

static void shutdown_stop_frontend_services(struct boot_svc_ctx *svc)
{
    (void)svc;
    tor_integration_stop();

    if (svc->gen->running)
        gen_stop(svc->gen);

    rpc_http_stop();
    fs_server_stop();
}

static void shutdown_persist_fast_restart_state(struct boot_svc_ctx *svc)
{
    boot_join_payment_service(svc);

    if (svc->state->map_block_index.size > 1000) {
        printf("Saving block index flat file (%zu entries)...\n",
               svc->state->map_block_index.size);
        save_block_index_flat(svc->datadir, svc->state);
    }

    addr_db_write(&svc->connman->manager, svc->datadir);
}

static void shutdown_quiesce_network_and_flush_coins(struct boot_svc_ctx *svc)
{
    /* Signal P2P threads to stop, then flush coins while threads wind down.
     * The message thread checks g_stop each iteration (~100ms). Any
     * in-flight activate_best_chain sees g_shutdown_requested and returns.
     * After signal_stop, no new block processing starts. */
    connman_signal_stop(svc->connman);
    boot_join_replay_service(svc);

    /* Flush coins to SQLite. The message thread is finishing its current
     * iteration. If it was mid-connect_block, it already flushed via the
     * g_shutdown_requested handler in activate_best_chain. */
    printf("Flushing coins cache to SQLite...\n");
    if (coins_view_cache_flush(svc->coins_tip)) {
        printf("Coins cache flushed.\n");
    } else {
        fprintf(stderr, "WARNING: Coins cache flush FAILED during shutdown!\n");
    }

    /* Now join threads — safe, coins already persisted */
    connman_join(svc->connman);
    connman_free(svc->connman);

    /* Final flush in case message thread connected blocks between
     * our flush and its exit */
    coins_view_cache_flush(svc->coins_tip);
    coins_view_cache_free(svc->coins_tip);
    coins_view_sqlite_close(svc->coins_sqlite);

    /* Close cached block file handles */
    disk_block_io_close_cache();
}

static void shutdown_persist_runtime_state(struct boot_svc_ctx *svc)
{
    /* Stop services wired from BOOT_QUEUE */
    mempool_limits_stop();
    wallet_backup_stop();
    disk_monitor_stop();
    ibd_throttle_stop();
    db_maintenance_stop();

    sync_watchdog_thread_stop(&svc->watchdog_thread,
                              &svc->watchdog_thread_started);
    gap_fill_stop();
    bg_validation_stop(&svc->bg_validation);
    bg_hash_verify_stop(&svc->bg_hash_verify);
    boot_join_address_backfill_service(svc);
    boot_join_tx_index_service(svc);
    boot_join_offer_service(svc);
    boot_join_catchup_service(svc);

    rpc_blockchain_mmr_save(boot_node_db());
    rpc_blockchain_mmb_save(boot_node_db());
    rpc_blockchain_commitment_mmr_save(boot_node_db());

    if (svc->block_tree_open) {
        block_tree_db_close(svc->block_tree);
        svc->block_tree_open = false;
    }

    if (svc->wallet_sqlite->open) {
        wallet_sqlite_flush(svc->wallet_sqlite, svc->wallet);
        wallet_sqlite_close(svc->wallet_sqlite);
    }
    if (svc->node_db->open) {
        db_service_flush_write(svc->db_service);
        node_db_sync_mempool_save(svc->node_db, svc->mempool);
        /* Checkpoint WAL before closing — prevents WAL corruption on
         * unclean restart and keeps the WAL file small. */
        if (node_db_wal_checkpoint(svc->node_db))
            printf("[shutdown] WAL checkpoint complete\n");
        else
            fprintf(stderr, "[shutdown] WAL checkpoint failed\n");
        db_service_close_write(svc->db_service);
    }
    if (svc->db_service)
        db_service_stop(svc->db_service);
}

static void shutdown_release_owned_resources(struct boot_svc_ctx *svc)
{
    app_runtime_set_current(NULL);
    wallet_free(svc->wallet);
    tx_mempool_free(svc->mempool);
    main_state_free(svc->state);
    sapling_free_params();

    ecc_verify_destroy();
    ecc_stop();
}

void app_shutdown_svc(struct boot_svc_ctx *svc)
{
    extern volatile sig_atomic_t g_shutdown_requested;

    atomic_store(svc->running, false);
    g_shutdown_requested = 1;
    event_emitf(EV_NODE_SHUTDOWN, 0, "graceful");
    event_async_stop();

    printf("Shutting down...\n");

    /* Emergency coins flush FIRST — minimize UTXO loss window.
     * SIGKILL from OOM killer / systemd timeout can arrive at any time
     * during shutdown. Flushing coins before anything else ensures the
     * UTXO state is safe even if the rest of shutdown is interrupted. */
    if (svc->coins_tip) {
        printf("Emergency coins flush...\n");
        coins_view_cache_flush(svc->coins_tip);
        printf("Emergency flush done.\n");
    }

    shutdown_stop_frontend_services(svc);
    shutdown_persist_fast_restart_state(svc);
    shutdown_quiesce_network_and_flush_coins(svc);
    shutdown_persist_runtime_state(svc);
    shutdown_release_owned_resources(svc);

    printf("Shutdown complete.\n");
}

/* ── Utility functions ─────────────────────────────────────── */

void app_add_node(const char *host, int port)
{
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

    if (port <= 0) {
        char *colon = strrchr(hostbuf, ':');
        if (colon && colon != hostbuf) {
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                port = p;
                *colon = '\0';
            }
        }
    }

    uint16_t use_port = port > 0 ? (uint16_t)port
                                 : S->connman->manager.default_port;

    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = use_port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(hostbuf, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);

        printf("Connecting to addnode %s:%u\n", hostbuf, use_port);
        connman_open_connection(S->connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

void app_start_metrics(bool mining)
{
    S->metrics->ms = S->state;
    S->metrics->cm = S->connman;
    S->metrics->params = chain_params_get();
    S->metrics->mining = mining;
    if (!metrics_start(S->metrics))
        fprintf(stderr, "WARNING: failed to start metrics thread\n");
}

void app_stop_metrics(void)
{
    metrics_stop(S->metrics);
}

/* ── Sync state observer ──────────────────────────────────────── *
 * Async observer that logs sync state transitions, tip updates,
 * block connections, and reorgs. Provides high-level observability
 * of the sync pipeline without blocking any P2P or validation thread.
 *
 * Registered at boot via event_observe_async() for:
 *   EV_SYNC_STATE_CHANGE — sync FSM transitions
 *   EV_TIP_UPDATED       — chain tip advances
 *   EV_BLOCK_CONNECTED    — individual blocks connected
 *   EV_REORG_START        — chain reorganization begins */
void boot_sync_state_logger(enum event_type type, uint32_t peer_id,
                               const void *payload, uint32_t payload_len,
                               void *ctx)
{
    (void)ctx;
    const char *msg = (payload_len > 0 && payload) ? (const char *)payload : "";

    switch (type) {
    case EV_SYNC_STATE_CHANGE:
        printf("[observer] sync state → %s\n", msg);
        break;
    case EV_TIP_UPDATED:
        /* Only log major milestones to avoid flooding */
        break;
    case EV_BLOCK_CONNECTED:
        break; /* too noisy for printf, event log captures it */
    case EV_REORG_START:
        fprintf(stderr, "[observer] REORG: %s (peer=%u)\n", msg, peer_id);
        break;
    default:
        break;
    }
    fflush(stdout);
}
