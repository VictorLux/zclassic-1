#define _GNU_SOURCE  /* pthread_timedjoin_np */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot background-workers unit — long-lived helper threads spawned by
 * app_init_services at scattered points during runtime startup and joined by
 * app_shutdown_svc. The start/join helpers are exposed via
 * boot_background_workers.h and called from their original sites in
 * boot_services.c, preserving boot order.
 *
 * Workers: payment_processor_thread, background_utxo_replay,
 * build_snapshot_offer_thread, address_backfill_service_thread,
 * hodl_history_worker_thread, projection_backfill_service_thread.
 *
 * The worker bodies reach the boot context through the boot_services.c
 * accessors (boot_node_db / boot_db_service / boot_running /
 * boot_profile_has_file_service / boot_start_catchup_service /
 * boot_reap_catchup_service / boot_serialize_utxo_snapshot) declared in
 * boot_internal.h, and the snapshot-offer worker reaches the single MMB leaf
 * store via the g_mmb_leaf_store extern (also in boot_internal.h). The generic
 * thread helpers move here; boot_join_thread_bounded is re-exported because the
 * catchup-job helpers that stay in boot_services.c reuse it.
 */

#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "config/boot_background_workers.h"
#include "config/boot_snapshot_import.h"
#include "services/chain_activation_service.h"
#include "services/chain_state_service.h"
#include "services/consensus_snapshot_export_service.h"
#include "services/hodl_history_service.h"
#include "models/mmb_leaf_store.h"
#include "chain/chainparams.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "core/uint256.h"
#include "coins/coins_view.h"
#include "controllers/blockchain_controller.h"
#include "controllers/file_controller.h"
#include "net/file_service.h"
#include "net/fast_sync.h"
#include "validation/process_block.h"
#include "rpc/legacy_chain_oracle.h"
#include "storage/disk_block_io.h"
#include "models/block.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

extern void store_process_payments(const char *datadir);
extern void *backfill_addresses_thread(void *arg);

/* Worker bodies — forward-declared so the start/join pairs below can name them
 * before their definitions further down (matching the original boot_services.c
 * forward declarations). */
static void *payment_processor_thread(void *arg);
static void *background_utxo_replay(void *arg);
static void *build_snapshot_offer_thread(void *arg);
static void *address_backfill_service_thread(void *arg);

/* ── Generic boot thread helpers ───────────────────────────── */

static bool boot_start_thread_service(pthread_t *thread,
                                      bool *started,
                                      void *(*entry)(void *),
                                      void *arg)
{
    if (!thread || !started || !entry || *started)
        return false;
    /* Generic boot service starter wrapper for composition-owned helper
     * threads. Callers own the pthread_t and join it explicitly. A
     * thread_registry_spawn_ex equivalent here would require a
     * name-from-caller param; deferred to a focused follow-up.
     * raw-pthread-ok */
    if (pthread_create(thread, NULL, entry, arg) != 0)
        return false;
    *started = true;
    return true;
}

static void boot_join_deadline_from_now(struct timespec *ts, int timeout_sec)
{
    platform_time_realtime_timespec(ts);
    if (timeout_sec < 0)
        timeout_sec = 0;
    ts->tv_sec += timeout_sec;
}

bool boot_join_thread_bounded(pthread_t thread,
                              const char *name,
                              int timeout_sec)
{
    struct timespec deadline;
    int rc;

    boot_join_deadline_from_now(&deadline, timeout_sec);
    rc = pthread_timedjoin_np(thread, NULL, &deadline);
    if (rc == 0)
        return true;

    if (rc == ETIMEDOUT) {
        fprintf(stderr,
                "[shutdown] %s join timed out after %ds; detaching\n",
                name ? name : "thread", timeout_sec);
    } else {
        fprintf(stderr,
                "[shutdown] %s join failed rc=%d (%s); detaching\n",
                name ? name : "thread", rc, strerror(rc));
    }
    pthread_detach(thread);
    return false;
}

static void boot_join_thread_service_named(pthread_t *thread,
                                           bool *started,
                                           const char *name,
                                           int timeout_sec)
{
    if (!thread || !started || !*started)
        return;
    boot_join_thread_bounded(*thread, name, timeout_sec);
    *started = false;
}

/* ── Start/join pairs ──────────────────────────────────────── */

bool boot_start_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->payment_thread,
                                     &svc->payment_thread_started,
                                     payment_processor_thread, svc);
}

void boot_join_payment_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->payment_thread,
                                   &svc->payment_thread_started,
                                   "payment", 5);
}

bool boot_start_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->replay_thread,
                                     &svc->replay_thread_started,
                                     background_utxo_replay, svc);
}

void boot_join_replay_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->replay_thread,
                                   &svc->replay_thread_started,
                                   "utxo_replay", 5);
}

bool boot_start_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    return boot_start_thread_service(&svc->offer_thread,
                                     &svc->offer_thread_started,
                                     build_snapshot_offer_thread, svc);
}

void boot_join_offer_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->offer_thread,
                                   &svc->offer_thread_started,
                                   "snapshot_offer", 5);
}

bool boot_start_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir)
        return false;
    return boot_start_thread_service(&svc->address_backfill_thread,
                                     &svc->address_backfill_thread_started,
                                     address_backfill_service_thread, svc);
}

void boot_join_address_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    boot_join_thread_service_named(&svc->address_backfill_thread,
                                   &svc->address_backfill_thread_started,
                                   "address_backfill", 5);
}

bool boot_start_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir ||
        snapshot_tx_index_job_is_started(&svc->tx_index_job))
        return false;
    return snapshot_tx_index_job_start(&svc->tx_index_job, svc->datadir);
}

/* HODL history worker.
 *
 * Fills the hodl_history table incrementally so /explorer/hodl can
 * render a time-series of "% of supply held > 1 year" without
 * recomputing on every page hit. The worker walks every ~day
 * (HODL_HISTORY_SAMPLE_STRIDE blocks) up to the current chain tip,
 * idempotent on already-filled rows. Each fill call does one sample
 * then sleeps 1s so we never starve the database. */
static void *hodl_history_worker_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    if (!svc)
        return NULL;
    /* Initial settle: wait for boot to complete + first chain advance. */
    sleep(15);
    while (!svc->hodl_history_thread_stop) {
        struct node_db *ndb = boot_node_db(svc);
        if (ndb && ndb->open && svc->state) {
            int tip = active_chain_height(&svc->state->chain_active);
            if (tip > 0) {
                /* Fill at most 4 samples per tick (~one minute of work
                 * on a busy DB), then sleep so other readers move. */
                (void)hodl_history_fill_pending(ndb->db, tip, 4);
            }
        }
        /* Slow tick — 60s — until the table is caught up, then
         * effectively idle since fill_pending becomes a no-op. */
        for (int i = 0; i < 60 && !svc->hodl_history_thread_stop; i++)
            sleep(1);
    }
    return NULL;
}

static void *projection_backfill_service_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    int last_start_height = -1;

    if (!svc)
        return NULL;

    while (!svc->projection_backfill_thread_stop && boot_running(svc)) {
        struct node_db *ndb = boot_node_db(svc);
        int chain_tip = svc->state ?
            active_chain_height(&svc->state->chain_active) : -1;
        int projection_tip = -1;
        int projection_block_tip = -1;

        boot_reap_catchup_service(svc);

        if (ndb && ndb->open && chain_tip >= 0 &&
            !node_db_sync_catchup_job_is_started(&svc->catchup_job)) {
            projection_tip = node_db_sync_get_tip_height(ndb);
            projection_block_tip = db_block_max_height(ndb);
            if (projection_block_tip >= 0 &&
                projection_tip > projection_block_tip &&
                projection_block_tip < chain_tip) {
                struct block_index *rewind_tip =
                    active_chain_at(&svc->state->chain_active,
                                    projection_block_tip);
                if (rewind_tip && rewind_tip->phashBlock &&
                    node_db_sync_set_tip(ndb, rewind_tip->phashBlock->data,
                                         projection_block_tip)) {
                    event_emitf(EV_RECOVERY_ACTION, 0,
                                "projection-cursor-rewind from=%d to=%d",
                                projection_tip, projection_block_tip);
                    projection_tip = projection_block_tip;
                }
            }
            if (projection_tip < chain_tip) {
                if (last_start_height != chain_tip) {
                    event_emitf(EV_RECOVERY_ACTION, 0,
                                "projection-backfill-start from=%d to=%d",
                                projection_tip + 1, chain_tip);
                    last_start_height = chain_tip;
                }
                if (!boot_start_catchup_service(svc, svc->datadir)) {
                    fprintf(stderr,
                            "WARNING: projection backfill start failed "
                            "(projection=%d chain=%d)\n",
                            projection_tip, chain_tip);
                }
            }
        }

        for (int i = 0; i < 5 &&
             !svc->projection_backfill_thread_stop &&
             boot_running(svc); i++) {
            sleep(1);
        }
    }

    boot_reap_catchup_service(svc);
    return NULL;
}

bool boot_start_hodl_history_service(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->datadir)
        return false;
    svc->hodl_history_thread_stop = false;
    return boot_start_thread_service(&svc->hodl_history_thread,
                                     &svc->hodl_history_thread_started,
                                     hodl_history_worker_thread, svc);
}

void boot_join_hodl_history_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    svc->hodl_history_thread_stop = true;
    boot_join_thread_service_named(&svc->hodl_history_thread,
                                   &svc->hodl_history_thread_started,
                                   "hodl_history", 5);
}

bool boot_start_projection_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    svc->projection_backfill_thread_stop = false;
    return boot_start_thread_service(&svc->projection_backfill_thread,
                                     &svc->projection_backfill_thread_started,
                                     projection_backfill_service_thread, svc);
}

void boot_join_projection_backfill_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    svc->projection_backfill_thread_stop = true;
    boot_join_thread_service_named(&svc->projection_backfill_thread,
                                   &svc->projection_backfill_thread_started,
                                   "projection_backfill", 5);
}

void boot_join_tx_index_service(struct boot_svc_ctx *svc)
{
    if (!svc)
        return;
    if (!svc->tx_index_job.started)
        return;
    boot_join_thread_bounded(svc->tx_index_job.thread, "snapshot_tx_index", 5);
    svc->tx_index_job.started = false;
}

/* ── Helper threads ────────────────────────────────────────── */

/* Watchdog: detect stuck chain and clear BLOCK_FAILED to allow retry. */
static void watchdog_check_stuck(struct boot_svc_ctx *svc)
{
    static int64_t last_height_change = 0;
    static int last_height = -1;

    int h = active_chain_height(&svc->state->chain_active);
    int64_t now = (int64_t)platform_time_wall_time_t();

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

    db_path = zcl_malloc(1024, "address_backfill_db_path");
    if (!db_path)
        return NULL;
    snprintf(db_path, 1024, "%s/node.db", svc->datadir);
    backfill_addresses_thread(db_path);
    free(db_path);
    return NULL;
}

/* ── Background UTXO replay ───────────────────────────────── */
/* After file sync, replay blocks to build UTXO set in background.
 * Node serves data immediately; UTXO set builds while running. */

_Atomic bool g_utxo_replay_active = false;
_Atomic int g_utxo_replay_height = 0;

/* boot_import_snapshot_db lives in config/src/boot_snapshot_import.c so
 * both boot.c (the pre-restore probe — the authoritative call site) and
 * this file (the late-receive path) share one implementation. */

static void *background_utxo_replay(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const struct chain_params *params = chain_params_get();

    if (!svc || !svc->state || !svc->coins_tip || !params || !svc->datadir)
        return NULL;

    atomic_store(&g_utxo_replay_active, true);
    int64_t t0 = (int64_t)platform_time_wall_time_t();

    printf("UTXO replay: starting background chain validation...\n");
    fflush(stdout);

    /* ── Restore chain state from coins_best_block ──────────────
     * After snapshot import (file or P2P), coins_best_block in SQLite
     * points to the snapshot height, but the in-memory g_coins_tip and
     * active chain are still at genesis. We must advance both so the reducer
     * activation path starts from the snapshot height, not genesis.
     * Without this, connect_block fails at height 1 with BIP30 because
     * the snapshot's UTXOs include block 1's unspent coinbase. */
    struct node_db *ndb_restore = boot_node_db(svc);
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
                    struct chain_state_rollback_authorization rollback_auth = {
                        .source = CSR_ROLLBACK_SOURCE_SNAPSHOT,
                        .decision = POLICY_ALLOW,
                        .from_height = active_chain_height(
                            &svc->state->chain_active),
                        .to_height = snap_block->nHeight,
                        .max_depth = INT64_MAX,
                        .evidence_class = "snapshot_coins_best_block",
                        .reason = "utxo_replay_snapshot_restore",
                    };
                    struct chain_state_commit commit = {
                        .new_tip = snap_block,
                        .new_coins_best = cb_hash,
                        .expected_utxo_count = 0,
                        .update_header_tip = false,
                        .rollback_auth = &rollback_auth,
                        .wallet_scan_height = -1,
                        .reason = "utxo_replay_snapshot_restore",
                    };
                    enum csr_result rc = csr_commit_tip(csr_instance(),
                                                        &commit);
                    if (rc == CSR_OK) {
                        printf("UTXO replay: restored chain state from snapshot "
                               "at h=%d\n", snap_block->nHeight);
                    } else {
                        fprintf(stderr, // obs-ok:pre-existing-diagnostic
                                "UTXO replay: csr rejected snapshot restore "
                                "(%s) h=%d\n", csr_result_name(rc),
                                snap_block->nHeight);
                    }
                } else if (!snap_block) {
                    printf("UTXO replay: coins_best_block not in index "
                           "(waiting for P2P headers)\n");
                }
            }
        }
    }

    /* IBD turbo: skip non-essential work during replay */
    struct db_service *dbsvc = boot_db_service(svc);
    struct node_db *ndb = boot_node_db(svc);
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
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
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

static void *build_snapshot_offer_thread(void *arg)
{
    struct boot_svc_ctx *svc = arg;
    const char *datadir = svc ? svc->datadir : NULL;

    if (!datadir || datadir[0] == '\0')
        return NULL;

    /* Wave L (Goal 3 — fast secure sync): snapshot export defaults to
     * ON when file_service is enabled. Every zclassic23 node that runs
     * a file service contributes a shareable consensus_snapshot.db so
     * fresh peers can fast-sync without a central download host.
     *
     * Cost: SQLite vacuum allocates transiently — typically a few GB
     * on archival nodes, sub-second to a few seconds on healthy hosts.
     * Operators on memory-constrained boxes can opt out by setting
     * ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT=0.
     *
     * Build only fires when file_service is enabled (boot profile);
     * other profiles still skip the export. */
    bool file_service_enabled =
        svc && boot_profile_has_file_service(svc->app_ctx);
    const char *export_snapshot =
        getenv("ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT");
    bool export_opt_out = export_snapshot &&
                          strcmp(export_snapshot, "0") == 0;
    if (file_service_enabled && !export_opt_out) {
        printf("Exporting consensus snapshot (no wallet data)...\n");
        struct zcl_result export_result =
            consensus_snapshot_export_service_run(datadir);
        if (export_result.ok) {
            file_controller_refresh_manifest();
            fs_server_refresh_manifest();
            printf("Consensus snapshot ready for file service\n");
        } else {
            printf("Consensus snapshot export skipped/failed (%s)\n",
                   export_result.message);
        }
    } else if (file_service_enabled) {
        printf("Consensus snapshot export skipped on boot "
               "(ZCL_EXPORT_CONSENSUS_SNAPSHOT_ON_BOOT=0 — opt-out)\n");
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
            /* Replay through the snapshot anchor.  Heights are zero-based,
             * so a snapshot at height H needs H+1 MMB leaves and a
             * FlyClient chain_length of H+1. */
            uint64_t replay_count = (uint64_t)offer.height + 1;
            if (g_mmb_leaf_store.num_leaves < replay_count) {
                printf("Fast sync: MMB leaf store short (%llu/%llu); "
                       "snapshot offer unavailable\n",
                       (unsigned long long)g_mmb_leaf_store.num_leaves,
                       (unsigned long long)replay_count);
                memset(offer.mmb_root, 0, 32);
            } else if (lh && replay_count > 0) {
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
        /* Pre-serialize snapshot file and publish its SHA3 metadata.
         * The legacy zsnapshot offer is only advertised when an explicit
         * bounded RAM serving cache exists; otherwise peers should use the
         * chunk/file manifests below. */
        bool snapshot_serving_ready = false;
        struct node_db *ndb = boot_node_db(svc);
        if (ndb && ndb->open) {
            int64_t snap_count = fast_sync_prebuild_snapshot(
                datadir, boot_serialize_utxo_snapshot, ndb);
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
                int64_t snap_buf_size = 0;
                snapshot_serving_ready =
                    fast_sync_get_snapshot_buf(&snap_buf_size) != NULL &&
                    snap_buf_size > 0;
            }
        }

        bool have_mmb_root = false;
        for (size_t i = 0; i < sizeof(offer.mmb_root); i++) {
            if (offer.mmb_root[i] != 0) {
                have_mmb_root = true;
                break;
            }
        }
        if (have_mmb_root && snapshot_serving_ready) {
            msg_processor_update_offer(&offer);
            printf("Fast sync ready: h=%d, %llu UTXOs, MMB+MMR secured\n",
                   offer.height, (unsigned long long)offer.num_utxos);
        } else if (have_mmb_root) {
            printf("Fast sync: snapshot offer withheld "
                   "(disk-backed snapshot serving not enabled)\n");
        } else {
            printf("Fast sync: snapshot offer withheld (MMB root unavailable)\n");
        }
    } else {
        printf("Fast sync: no snapshot available yet\n");
    }

    if (file_service_enabled) {
        printf("Building chunk sync manifest...\n");
        struct sync_manifest chunk_manifest;
        memset(&chunk_manifest, 0, sizeof(chunk_manifest));
        if (fast_sync_build_manifest(datadir, &chunk_manifest)) {
            int32_t manifest_height = chunk_manifest.height;
            uint32_t num_chunks = chunk_manifest.num_chunks;
            uint64_t num_utxos = chunk_manifest.num_utxos;
            msg_processor_publish_manifest(&chunk_manifest);
            printf("Chunk manifest ready: h=%d, %u chunks (%llu UTXOs)\n",
                   manifest_height, num_chunks,
                   (unsigned long long)num_utxos);
        } else {
            printf("Chunk manifest: not available yet\n");
        }
    }

    int32_t tip_h = offer.height;

    if (file_service_enabled && tip_h > BLOCKS_PER_PIECE) {
        printf("Building block piece manifest...\n");
        struct block_piece_manifest block_manifest;
        memset(&block_manifest, 0, sizeof(block_manifest));
        if (block_piece_manifest_build_active_chain(&svc->state->chain_active,
                                                    1, tip_h,
                                                    &block_manifest) ||
            block_piece_manifest_build(datadir, 1, tip_h,
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
