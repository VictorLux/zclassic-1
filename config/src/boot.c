/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "config/boot_internal.h"
#include "config/file_ops.h"
#include "services/snapshot_sync_service.h"
#include "services/chain_activation_controller.h"
#include "services/chain_state_repository.h"
#include "services/recovery_policy.h"
#include "services/utxo_recovery_service.h"
#include "services/block_index_integrity.h"
#include "services/wallet_backup_service.h"
#include "services/disk_monitor.h"
#include "services/ibd_throttle.h"
#include "services/db_maintenance.h"
#include "controllers/wallet_scan.h"
#include "util/sync.h"
#include "net/msgprocessor.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "chain/checkpoints.h"
#include "storage/coins_view_sqlite.h"
#include "storage/coins_db.h"
#include "consensus/validation.h"
#include "controllers/blockchain_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/network_controller.h"
#include "rpc/httpserver.h"
#include "controllers/mining_controller.h"
#include "controllers/transaction_controller.h"
#include "rpc/server.h"
#include "storage/block_index_db.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "validation/contextual_check_tx.h"
#include "net/connman.h"
#include "net/msgprocessor.h"
#include "keys/key_io.h"
#include "mining/gen.h"
#include "script/standard.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_controller.h"
#include "controllers/wallet_controller.h"
#include "controllers/zslp_controller.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"
#include "wallet/wallet_db.h"
#include "sapling/params_init.h"
#include "metrics/metrics.h"
#include "chain/pow.h"
#include "controllers/sync_controller.h"
#include "controllers/legacy_import.h"
#include "controllers/snapshot_controller.h"
#include "validation/update_coins.h"
#include "validation/connect_block.h"
#include "storage/disk_block_io.h"
/* LevelDB dbwrapper only used for legacy import paths */
#include "net/tor_integration.h"
#include "net/https_server.h"
#include "net/fast_sync.h"
#include "net/peer_strategy.h"
#include "event/event.h"
#include "controllers/event_controller.h"
#include "models/block.h"
#include <netdb.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include <sqlite3.h>

static struct main_state g_state;
static struct coins_view_sqlite g_coins_sqlite;
static struct coins_view_cache g_coins_tip;
static struct chain_activation_controller g_activation_ctl;

struct chain_activation_controller *boot_activation_controller(void)
{
    return &g_activation_ctl;
}
static struct block_tree_db g_block_tree;
struct block_tree_db *g_active_block_tree = NULL;
static bool g_block_tree_open = false;
static struct tx_mempool g_mempool;
static struct rpc_table g_rpc_table;
static struct msg_processor g_msg_processor;
static struct connman g_connman;
static struct wallet g_wallet;
static struct gen_context g_gen;
static struct wallet_sqlite g_wallet_sqlite;
static struct node_db g_node_db;
static struct db_service g_db_service;
static const char *g_datadir = NULL;
const char *g_blog_datadir = NULL;
static _Atomic bool g_running = false;
static struct wallet_backup_config g_wallet_backup_cfg;
static struct disk_monitor_config g_disk_monitor_cfg;
static struct ibd_throttle_config g_ibd_throttle_cfg;

/* ── System RAM query ────────────────────────────────────────── */

static size_t get_system_ram(void)
{
    struct sysinfo si;
    if (sysinfo(&si) != 0)
        return 0;
    return (size_t)si.totalram * (size_t)si.mem_unit;
}

/* ── PID lock file for data directory ─────────────────────────── */

static char g_pidfile_path[1024];

/* Acquire data directory lock. Returns true if lock acquired,
 * false if another instance is running. */
static bool acquire_datadir_lock(const char *datadir)
{
    snprintf(g_pidfile_path, sizeof(g_pidfile_path), "%s/zclassic23.pid",
             datadir);

    /* Check existing PID file */
    FILE *f = fopen(g_pidfile_path, "r");
    if (f) {
        char buf[32] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n > 0) {
            long old_pid = strtol(buf, NULL, 10);
            if (old_pid > 0) {
                if (kill((pid_t)old_pid, 0) == 0) {
                    fprintf(stderr,
                        "[boot] Data directory locked by PID %ld (running). "
                        "Cannot start.\n", old_pid);
                    return false;
                }
                printf("[boot] Stale lock detected (PID %ld is not running). "
                       "Removing lock file.\n", old_pid);
            }
        }
    }

    /* Write our PID */
    f = fopen(g_pidfile_path, "w");
    if (!f) {
        fprintf(stderr, "[boot] Cannot create PID file %s: %s\n",
                g_pidfile_path, strerror(errno));
        return true; /* non-fatal — proceed without lock */
    }
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return true;
}

static void release_datadir_lock(void)
{
    if (g_pidfile_path[0])
        unlink(g_pidfile_path);
}

static struct db_service *boot_runtime_db_service(void)
{
    return db_service_is_started(&g_db_service) ? &g_db_service : NULL;
}

/* Policy-gated UTXO wipe moved to utxo_recovery_service.c.
 * boot_policy_wipe_utxos → utxo_recovery_wipe(&g_node_db, reason) */

static bool boot_db_enter_turbo_mode(void)
{
    struct db_service *dbsvc = boot_runtime_db_service();

    if (dbsvc)
        return db_service_ibd_turbo_mode(dbsvc);
    return g_node_db.open && node_db_ibd_turbo_mode(&g_node_db);
}

static bool boot_db_restore_normal_mode(void)
{
    struct db_service *dbsvc = boot_runtime_db_service();

    if (dbsvc)
        return db_service_normal_mode(dbsvc);
    return g_node_db.open && node_db_normal_mode(&g_node_db);
}

static bool boot_db_set_sync_batch_size(int batch_size)
{
    struct db_service *dbsvc = boot_runtime_db_service();

    if (dbsvc)
        return db_service_set_sync_batch_size(dbsvc, batch_size);
    if (!g_node_db.open)
        return false;
    node_db_set_sync_batch_size(&g_node_db, batch_size);
    return true;
}

/* Shielded backfill moved to utxo_recovery_service.c —
 * utxo_recovery_backfill_shielded(). */
static struct metrics_context g_metrics;

/* Comparator for sorting block_index pointers by height (for qsort). */
static int cmp_block_index_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index **)a;
    const struct block_index *pb = *(const struct block_index **)b;
    return (pa->nHeight > pb->nHeight) - (pa->nHeight < pb->nHeight);
}

/* Callback for block_tree_db_load_block_index_guts — inserts a block
 * into the block map, reusing existing entry if hash already present. */
static struct block_index *boot_insert_block_index_cb(void *ctx_ptr,
                                                       const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index((struct chainstate *)ms, hash);
}

/* SQLite tuning and file operations now live in the model layer:
 *   node_db_ibd_turbo_mode()  — database.h
 *   node_db_normal_mode()     — database.h
 *   file_copy(), dir_copy()   — file_ops.h
 */

/* Background ZK param loading */
static char g_params_dir_buf[1024];
static pthread_t g_params_thread;
static bool g_params_thread_started = false;
static _Atomic bool g_params_loaded = false;
static struct boot_svc_ctx g_svc;

void *load_params_thread(void *arg)
{
    (void)arg;
    printf("Loading verification keys (background)...\n");
    if (sapling_init_params(g_params_dir_buf))
        atomic_store(&g_params_loaded, true);
    else
        fprintf(stderr, "Warning: Failed to load ZK params\n");
    printf("Verification keys loaded.\n");
    return NULL;
}

/* Block index, chainstate rebuild, and address backfill are in
 * boot_index.c. Service startup (P2P, RPC, Tor) and shutdown
 * are in boot_services.c. */


void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 18232;
    ctx->p2p_port = 8033;
    ctx->https_port = 8443;
    ctx->fs_port = 18034;
    ctx->listen = true;   /* accept inbound by default — be a good peer */
    ctx->checkpoints_enabled = true;
}


/* Boot timing helper */
static int64_t boot_clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool app_init(struct app_context *ctx)
{
    int64_t t_boot_start = boot_clock_ms();
    int64_t t_phase;

    db_service_init(&g_db_service);

    /* Initialize event log first — everything after this is observable */
    event_log_init();
    event_install_crash_handler();

    /* Start async observer thread + register error accumulator.
     * Captures DB errors, block rejections, flush failures for
     * instant health queries via /api/health and healthcheck RPC. */
    if (!event_async_start())
        fprintf(stderr, "WARNING: failed to start async event dispatcher\n");
    struct error_ring *er = error_ring_global();
    event_observe_async(EV_DB_ERROR, error_ring_observer, er);
    event_observe_async(EV_COINS_FLUSH_FAILED, error_ring_observer, er);
    event_observe_async(EV_BLOCK_REJECTED, error_ring_observer, er);
    event_observe_async(EV_UTXO_CHECKPOINT_FAIL, error_ring_observer, er);

    /* Observe validation failures as errors too */
    event_observe_async(EV_MODEL_VALIDATION_FAILED, error_ring_observer, er);

    event_emitf(EV_NODE_STARTING, 0, "zclassic23 1.0.0");

    if (ctx->regtest)
        chain_params_select(CHAIN_REGTEST);
    else if (ctx->testnet)
        chain_params_select(CHAIN_TESTNET);
    else
        chain_params_select(CHAIN_MAIN);

    const struct chain_params *params = chain_params_get();
    g_datadir = ctx->datadir;
    g_blog_datadir = ctx->datadir;

    /* Auto-create datadir if it doesn't exist */
    {
        struct stat st;
        if (stat(ctx->datadir, &st) != 0) {
            mkdir(ctx->datadir, 0700);
            printf("Created data directory: %s\n", ctx->datadir);
        }
    }

    /* Acquire data directory lock — prevents two instances from
     * corrupting SQLite / LevelDB by writing concurrently. */
    if (!acquire_datadir_lock(ctx->datadir))
        return false;

    /* Crash recovery detection: check for unclean shutdown.
     * A clean shutdown writes .shutdown_clean marker. If it's missing
     * and a WAL file exists, the previous run crashed. */
    {
        char marker_path[1024], wal_path[1024];
        snprintf(marker_path, sizeof(marker_path), "%s/.shutdown_clean",
                 ctx->datadir);
        snprintf(wal_path, sizeof(wal_path), "%s/node.db-wal",
                 ctx->datadir);

        struct stat wal_st;
        bool wal_exists = (stat(wal_path, &wal_st) == 0 && wal_st.st_size > 0);
        bool marker_exists = (access(marker_path, F_OK) == 0);

        if (!marker_exists && wal_exists) {
            printf("[boot] Unclean shutdown detected (WAL=%lldB, "
                   "clean marker missing)\n",
                   (long long)wal_st.st_size);
            event_emitf(EV_CRASH_RECOVERY_START, 0,
                "wal_size=%lld clean_marker=missing",
                (long long)wal_st.st_size);
        } else if (!marker_exists) {
            /* First boot or marker was never written — not a crash */
            printf("[boot] First boot or marker absent (no WAL)\n");
        } else {
            printf("[boot] Clean shutdown marker present\n");
        }

        /* Remove marker at boot — will be re-created on clean shutdown */
        unlink(marker_path);
    }

    /* Disk monitor — armed before first SQLite open so the
     * refuse-when-critical flag blocks writes before damage. */
    disk_monitor_config_defaults(&g_disk_monitor_cfg);
    g_disk_monitor_cfg.datadir = ctx->datadir;
    if (disk_monitor_start(&g_disk_monitor_cfg))
        printf("Disk monitor started (warn=%lldGB refuse=%lldGB)\n",
               (long long)(g_disk_monitor_cfg.warn_free_bytes >> 30),
               (long long)(g_disk_monitor_cfg.refuse_free_bytes >> 30));

    /* IBD throttle — rate-limits block processing during initial sync.
     * NULL config reads ZCL_IBD_BLOCKS_PER_SEC / ZCL_IBD_BURST from env
     * with defaults 500/50. Pass-through when not running. */
    ibd_throttle_config_defaults(&g_ibd_throttle_cfg);
    ibd_throttle_config_from_env(&g_ibd_throttle_cfg);
    if (ibd_throttle_start(&g_ibd_throttle_cfg))
        printf("IBD throttle started (rate=%lld/s burst=%lld)\n",
               (long long)g_ibd_throttle_cfg.blocks_per_sec,
               (long long)g_ibd_throttle_cfg.burst);

    /* Assumevalid is set after block index loads (see ~line 1275).
     * The remote dev's implementation in contextual_check_tx.c handles
     * both script verification (connect_block.c) and Sapling proof
     * verification (contextual_check_tx.c) via g_assume_valid_height. */

    ecc_start();
    ecc_verify_init();

    /* SHA-256 hardware self-test */
    if (!sha256_selftest())
        printf("WARNING: SHA-256 SHA-NI self-test FAILED — using portable fallback\n");
    printf("SHA-256: %s\n", sha256_implementation());

    /* Report field arithmetic acceleration */
    extern const char *fr_accel_implementation(void);
    printf("Field arithmetic: %s\n", fr_accel_implementation());

    main_state_init(&g_state);
    g_state.fTxIndex = ctx->tx_index;
    g_state.fCheckpointsEnabled = ctx->checkpoints_enabled;

    /* Initialize chain activation controller — single authority for
     * when activate_best_chain can run. Must be before any chain work. */
    activation_controller_init(&g_activation_ctl, &g_state, &g_coins_tip,
                               params, ctx->datadir);
    activation_set_state(&g_activation_ctl, ACTIVATION_BOOT_PENDING,
                         "boot_start");

    /* -assumevalid: skip Groth16/Sapling proof verification for blocks
     * at or below the specified hash's height. Default: latest checkpoint.
     * Pass -assumevalid=0 to disable (verify everything). */
    {
        const struct chain_params *cp = chain_params_get();
        if (ctx->assume_valid && strcmp(ctx->assume_valid, "0") == 0) {
            g_assume_valid_height = -1;
            printf("Assume-valid: disabled (verifying all proofs)\n");
        } else if (ctx->assume_valid) {
            /* Resolve user-provided hash after block index loads (deferred) */
        } else {
            /* Default: latest checkpoint height */
            if (cp->checkpointData.nEntries > 0) {
                g_assume_valid_height =
                    cp->checkpointData.entries[cp->checkpointData.nEntries - 1].height;
                printf("Assume-valid: height %d (latest checkpoint)\n",
                       g_assume_valid_height);
            }
        }
    }

    /* Defer ZK key loading to background thread — not needed for RPC startup.
     * Keys load in parallel while block index + wallet initialize. */
    g_params_thread_started = false;
    if (ctx->params_dir) {
        snprintf(g_params_dir_buf, sizeof(g_params_dir_buf), "%s", ctx->params_dir);
        if (pthread_create(&g_params_thread, NULL, load_params_thread, NULL) == 0)
            g_params_thread_started = true;
        else
            fprintf(stderr,
                    "WARNING: failed to start ZK params loader thread\n");
    }

    /* Initialize wallet (before block index — needed for -importlegacy) */
    t_phase = boot_clock_ms();
    wallet_init(&g_wallet);

    /* Load wallet from SQLite (node.db wallet_* tables) */
    if (g_node_db.open && wallet_sqlite_open(&g_wallet_sqlite, g_node_db.db)) {
        wallet_sqlite_read_keys(&g_wallet_sqlite, &g_wallet);
        wallet_sqlite_read_txs(&g_wallet_sqlite, &g_wallet);
        wallet_rebuild_spent_set(&g_wallet);
        wallet_sqlite_read_sapling_keys(&g_wallet_sqlite, &g_wallet);
        wallet_sqlite_read_scripts(&g_wallet_sqlite, &g_wallet);
        wallet_sqlite_read_watch_only(&g_wallet_sqlite, &g_wallet);
        int saved_height = 0;
        if (wallet_sqlite_read_scan_height(&g_wallet_sqlite, &saved_height))
            g_wallet.best_block_height = saved_height;
        printf("Wallet loaded: %zu keys, %zu sapling keys, %zu scripts, "
               "%zu watch-only, %zu txs, scan height %d.\n",
               g_wallet.keystore.num_keys,
               g_wallet.sapling_keys.num_keys,
               g_wallet.keystore.num_scripts,
               g_wallet.keystore.num_watching,
               g_wallet.num_wallet_tx,
               g_wallet.best_block_height);
    } else {
        printf("New wallet created.\n");
    }

    /* One-time wallet migration: if SQLite wallet is empty but LevelDB
     * wallet/ directory exists, import keys/txs from LevelDB. */
    if (g_wallet.keystore.num_keys == 0) {
        char wallet_path[1024];
        snprintf(wallet_path, sizeof(wallet_path), "%s/wallet", ctx->datadir);
        struct stat wst;
        if (stat(wallet_path, &wst) == 0) {
            struct wallet_db legacy_wdb;
            if (wallet_db_open(&legacy_wdb, wallet_path)) {
                printf("Migrating wallet from LevelDB...\n");
                wallet_db_read_keys(&legacy_wdb, &g_wallet);
                wallet_db_read_txs(&legacy_wdb, &g_wallet);
                wallet_rebuild_spent_set(&g_wallet);
                wallet_db_read_sapling_keys(&legacy_wdb, &g_wallet);
                wallet_db_read_scripts(&legacy_wdb, &g_wallet);
                int saved_height = 0;
                if (wallet_db_read_scan_height(&legacy_wdb, &saved_height))
                    g_wallet.best_block_height = saved_height;
                wallet_db_close(&legacy_wdb);

                /* Save to SQLite */
                if (g_wallet_sqlite.open)
                    wallet_sqlite_flush(&g_wallet_sqlite, &g_wallet);

                printf("Wallet migrated: %zu keys, %zu sapling keys\n",
                       g_wallet.keystore.num_keys,
                       g_wallet.sapling_keys.num_keys);
            }
        }
    }

    if (g_wallet.keystore.num_keys == 0) {
        /* Safety check: if wallet_keys table exists but is empty, this
         * might be WAL corruption (keys were in WAL, WAL was deleted).
         * Log a prominent warning but still create keys — the node
         * needs a wallet to function. The lost keys are unrecoverable
         * but we must not silently pretend nothing happened. */
        if (g_node_db.open) {
            sqlite3_stmt *wk_check = NULL;
            if (sqlite3_prepare_v2(g_node_db.db,
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='table' AND name='wallet_keys'",
                    -1, &wk_check, NULL) == SQLITE_OK && wk_check) {
                if (sqlite3_step(wk_check) == SQLITE_ROW &&
                    sqlite3_column_int(wk_check, 0) > 0) {
                    fprintf(stderr,
                        "WARNING: wallet_keys table exists but has 0 keys.\n"
                        "  This may indicate key loss from WAL/SHM deletion.\n"
                        "  Generating fresh keys — old addresses are LOST.\n");
                }
                sqlite3_finalize(wk_check);
            }
        }
        wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);
        /* Persist keys to SQLite immediately — do NOT leave in WAL only.
         * WAL deletion (crash, manual cleanup) would destroy private keys. */
        if (g_wallet_sqlite.open)
            wallet_sqlite_flush(&g_wallet_sqlite, &g_wallet);
        if (g_node_db.open)
            node_db_wal_checkpoint(&g_node_db);
        printf("New wallet created.\n");
    }
    printf("Wallet has %zu keys.\n", g_wallet.keystore.num_keys);
    printf("[boot] %-30s %lldms\n", "wallet_load",
           (long long)(boot_clock_ms() - t_phase));

    /* Wallet backup service — hourly backup rotation after wallet is loaded.
     * Default: ~/wallet_backups, interval=3600s, max_versions=168. */
    wallet_backup_config_defaults(&g_wallet_backup_cfg);
    if (wallet_backup_start(&g_wallet_backup_cfg, &g_node_db))
        printf("Wallet backup started (interval=%ds max=%d)\n",
               g_wallet_backup_cfg.interval_seconds,
               g_wallet_backup_cfg.max_versions);

    /* DB maintenance — periodic WAL checkpoint, ANALYZE, VACUUM.
     * Keeps WAL file bounded and query plans fresh. */
    if (g_node_db.open) {
        struct db_maintenance_schedule dbm_sched;
        db_maintenance_schedule_defaults(&dbm_sched);
        dbm_sched.wal_checkpoint_minutes = 5; /* checkpoint every 5 min */
        if (db_maintenance_start(&g_node_db, &dbm_sched))
            printf("DB maintenance started (wal=%dmin analyze=%dh)\n",
                   dbm_sched.wal_checkpoint_minutes,
                   dbm_sched.analyze_hours);
    }

    /* Pre-flight: check for stale lock files from crashed processes */
    {
        char lock_path[1024];

        /* Check LevelDB locks */
        snprintf(lock_path, sizeof(lock_path), "%s/blocks/index/LOCK",
                 ctx->datadir);
        if (access(lock_path, F_OK) == 0) {
            /* LevelDB LOCK exists — check if holder is still alive */
            FILE *lf = fopen(lock_path, "r");
            if (lf) {
                char pidbuf[32] = {0};
                size_t nr = fread(pidbuf, 1, sizeof(pidbuf) - 1, lf);
                fclose(lf);
                if (nr > 0) {
                    long pid = strtol(pidbuf, NULL, 10);
                    if (pid > 0 && kill((pid_t)pid, 0) != 0) {
                        printf("Removing stale LevelDB LOCK (pid %ld dead)\n",
                               pid);
                        unlink(lock_path);
                    } else if (pid > 0) {
                        fprintf(stderr,
                            "ERROR: LevelDB locked by pid %ld (still running)\n"
                            "Kill the other process or use a different datadir.\n",
                            pid);
                    }
                }
            }
        }
        snprintf(lock_path, sizeof(lock_path), "%s/chainstate/LOCK",
                 ctx->datadir);
        if (access(lock_path, F_OK) == 0) {
            FILE *lf = fopen(lock_path, "r");
            if (lf) {
                char pidbuf[32] = {0};
                size_t nr = fread(pidbuf, 1, sizeof(pidbuf) - 1, lf);
                fclose(lf);
                if (nr > 0) {
                    long pid = strtol(pidbuf, NULL, 10);
                    if (pid > 0 && kill((pid_t)pid, 0) != 0) {
                        printf("Removing stale chainstate LOCK (pid %ld dead)\n",
                               pid);
                        unlink(lock_path);
                    }
                }
            }
        }

        /* Check SQLite WAL lock — WAL mode handles this via busy_timeout,
         * but a crash can leave a -wal file. SQLite recovers automatically. */
        snprintf(lock_path, sizeof(lock_path), "%s/node.db-wal",
                 ctx->datadir);
        if (access(lock_path, F_OK) == 0)
            printf("SQLite WAL file exists (normal after crash recovery)\n");
    }

    /* Open SQLite node database */
    t_phase = boot_clock_ms();
    if (node_db_sync_init(&g_node_db, ctx->datadir)) {
        node_db_migrate(&g_node_db, ctx->datadir);
        int db_tip = node_db_sync_get_tip_height(&g_node_db);
        if (db_tip >= 0) {
            printf("SQLite tip: height=%d\n", db_tip);
            event_emitf(EV_BOOT_DB_OPEN, 0, "schema=%d tip=%d",
                        node_db_schema_version(&g_node_db), db_tip);
        }
    } else {
        fprintf(stderr, "Warning: SQLite database unavailable\n");
        event_emitf(EV_DB_ERROR, 0, "SQLite open failed at %s/node.db",
                    ctx->datadir);
    }
    printf("[boot] %-30s %lldms\n", "sqlite_open_migrate",
           (long long)(boot_clock_ms() - t_phase));

    /* Fast path: -importlegacy imports wallet data from legacy block files
     * and exits. No block index, no P2P, no RPC needed. */
    if (ctx->import_legacy_dir) {
        if (!g_node_db.open) {
            fprintf(stderr, "Error: SQLite database required for import\n");
            return false;
        }
        int result = legacy_import(ctx->import_legacy_dir,
                                    &g_node_db, &g_wallet,
                                    ctx->sapling_scan);
        if (result >= 0)
            printf("Import complete: %d wallet transactions found.\n", result);
        else
            fprintf(stderr, "Import failed.\n");
        return false; /* triggers exit in main() */
    }

    /* -snapshot: Create snapshot of legacy data dir, import in parallel,
     * then start normally with P2P sync to catch up any delta. */
    if (ctx->snapshot_dir) {
        if (!g_node_db.open) {
            fprintf(stderr, "Error: SQLite database required for snapshot\n");
            return false;
        }

        /* Step 1: Create snapshot (hardlink block files, copy LevelDB) */
        const char *snap = snapshot_create(ctx->snapshot_dir,
                                           ctx->datadir, 2);
        if (!snap) {
            fprintf(stderr, "Error: Failed to create snapshot\n");
            return false;
        }

        /* Step 2: Parallel import (block index + UTXOs + wallet) */
        if (snapshot_import(snap, ctx->datadir,
                            &g_node_db, &g_wallet) < 0) {
            fprintf(stderr, "Warning: Snapshot import had errors\n");
        }

        /* Step 3: Build transaction index after runtime services take
         * ownership of background jobs, so shutdown can join it cleanly. */
    }

    /* -import-from: copy zclassicd's data and start immediately.
     *
     * Usage: ./zclassic23 -import-from=~/.zclassic
     *
     * Strategy (fastest to slowest, tried in order):
     * 1. Symlink LevelDB dirs + hardlink block files (instant, same FS)
     * 2. Hardlink everything (instant, same FS)
     * 3. Copy (minutes, cross FS)
     *
     * IMPORTANT: zclassicd MUST be stopped first. LevelDB cannot be
     * shared between processes. The block files are read-only so
     * hardlinks/symlinks are safe even if zclassicd restarts later.
     *
     * The LevelDB formats are wire-compatible — same CDiskBlockIndex
     * and CCoins serialization. No conversion needed. */
    if (ctx->legacy_import_dir) {
        struct timespec _fs_ts;
        clock_gettime(CLOCK_MONOTONIC, &_fs_ts);
        int64_t t_fs_start_ms = _fs_ts.tv_sec * 1000 + _fs_ts.tv_nsec / 1000000;
        printf("═══ Fast Sync from Legacy Node ═══\n");
        printf("Source: %s\n", ctx->legacy_import_dir);
        printf("Target: %s\n\n", ctx->datadir);

        char src_test[1024];
        snprintf(src_test, sizeof(src_test), "%s/blocks", ctx->legacy_import_dir);
        struct stat st_check;
        if (stat(src_test, &st_check) != 0) {
            fprintf(stderr, "ERROR: Source not found: %s\n"
                    "  Stop zclassicd first, then:\n"
                    "  ./zclassic23 -import-from=~/.zclassic\n", src_test);
            return false;
        }

        /* Check if zclassicd is still running (LevelDB lock check) */
        {
            char lock_path[1024];
            snprintf(lock_path, sizeof(lock_path),
                     "%s/blocks/index/LOCK", ctx->legacy_import_dir);
            FILE *lf = fopen(lock_path, "r");
            if (lf) {
                char pidbuf[32] = {0};
                size_t nr = fread(pidbuf, 1, sizeof(pidbuf) - 1, lf);
                fclose(lf);
                if (nr > 0) {
                    long pid = strtol(pidbuf, NULL, 10);
                    if (pid > 0 && kill((pid_t)pid, 0) == 0) {
                        fprintf(stderr,
                            "ERROR: zclassicd is still running (pid %ld)!\n"
                            "  Stop it first: zcl-rpc stop\n", pid);
                        return false;
                    }
                }
            }
        }

        char src[1024], dst[1024];

        /* Count source block files */
        int num_blk = 0;
        int64_t total_bytes = 0;
        {
            snprintf(src, sizeof(src), "%s/blocks", ctx->legacy_import_dir);
            for (int f = 0; f < 9999; f++) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/blk%05d.dat", src, f);
                struct stat fst;
                if (stat(path, &fst) != 0) break;
                total_bytes += fst.st_size;
                num_blk++;
            }
        }
        printf("  Source: %d block files, %.1f GB\n\n",
               num_blk, (double)total_bytes / (1024.0*1024.0*1024.0));

        /* Block files: remove stale, then hardlink (or copy cross-device).
         * Stale blk/rev files from old P2P sync cause "failed to read block"
         * because they have different sizes than the source originals. */
        snprintf(dst, sizeof(dst), "%s/blocks", ctx->datadir);
        mkdir(dst, 0700);
        snprintf(src, sizeof(src), "%s/blocks", ctx->legacy_import_dir);
        printf("  [1/3] Block files...");
        fflush(stdout);
        block_files_clean(dst);
        int copied = block_files_copy(src, dst);
        if (copied < 0) {
            fprintf(stderr, " failed\n");
            fprintf(stderr, "legacy import: block file copy failed from %s to %s\n",
                    src, dst);
            return false;
        }
        if (copied == 0) {
            fprintf(stderr, " failed\n");
            fprintf(stderr, "legacy import: failed to copy block files from %s to %s\n",
                    src, dst);
            return false;
        }
        printf(" %d files copied\n", copied);

        /* Block index: byte copy (each node needs its own LevelDB lock) */
        snprintf(dst, sizeof(dst), "%s/blocks/index", ctx->datadir);
        snprintf(src, sizeof(src), "%s/blocks/index", ctx->legacy_import_dir);
        printf("  [2/3] Block index...");
        fflush(stdout);
        if (!dir_copy(src, dst)) {
            fprintf(stderr, " failed\n");
            fprintf(stderr, "legacy import: failed to copy block index from %s to %s\n",
                    src, dst);
            return false;
        }
        printf(" done\n");

        /* Chainstate: byte copy (each node needs its own LevelDB lock) */
        snprintf(dst, sizeof(dst), "%s/chainstate", ctx->datadir);
        snprintf(src, sizeof(src), "%s/chainstate", ctx->legacy_import_dir);
        printf("  [3/3] Chainstate...");
        fflush(stdout);
        if (!dir_copy(src, dst)) {
            fprintf(stderr, " failed\n");
            fprintf(stderr, "legacy import: failed to copy chainstate from %s to %s\n",
                    src, dst);
            return false;
        }
        printf(" done\n");

        /* Copy block_index.bin flat file if available.
         * This avoids the 116s LevelDB scan on first boot.
         * Check: 1) our own data dir (prior run), 2) default c23 dir. */
        {
            char flat_src[1024], flat_dst[1024];
            snprintf(flat_dst, sizeof(flat_dst), "%s/block_index.bin",
                     ctx->datadir);
            struct stat flat_st;
            bool have_flat = false;

            /* Check our own datadir (for re-legacy import) */
            if (!have_flat && stat(flat_dst, &flat_st) == 0 &&
                flat_st.st_size > 1000000) {
                have_flat = true; /* already there */
            }
            /* Check default C23 data dir */
            if (!have_flat) {
                const char *home = getenv("HOME");
                if (home) {
                    snprintf(flat_src, sizeof(flat_src),
                             "%s/.zclassic-c23/block_index.bin", home);
                    if (stat(flat_src, &flat_st) == 0 &&
                        flat_st.st_size > 1000000) {
                        printf("  [+] block_index.bin...");
                        fflush(stdout);
                        if (file_copy(flat_src, flat_dst))
                            printf(" copied (%.0f MB)\n",
                                   (double)flat_st.st_size / (1024.0*1024.0));
                        else
                            printf(" failed (will rebuild from LevelDB)\n");
                    }
                }
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &_fs_ts);
        int64_t t_fs_elapsed_ms = _fs_ts.tv_sec * 1000 +
            _fs_ts.tv_nsec / 1000000 - t_fs_start_ms;
        printf("\n═══ Fast sync file copy: %lldms ═══\n\n",
               (long long)t_fs_elapsed_ms);

        /* Delete stale flat file — force fresh LevelDB load.
         * The flat file from a prior run may have wrong chain_work=0,
         * causing activate_best_chain to see most_work=0 and get stuck. */
        {
            char stale_flat[1024];
            snprintf(stale_flat, sizeof(stale_flat), "%s/block_index.bin",
                     ctx->datadir);
            if (unlink(stale_flat) == 0)
                printf("Deleted stale block_index.bin (will rebuild from LevelDB)\n");
        }
    }

    /* Open block index database.
     * Remove stale LOCK files — left behind by unclean legacy import exit. */
    char blocktree_path[1024];
    snprintf(blocktree_path, sizeof(blocktree_path), "%s/blocks/index",
             ctx->datadir);
    {
        char lock_path[1100];
        snprintf(lock_path, sizeof(lock_path), "%s/LOCK", blocktree_path);
        unlink(lock_path); /* harmless if doesn't exist */
    }
    if (block_tree_db_open(&g_block_tree, blocktree_path,
                           256 << 20, false, false)) {
        g_block_tree_open = true;
        g_active_block_tree = &g_block_tree;
    } else {
        fprintf(stderr, "Warning: Could not open block tree DB at %s\n",
                blocktree_path);
    }

    /* Open coins view on the SHARED sqlite3 handle.
     * Both node_db and coins_view_sqlite use the same connection.
     * Transaction coordination is handled by flush_coins_if_needed
     * which commits node_db's batch before the coins flush runs
     * its own BEGIN/COMMIT. One connection = no WAL lock contention. */
    if (g_node_db.open) {
        if (!coins_view_sqlite_open(&g_coins_sqlite, g_node_db.db)) {
            fprintf(stderr, "Warning: Could not open SQLite coins view\n");
        }
    }

    /* One-time migration: import UTXOs from LevelDB chainstate into SQLite.
     * The old LevelDB had the authoritative UTXO set; SQLite's utxos table
     * may be incomplete. Check for migration flag in node_state. */
    if (g_node_db.open && g_coins_sqlite.db) {
        /* Seed coins_best_block from tip_hash if not yet set.
         * Only do this if UTXOs exist AND no LDB chainstate is available.
         * If chainstate/ exists, the LDB import will set coins_best_block
         * correctly — seeding from tip_hash would create a mismatch
         * (UTXO data from LDB height ~3M labeled as chain tip ~2M). */
        struct uint256 coins_check;
        memset(&coins_check, 0, sizeof(coins_check));
        if (!coins_view_sqlite_get_best_block(&g_coins_sqlite, &coins_check)
            || uint256_is_null(&coins_check)) {
            /* Check if LDB chainstate exists — if so, skip the seed
             * and let LDB import set coins_best_block properly. */
            char cs_path[576];
            snprintf(cs_path, sizeof(cs_path), "%s/chainstate", ctx->datadir);
            struct stat cs_st;
            bool has_chainstate = (stat(cs_path, &cs_st) == 0 &&
                                    S_ISDIR(cs_st.st_mode));
            if (has_chainstate) {
                printf("[boot] chainstate/ exists — skipping "
                       "coins_best_block seed (LDB import will set it)\n");
            } else {
                int64_t utxo_count = node_db_utxo_count(&g_node_db);
                if (utxo_count > 0) {
                    uint8_t tip_buf[32];
                    size_t tip_len = 0;
                    if (node_db_state_get(&g_node_db, "tip_hash",
                                           tip_buf, 32, &tip_len) &&
                        tip_len == 32) {
                        node_db_state_set(&g_node_db, "coins_best_block",
                                          tip_buf, 32);
                        printf("Migrated coins_best_block from tip_hash "
                               "(%lld UTXOs)\n", (long long)utxo_count);
                    }
                }
            }
        }

        /* Auto-recovery: check for needs_reimport flag */
        if (utxo_recovery_check_reimport_flag(ctx->datadir))
            ctx->reimport_utxos = true;

        /* -reimport-utxos: force re-import from LevelDB chainstate */
        if (ctx->reimport_utxos) {
            if (!utxo_recovery_prepare_reimport(&g_node_db))
                ctx->reimport_utxos = false;
        }

        /* LDB UTXO import deferred to post-block-index (see below). */
    }

    coins_view_cache_init(&g_coins_tip, &g_coins_sqlite.view);

    /* Wire the process-lifetime chain_state_repository singleton now
     * that g_coins_tip is alive. From this point on, call-site
     * migrations can go through csr_commit_tip() and get all six
     * sources of truth updated atomically under one mutex. Wallet
     * scan height is unwired (NULL) — the wallet manages its own
     * scan state and we don't want to tempt callers into driving it
     * through the repository until Phase 3. */
    csr_init(csr_instance(),
             &g_state.map_block_index,
             &g_state.chain_active,
             &g_state.pindex_best_header,
             &g_coins_tip,
             &g_node_db,
             NULL);

    /* Wire UTXO commitment: load from SQLite and set pointer for
     * persistence on flush. */
    set_coins_sqlite_for_commitment(&g_coins_sqlite);
    if (coins_view_sqlite_read_commitment(&g_coins_sqlite, &g_coins_tip.commitment)) {
        printf("Loaded UTXO commitment from SQLite (count=%llu)\n",
               (unsigned long long)g_coins_tip.commitment.count);
    }

    /* skip_activate removed — activation controller is the authority */
    bool fast_restart = false;

    /* Block index is now cached in SQLite (load_block_index_sqlite).
     * The full index is saved on shutdown/save, enabling instant restart
     * without the 10-15s LevelDB scan. */

    /* OOM protection: estimate block index memory before loading.
     * Warn if it would exceed 50% of system RAM. */
    {
        size_t sys_ram = get_system_ram();
        if (sys_ram > 0) {
            /* Estimate from SQLite or flat file entry count */
            int64_t est_count = 0;
            if (g_node_db.open)
                est_count = db_block_max_height(&g_node_db);
            if (est_count <= 0)
                est_count = 3000000; /* conservative default */
            size_t est_mem = (size_t)est_count * sizeof(struct block_index)
                           + (size_t)est_count * 2 * sizeof(struct block_map_entry);
            if (est_mem > sys_ram / 2) {
                fprintf(stderr,
                    "[boot] WARNING: block index estimated at %zuMB "
                    "(%lld entries x %zu bytes + hash map)\n"
                    "[boot] System has %zuMB RAM. This may cause OOM.\n",
                    est_mem / (1024 * 1024), (long long)est_count,
                    sizeof(struct block_index),
                    sys_ram / (1024 * 1024));
            }
            printf("[boot] system_ram=%zuMB block_index_estimate=%zuMB "
                   "(%lld entries)\n",
                   sys_ram / (1024 * 1024), est_mem / (1024 * 1024),
                   (long long)est_count);
        }
    }

    /* Block index load: flat file first (mmap, <2s), then SQLite, then LevelDB.
     * Jeff Dean rule: use the fastest data structure available. */
    t_phase = boot_clock_ms();
    {
        bool loaded = false;
        loaded = load_block_index_flat(ctx->datadir, &g_state);
        if (!loaded && g_node_db.open)
            loaded = load_block_index_sqlite(&g_node_db, &g_state);

        /* Check if flat file is stale — if it loaded but has far fewer
         * entries than the chain (checked via SQLite), reload from LevelDB.
         * This fixes the case where an old flat file with 6K entries
         * prevents loading the full 3M+ entry index. */
        if (loaded && g_node_db.open) {
            int64_t db_height = db_block_max_height(&g_node_db);
            size_t flat_count = g_state.map_block_index.size;
            if (db_height > 0 && (int64_t)flat_count < db_height - 1000) {
                printf("Block index flat: stale (%zu entries vs chain height %lld)"
                       " — reloading from LevelDB\n",
                       flat_count, (long long)db_height);
                fflush(stdout);
                loaded = false;  /* fall through to LevelDB */
            }

            /* Consistency check: the SQLite tip block must exist in
             * the loaded flat block index AT THE CORRECT HEIGHT.
             * If not, the flat file is corrupted (e.g., a stale h=60
             * placeholder for the current tip hash). Reload from SQLite. */
            if (loaded && db_height > 0) {
                struct db_block tip_blk;
                if (db_block_find_by_height(&g_node_db, (int)db_height,
                                             &tip_blk)) {
                    struct uint256 tip_hash;
                    memcpy(tip_hash.data, tip_blk.hash, 32);
                    struct block_index *flat_tip = block_map_find(
                        &g_state.map_block_index, &tip_hash);
                    if (!flat_tip ||
                        (int64_t)flat_tip->nHeight != db_height) {
                        fprintf(stderr,
                            "Block index flat: tip hash maps to wrong "
                            "height (%d vs SQLite %lld). Corrupt flat "
                            "file — reloading from SQLite.\n",
                            flat_tip ? flat_tip->nHeight : -1,
                            (long long)db_height);
                        loaded = false;
                    }
                }
            }
        }

        if (!loaded) {
            int64_t t_idx_start = (int64_t)time(NULL);
            printf("Loading block index from LevelDB...\n");
            if (!load_block_index(&g_state, params, &g_block_tree, g_block_tree_open)) {
                fprintf(stderr, "Warning: Failed to load block index\n");
            }
            int64_t t_idx_elapsed = (int64_t)time(NULL) - t_idx_start;
            printf("Block index loaded: %zu entries in %llds\n",
                   g_state.map_block_index.size, (long long)t_idx_elapsed);
            event_emitf(EV_BOOT_BLOCK_INDEX, 0, "loaded entries=%zu elapsed=%llds",
                        g_state.map_block_index.size, (long long)t_idx_elapsed);

            /* Save flat file for next restart */
            if (g_state.map_block_index.size > 1000)
                save_block_index_flat(ctx->datadir, &g_state);
        }

        /* If block index is much smaller than the chain, try loading
         * from zclassicd's LevelDB. This gives us 3M+ entries with
         * correct heights and pprev chains in seconds. Triggers when
         * we have <10% of expected entries (e.g., 3K vs 3M chain). */
        {
            int chain_h = active_chain_height(&g_state.chain_active);
            if (chain_h < 1000) {
                /* Estimate expected height from SQLite or coins */
                int64_t db_h = g_node_db.open ? db_block_max_height(&g_node_db) : 0;
                if (db_h > chain_h) chain_h = (int)db_h;
            }
            /* Always try zclassicd LDB if our index has far fewer entries
             * than the chain height. After snapshot sync, our own LDB has
             * only a handful of entries with scrambled heights. */
            bool need_zcd = ((int64_t)g_state.map_block_index.size <
                             (int64_t)chain_h * 9 / 10);
        if (need_zcd) {
            const char *home = getenv("HOME");
            char zcd_idx_path[1024];
            if (home)
                snprintf(zcd_idx_path, sizeof(zcd_idx_path),
                         "%s/.zclassic/blocks/index", home);
            else
                snprintf(zcd_idx_path, sizeof(zcd_idx_path),
                         ".zclassic/blocks/index");

            struct stat zcd_st;
            if (stat(zcd_idx_path, &zcd_st) == 0) {
                printf("Loading block index from zclassicd LevelDB: %s\n",
                       zcd_idx_path);
                fflush(stdout);

                /* Remove stale LOCK from crashed zclassicd */
                char lock_path[1100];
                snprintf(lock_path, sizeof(lock_path), "%s/LOCK", zcd_idx_path);
                unlink(lock_path);

                struct block_tree_db zcd_btdb;
                int64_t t0 = (int64_t)time(NULL);
                if (block_tree_db_open(&zcd_btdb, zcd_idx_path,
                                       450 << 20, false, false)) {
                    if (block_tree_db_load_block_index_guts(
                            &zcd_btdb, boot_insert_block_index_cb, &g_state)) {
                        int64_t elapsed = (int64_t)time(NULL) - t0;
                        printf("Loaded %zu block index entries from zclassicd "
                               "in %llds\n",
                               g_state.map_block_index.size,
                               (long long)elapsed);

                        /* Fix phashBlock pointers after bulk insert */
                        size_t iter2 = 0;
                        struct block_index *pi2;
                        const struct uint256 *hash2;
                        while (block_map_next(&g_state.map_block_index,
                                              &iter2, &hash2, &pi2))
                            if (pi2) pi2->phashBlock = hash2;

                        /* Compute chain work + set chain tip directly.
                         * This avoids the O(n^2) find_most_work_chain scan
                         * which is catastrophically slow with 3M entries. */
                        {
                            size_t n = g_state.map_block_index.size;
                            struct block_index **sorted = malloc(
                                n * sizeof(struct block_index *));
                            if (sorted) {
                                size_t si = 0, idx2 = 0;
                                struct block_index *sp;
                                while (block_map_next(&g_state.map_block_index,
                                                      &si, NULL, &sp))
                                    if (sp && idx2 < n) sorted[idx2++] = sp;
                                n = idx2;
                                qsort(sorted, n, sizeof(*sorted),
                                      cmp_block_index_height);

                                /* Forward pass: compute nChainWork + nChainTx */
                                struct block_index *best = NULL;
                                for (size_t i = 0; i < n; i++) {
                                    struct block_index *b = sorted[i];
                                    struct arith_uint256 proof = GetBlockProof(b);
                                    if (b->pprev)
                                        arith_uint256_add(&b->nChainWork,
                                            &b->pprev->nChainWork, &proof);
                                    else
                                        b->nChainWork = proof;

                                    if (b->nTx > 0) {
                                        if (b->pprev && b->pprev->nChainTx > 0)
                                            b->nChainTx = b->pprev->nChainTx + b->nTx;
                                        else if (!b->pprev)
                                            b->nChainTx = b->nTx;
                                    }

                                    /* Track best valid chain tip */
                                    if (b->nChainTx > 0 &&
                                        (b->nStatus & BLOCK_HAVE_DATA) &&
                                        !(b->nStatus & BLOCK_FAILED_MASK)) {
                                        if (!best || arith_uint256_compare(
                                                &b->nChainWork,
                                                &best->nChainWork) > 0)
                                            best = b;
                                    }
                                }
                                free(sorted);

                                if (best && best->nHeight > 0) {
                                    printf("Chain tip from zclassicd: height=%d "
                                           "nChainTx=%u\n",
                                           best->nHeight, best->nChainTx);
                                    active_chain_set_tip(&g_state.chain_active,
                                                         best);
                                    g_state.pindex_best_header = best;
                                }
                            }
                        }

                        /* Save flat file for instant future boots */
                        save_block_index_flat(ctx->datadir, &g_state);
                    }
                    block_tree_db_close(&zcd_btdb);
                } else {
                    fprintf(stderr, "Could not open zclassicd block index "
                            "at %s\n", zcd_idx_path);
                }

                /* Copy block files from zclassicd if we don't have them */
                if (g_state.map_block_index.size > 1000) {
                    char zcd_blk_dir[1024], c23_blk_dir[1024];
                    if (home)
                        snprintf(zcd_blk_dir, sizeof(zcd_blk_dir),
                                 "%s/.zclassic/blocks", home);
                    else
                        snprintf(zcd_blk_dir, sizeof(zcd_blk_dir),
                                 ".zclassic/blocks");
                    snprintf(c23_blk_dir, sizeof(c23_blk_dir),
                             "%s/blocks", ctx->datadir);

                    for (int fi = 0; fi < 256; fi++) {
                        char src_path[1200], dst_path[1200];
                        snprintf(src_path, sizeof(src_path),
                                 "%s/blk%05d.dat", zcd_blk_dir, fi);
                        snprintf(dst_path, sizeof(dst_path),
                                 "%s/blk%05d.dat", c23_blk_dir, fi);
                        struct stat src_st, dst_st;
                        if (stat(src_path, &src_st) != 0) {
                            if (fi > 2) break; /* stop after gap */
                            continue;
                        }
                        /* Skip if destination already exists and is same size */
                        if (stat(dst_path, &dst_st) == 0 &&
                            dst_st.st_size == src_st.st_size)
                            continue;
                        /* Hard link first (instant, same filesystem) */
                        if (link(src_path, dst_path) == 0) {
                            if (fi % 10 == 0)
                                printf("  linked blk%05d.dat (%lld MB)\n",
                                       fi, (long long)(src_st.st_size >> 20));
                        } else {
                            /* Different filesystem — fall back to copy */
                            char cmd[2500];
                            snprintf(cmd, sizeof(cmd), "cp '%s' '%s'",
                                     src_path, dst_path);
                            printf("  copying blk%05d.dat (%lld MB)...\n",
                                   fi, (long long)(src_st.st_size >> 20));
                            fflush(stdout);
                            system(cmd);
                        }
                        /* Also link/copy rev (undo) files */
                        snprintf(src_path, sizeof(src_path),
                                 "%s/rev%05d.dat", zcd_blk_dir, fi);
                        snprintf(dst_path, sizeof(dst_path),
                                 "%s/rev%05d.dat", c23_blk_dir, fi);
                        if (stat(src_path, &src_st) == 0) {
                            if (link(src_path, dst_path) != 0) {
                                char cmd[2500];
                                snprintf(cmd, sizeof(cmd), "cp '%s' '%s'",
                                         src_path, dst_path);
                                system(cmd);
                            }
                        }
                    }
                    printf("Block files linked/copied from zclassicd\n");
                    fflush(stdout);
                }
            }
        } /* need_zcd */
        } /* chain height check scope */

        /* Save recent blocks to SQLite (skip for large indexes —
         * the flat file handles 3M+ entries in 1-3s and the SQLite
         * cache path uses 10GB+ RAM causing OOM kills) */
        if (g_node_db.open && g_state.map_block_index.size > 1000
            && g_state.map_block_index.size < 500000)
            save_block_index_recent(&g_node_db, &g_state);

        /* Ensure block files from zclassicd are available.
         * Hard-link (instant, same FS) or skip (cross-FS handled above).
         * This runs every boot to catch the case where block_index.bin
         * was loaded from a previous session but blocks/ was wiped. */
        if (g_state.map_block_index.size > 1000) {
            const char *home = getenv("HOME");
            char zcd_blk[1024], c23_blk[1024];
            if (home)
                snprintf(zcd_blk, sizeof(zcd_blk), "%s/.zclassic/blocks", home);
            else
                snprintf(zcd_blk, sizeof(zcd_blk), ".zclassic/blocks");
            snprintf(c23_blk, sizeof(c23_blk), "%s/blocks", ctx->datadir);

            int linked = 0;
            for (int fi = 0; fi < 256; fi++) {
                char src[1200], dst[1200];
                snprintf(src, sizeof(src), "%s/blk%05d.dat", zcd_blk, fi);
                snprintf(dst, sizeof(dst), "%s/blk%05d.dat", c23_blk, fi);
                struct stat ss, ds;
                if (stat(src, &ss) != 0) { if (fi > 2) break; continue; }
                if (stat(dst, &ds) == 0) continue; /* already exists */
                if (link(src, dst) == 0) linked++;
                /* Also link rev file */
                snprintf(src, sizeof(src), "%s/rev%05d.dat", zcd_blk, fi);
                snprintf(dst, sizeof(dst), "%s/rev%05d.dat", c23_blk, fi);
                if (stat(src, &ss) == 0 && stat(dst, &ds) != 0)
                    link(src, dst);
            }
            if (linked > 0)
                printf("Linked %d block files from zclassicd\n", linked);
        }

        /* Propagate nChainTx for all blocks in the index.
         * The flat file and LevelDB don't always store correct nChainTx.
         * Without this, find_most_work_chain() skips blocks with
         * nChainTx=0, causing "tip=X most_work=Y" with Y << X. */
        if (g_state.map_block_index.size > 100) {
            size_t n = g_state.map_block_index.size;
            struct block_index **sorted = malloc(n * sizeof(*sorted));
            if (sorted) {
                size_t si = 0, idx = 0;
                struct block_index *sp;
                while (block_map_next(&g_state.map_block_index, &si, NULL, &sp))
                    if (sp && idx < n) sorted[idx++] = sp;
                n = idx;
                /* Sort by height for forward propagation */
                qsort(sorted, n, sizeof(*sorted), cmp_block_index_height);
                /* Multi-pass propagation (converges in 1-2 passes for
                 * a well-connected chain, up to 5 for edge cases) */
                int total = 0;
                for (int pass = 0; pass < 5; pass++) {
                    int propagated = 0;
                    for (size_t i = 0; i < n; i++) {
                        struct block_index *b = sorted[i];
                        if (b->nHeight == 0) {
                            if (b->nChainTx == 0 && b->nTx > 0) {
                                b->nChainTx = b->nTx;
                                propagated++;
                            }
                        } else if (b->pprev && b->pprev->nChainTx > 0 && b->nTx > 0) {
                            unsigned int expected = b->pprev->nChainTx + b->nTx;
                            if (b->nChainTx != expected) {
                                b->nChainTx = expected;
                                propagated++;
                            }
                        }
                    }
                    total += propagated;
                    if (propagated == 0) break;
                }
                free(sorted);
                if (total > 0)
                    printf("nChainTx propagated for %d blocks\n", total);
            }
        }
    }

    printf("[boot] %-30s %lldms\n", "block_index_load",
           (long long)(boot_clock_ms() - t_phase));

    /* Log block index memory usage */
    {
        size_t entry_count = g_state.map_block_index.size;
        size_t entry_bytes = entry_count * sizeof(struct block_index);
        size_t map_bytes = g_state.map_block_index.capacity *
                           sizeof(struct block_map_entry);
        size_t total_bytes = entry_bytes + map_bytes;
        printf("[boot] block_index: %zu entries, %zu bytes/entry, "
               "index=%zuMB map=%zuMB total=%zuMB\n",
               entry_count, sizeof(struct block_index),
               entry_bytes / (1024 * 1024),
               map_bytes / (1024 * 1024),
               total_bytes / (1024 * 1024));
    }

    /* Bulk height repair: fix scrambled nHeight values from LDB import.
     * This must run AFTER block index is loaded but BEFORE header sync.
     * Without this, header processing fixes heights 160-at-a-time which
     * is far too slow for 3M+ entries with wrong heights. */
    if (g_state.map_block_index.size > 100)
        block_index_repair_heights(&g_state);

    /* pprev chain repair: fix corrupted pprev pointers from LDB import.
     * Reads hashPrevBlock from block data on disk and corrects pprev.
     * Must run after height repair (needs correct heights for sort). */
    if (g_state.map_block_index.size > 100)
        block_index_repair_pprev(&g_state, ctx->datadir);

    /* Block index integrity — verify sidecar SHA3 after all loads.
     * Refuse to boot on mismatch unless ZCL_ALLOW_CORRUPT_INDEX=1. */
    {
        struct block_index *tip = active_chain_tip(&g_state.chain_active);
        if (tip) {
            char err[256] = "";
            enum bii_verdict v = bii_verify(ctx->datadir, &g_node_db,
                                             tip, err, sizeof(err));
            if (v == BII_OK) {
                /* sidecar matches — good */
            } else if (v == BII_SIDECAR_MISSING || v == BII_BODY_MISSING) {
                /* first run or index will be rebuilt — acceptable */
            } else {
                const char *allow = getenv("ZCL_ALLOW_CORRUPT_INDEX");
                if (allow && allow[0] == '1') {
                    fprintf(stderr, "WARNING: block index integrity: %s "
                            "(continuing — ZCL_ALLOW_CORRUPT_INDEX=1)\n", err);
                } else {
                    fprintf(stderr, "FATAL: block index integrity: %s\n"
                            "Set ZCL_ALLOW_CORRUPT_INDEX=1 to override.\n", err);
                    bii_quarantine_corrupt(ctx->datadir, v);
                    return false;
                }
            }
        }
    }

    /* ── LDB UTXO import (runs AFTER block index load) ── */
    t_phase = boot_clock_ms();
    {
        struct utxo_recovery_ctx uctx = {
            .state = &g_state,
            .coins_sqlite = &g_coins_sqlite,
            .coins_tip = &g_coins_tip,
            .ndb = &g_node_db,
            .datadir = ctx->datadir,
            .params = params,
            .activation_ctl = &g_activation_ctl,
            .db_service = boot_runtime_db_service(),
        };
        struct utxo_import_result ir = utxo_recovery_import_ldb(&uctx);
        if (ir.skip_activate) {
            if (ir.anchor_reason[0])
                activation_set_anchor_active(&g_activation_ctl,
                                              ir.anchor_reason);
        }
    }

    printf("[boot] %-30s %lldms\n", "utxo_import",
           (long long)(boot_clock_ms() - t_phase));

    /* Resolve -assumevalid=<hash> now that block index is loaded */
    if (ctx->assume_valid && strcmp(ctx->assume_valid, "0") != 0) {
        struct uint256 av_hash;
        if (strlen(ctx->assume_valid) == 64) {
            /* Parse hex hash (reversed byte order like block explorer) */
            for (int bi = 0; bi < 32; bi++) {
                unsigned int byte;
                sscanf(ctx->assume_valid + bi * 2, "%02x", &byte);
                av_hash.data[31 - bi] = (uint8_t)byte;
            }
            struct block_index *pav = block_map_find(&g_state.map_block_index,
                                                      &av_hash);
            if (pav) {
                g_assume_valid_height = pav->nHeight;
                printf("Assume-valid: height %d (from hash)\n",
                       g_assume_valid_height);
            } else {
                printf("Assume-valid: hash not found in block index, "
                       "using checkpoint default\n");
            }
        } else {
            fprintf(stderr, "Warning: -assumevalid hash must be 64 hex chars\n");
        }
    }

    /* ── Single-pass block index scan ────────────────────────────
     * Previously 6+ separate O(n) scans of 3M entries (15-20s).
     * Now ONE pass that: clears BLOCK_FAILED, finds best header,
     * finds fallback (most chain work with HAVE_DATA+nChainTx),
     * finds reindex target, tracks max HAVE_DATA height. */
    struct block_index *scan_best_header = NULL;  /* most chain work */
    struct block_index *scan_fallback = NULL;      /* most work w/ data */
    struct block_index *scan_reindex_best = NULL;  /* highest w/ pprev+nChainTx */
    int scan_cleared_failed = 0;
    int scan_max_have_data_h = 0;

    {
        size_t si = 0;
        struct block_index *sp;
        while (block_map_next(&g_state.map_block_index, &si, NULL, &sp)) {
            if (!sp) continue;
            /* Clear BLOCK_FAILED */
            if (sp->nStatus & BLOCK_FAILED_MASK) {
                sp->nStatus &= ~BLOCK_FAILED_MASK;
                scan_cleared_failed++;
            }
            /* Best header (most chain work) */
            if (!scan_best_header ||
                arith_uint256_compare(&sp->nChainWork,
                                      &scan_best_header->nChainWork) > 0)
                scan_best_header = sp;
            /* Fallback: most work with HAVE_DATA + nChainTx */
            if ((sp->nStatus & BLOCK_HAVE_DATA) && sp->nChainTx > 0) {
                if (!scan_fallback ||
                    arith_uint256_compare(&sp->nChainWork,
                                          &scan_fallback->nChainWork) > 0)
                    scan_fallback = sp;
            }
            /* Reindex target: highest with pprev + nChainTx */
            if (sp->pprev && sp->nHeight > 0 && sp->nChainTx > 0) {
                if (!scan_reindex_best ||
                    sp->nHeight > scan_reindex_best->nHeight)
                    scan_reindex_best = sp;
            }
            /* Max HAVE_DATA height */
            if ((sp->nStatus & BLOCK_HAVE_DATA) &&
                sp->nHeight > scan_max_have_data_h)
                scan_max_have_data_h = sp->nHeight;
        }
    }
    if (scan_cleared_failed > 0)
        printf("Cleared BLOCK_FAILED from %d block index entries\n",
               scan_cleared_failed);

    /* Restore chain tip from coins DB best block hash */
    if (ctx->reindex_chainstate) {
        if (scan_reindex_best) {
            active_chain_set_tip(&g_state.chain_active, scan_reindex_best);
            g_state.pindex_best_header = scan_reindex_best;
            printf("Reindex target: height=%d\n", scan_reindex_best->nHeight);
        } else {
            printf("Reindex: no best found (total=%zu)\n",
                   g_state.map_block_index.size);
        }
        if (!reindex_chainstate(&g_state, &g_coins_sqlite, &g_coins_tip,
                                 &g_node_db, ctx->datadir)) {
            fprintf(stderr, "Warning: Chainstate reindex had errors\n");
        }
    } else if (fast_restart) {
    } else if (g_state.map_block_index.size > 1) {
        struct utxo_recovery_ctx uctx = {
            .state = &g_state,
            .coins_sqlite = &g_coins_sqlite,
            .coins_tip = &g_coins_tip,
            .ndb = &g_node_db,
            .datadir = ctx->datadir,
            .params = params,
            .activation_ctl = &g_activation_ctl,
            .db_service = boot_runtime_db_service(),
        };
        struct chain_restore_result cr =
            utxo_recovery_restore_chain_tip(&uctx, scan_fallback);
        if (cr.skip_activate) {
            if (cr.anchor_reason[0])
                activation_set_anchor_active(&g_activation_ctl,
                                              cr.anchor_reason);
        }
        if (scan_best_header)
            g_state.pindex_best_header = scan_best_header;
    }

    /* Ensure genesis block is always properly initialized.
     * On a fresh start, load_block_index creates genesis. On restart,
     * LevelDB may have entries but genesis might lack BLOCK_HAVE_DATA
     * or chain_active might not have a tip set. Fix both. */
    {
        struct block_index *genesis = block_map_find(
            &g_state.map_block_index, &params->consensus.hashGenesisBlock);
        if (!genesis) {
            genesis = chainstate_insert_block_index(
                (struct chainstate *)&g_state,
                &params->consensus.hashGenesisBlock);
        }
        if (genesis) {
            if (genesis->nHeight != 0)
                genesis->nHeight = 0;
            if (!(genesis->nStatus & BLOCK_HAVE_DATA)) {
                genesis->nStatus |= BLOCK_HAVE_DATA;
                genesis->nStatus = (genesis->nStatus & ~BLOCK_VALID_MASK) |
                                    BLOCK_VALID_SCRIPTS;
                genesis->nTx = 1;
                genesis->nChainTx = 1;
                printf("Genesis block: marked BLOCK_HAVE_DATA\n");
            }
            if (arith_uint256_is_zero(&genesis->nChainWork))
                genesis->nChainWork = GetBlockProof(genesis);
            /* Set chain tip to genesis if no tip exists */
            if (!active_chain_tip(&g_state.chain_active)) {
                active_chain_set_tip(&g_state.chain_active, genesis);
                g_state.pindex_best_header = genesis;
                printf("Chain tip: initialized to genesis (height 0)\n");
            }
        }
    }

    /* Repair block index from SQLite.
     * After legacy import, blocks in the LevelDB index may lack BLOCK_VALID_SCRIPTS
     * (they were validated by zclassicd but our index doesn't know that).
     * Without this, activate_best_chain won't extend the chain past
     * previously-connected blocks because it only follows fully-validated
     * entries. Also fix any stale file positions. */
    if (g_node_db.open && g_state.map_block_index.size > 1000) {
        /* Only repair blocks near the tip (within 1000 of chain height).
         * The flat file load already has correct data for most blocks.
         * Full 3M-row scan was taking 8+ minutes — this takes <100ms. */
        int repair_from = active_chain_height(&g_state.chain_active) - 1000;
        if (repair_from < 0) repair_from = 0;
        sqlite3_stmt *sel = NULL;
        char repair_sql[256];
        snprintf(repair_sql, sizeof(repair_sql),
            "SELECT hash, file_num, data_pos, status FROM blocks "
            "WHERE file_num >= 0 AND data_pos >= 0 AND height >= %d",
            repair_from);
        int rc = sqlite3_prepare_v2(g_node_db.db, repair_sql, -1, &sel, NULL);
        if (rc == SQLITE_OK && sel) {
            int repaired = 0, checked = 0;
            while (sqlite3_step(sel) == SQLITE_ROW) {
                const void *hash_blob = sqlite3_column_blob(sel, 0);
                int hash_len = sqlite3_column_bytes(sel, 0);
                int file_num = sqlite3_column_int(sel, 1);
                int data_pos = sqlite3_column_int(sel, 2);
                int status = sqlite3_column_int(sel, 3);

                if (!hash_blob || hash_len != 32) continue;

                struct uint256 hash;
                memcpy(hash.data, hash_blob, 32);

                struct block_index *bi = block_map_find(
                    &g_state.map_block_index, &hash);
                if (!bi) continue;
                checked++;

                bool changed = false;

                /* Fix file positions */
                if (bi->nFile != file_num || bi->nDataPos != (unsigned)data_pos) {
                    if (file_num >= 0 && data_pos > 0) {
                        bi->nFile = file_num;
                        bi->nDataPos = (unsigned)data_pos;
                        changed = true;
                    }
                }

                /* Promote validation status from SQLite (indexlegacy
                 * sets BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO|BLOCK_VALID_SCRIPTS
                 * for all blocks it successfully indexes) */
                if (status > 0 && (bi->nStatus & BLOCK_VALID_MASK) <
                    (unsigned)(status & BLOCK_VALID_MASK)) {
                    bi->nStatus = (bi->nStatus & ~(unsigned)BLOCK_VALID_MASK) |
                                  ((unsigned)status & (BLOCK_VALID_MASK |
                                   BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO));
                    changed = true;
                }

                if (changed) repaired++;
            }
            sqlite3_finalize(sel);
            if (repaired > 0) {
                printf("Block index repair: updated %d/%d entries from SQLite\n",
                       repaired, checked);
                fflush(stdout);
            }
        }
    }

    /* Re-link phashBlock pointers after all block_map modifications.
     * block_map_grow reallocates buckets, invalidating any phashBlock
     * pointers stored in block_index entries. This single O(n) pass
     * ensures all pointers are valid before validation and P2P start. */
    {
        size_t relink_iter = 0;
        struct block_index *relink_bi;
        const struct uint256 *relink_hash;
        int relinked = 0;
        while (block_map_next(&g_state.map_block_index, &relink_iter,
                              &relink_hash, &relink_bi)) {
            if (relink_bi && relink_bi->phashBlock != relink_hash) {
                relink_bi->phashBlock = relink_hash;
                relinked++;
            }
        }
        if (relinked > 0)
            printf("Re-linked %d phashBlock pointers\n", relinked);
    }

    /* Validate coins/chain agreement and execute recovery */
    {
        struct boot_validation_result vr =
            validate_coins_chain_agreement(&g_state, &g_coins_tip,
                                           ctx->datadir);
        struct utxo_recovery_ctx uctx = {
            .state = &g_state,
            .coins_sqlite = &g_coins_sqlite,
            .coins_tip = &g_coins_tip,
            .ndb = &g_node_db,
            .datadir = ctx->datadir,
            .params = params,
            .activation_ctl = &g_activation_ctl,
            .db_service = boot_runtime_db_service(),
        };
        struct recovery_exec_result rr = utxo_recovery_execute(&uctx, &vr);
        (void)rr.skip_activate; /* activation controller handles state */

        /* Enter turbo mode if genesis reset happened */
        if (rr.recovered && vr.action != BOOT_RECOVER_RESET_CHAIN &&
            vr.action != BOOT_OK && g_node_db.open) {
            if (!boot_db_enter_turbo_mode())
                fprintf(stderr, "boot: failed to enter turbo mode\n");
            if (!boot_db_set_sync_batch_size(1000))
                fprintf(stderr, "boot: failed to set sync batch size\n");
        }
    }

    /* Clear stale HAVE_DATA above tip — targeted, not full scan.
     * Only needed if max HAVE_DATA height > chain tip (from the
     * single-pass scan above). Skip when block index has 1M+ entries
     * — that means it was loaded from zclassicd's LDB with correct
     * nFile/nDataPos, and clearing HAVE_DATA would force re-download
     * of 3M blocks that are already on disk. */
    {
        int tip_h = active_chain_height(&g_state.chain_active);
        if (scan_max_have_data_h > tip_h && tip_h > 0 &&
            g_state.map_block_index.size < 1000000) {
            int cleared = 0;
            size_t ci = 0;
            struct block_index *cp;
            while (block_map_next(&g_state.map_block_index, &ci, NULL, &cp)) {
                if (cp && cp->nHeight > tip_h &&
                    (cp->nStatus & BLOCK_HAVE_DATA)) {
                    cp->nStatus &= ~BLOCK_HAVE_DATA;
                    cleared++;
                }
            }
            if (cleared > 0)
                printf("Cleared stale HAVE_DATA from %d blocks above tip %d\n",
                       cleared, tip_h);
        }
    }

    /* Scan block files on disk if HAVE_DATA is missing.
     * Uses scan_max_have_data_h from the single-pass scan above
     * instead of another partial iteration. */
    {
        bool need_scan = (scan_max_have_data_h < 100 &&
                          g_state.map_block_index.size > 1000) ||
                         g_state.map_block_index.size < 100;
        if (need_scan) {
            bool have_block_files = false;
            for (int ci = 0; ci < 3 && !have_block_files; ci++) {
                char check_path[576];
                snprintf(check_path, sizeof(check_path),
                         "%s/blocks/blk%05d.dat", ctx->datadir, ci);
                struct stat check_st;
                if (stat(check_path, &check_st) == 0 && check_st.st_size > 0)
                    have_block_files = true;
            }
            if (have_block_files) {
                scan_block_files_mark_data(&g_state, ctx->datadir, params);
                fflush(stdout);

                /* After block file scan, try to resolve coins_best_block.
                 * The scan may have assigned wrong heights (blocks in random
                 * file order) or picked a wrong "most work" chain due to
                 * incomplete nChainTx propagation.  Use SQLite blocks table
                 * to find the correct height, then set the active chain tip
                 * to the coins-tip block.  This fires when:
                 *   (a) active_chain is empty (no HAVE_DATA blocks), OR
                 *   (b) active_chain tip is far below the coins tip
                 *       (scan picked a wrong short fork). */
                struct uint256 post_scan_best;
                coins_view_cache_get_best_block(&g_coins_tip, &post_scan_best);
                /* Restore chain tip to match UTXO snapshot height when
                 * the active chain is far below the coins tip. This happens
                 * after LDB import: the UTXO set is at 3M+ but block files
                 * only cover up to ~2M, so activate_best_chain sets a low
                 * tip. Without this fix, the node tries to re-connect
                 * blocks that are already reflected in the UTXO set, causing
                 * bad-txns-inputs-missingorspent failures. */
                if (!uint256_is_null(&post_scan_best)) {
                    /* Look up correct height from SQLite */
                    int target_h = -1;
                    if (g_node_db.open && g_node_db.db) {
                        /* Look up import height from SQLite blocks table.
                         * blocks.hash stores display-order (big-endian),
                         * coins_best_block is internal-order (little-endian). */
                        uint8_t hash_rev[32];
                        for (int bi = 0; bi < 32; bi++)
                            hash_rev[bi] = post_scan_best.data[31 - bi];

                        struct db_block sqlite_blk;
                        if (db_block_find_by_hash(&g_node_db, hash_rev,
                                                   &sqlite_blk) &&
                            sqlite_blk.height > 0) {
                            target_h = sqlite_blk.height;
                        }

                        /* Fallback: try finding by height range near chain tip */
                        if (target_h <= 0) {
                            sqlite3_stmt *qs = NULL;
                            sqlite3_prepare_v2(g_node_db.db,
                                "SELECT height FROM blocks "
                                "ORDER BY height DESC LIMIT 1",
                                -1, &qs, NULL);
                            if (qs) {
                                if (sqlite3_step(qs) == SQLITE_ROW)
                                    target_h = sqlite3_column_int(qs, 0);
                                sqlite3_finalize(qs);
                            }
                            if (target_h > 0)
                                printf("Post-scan: using max block height "
                                       "%d as import target\n", target_h);
                        }

                        if (target_h > 0)
                            printf("Post-scan: import height=%d\n", target_h);
                    }

                    struct block_index *post_found = block_map_find(
                        &g_state.map_block_index, &post_scan_best);
                    if (post_found && target_h > 0) {
                        /* Fix the block's height from SQLite truth */
                        if (post_found->nHeight != target_h) {
                            printf("Post-scan: correcting block nHeight "
                                   "%d→%d from SQLite\n",
                                   post_found->nHeight, target_h);
                            post_found->nHeight = target_h;
                        }
                        printf("Post-scan: setting chain tip to h=%d\n",
                               target_h);
                        active_chain_set_tip(&g_state.chain_active, post_found);
                        g_state.pindex_best_header = post_found;
                    } else if (!post_found) {
                        /* coins_best_block hash not found in block index.
                         * Instead of wiping UTXOs, find the highest UTXO
                         * height and set chain tip there.  The UTXO data
                         * is valid — only the metadata label is wrong. */
                        char hex[65];
                        uint256_get_hex(&post_scan_best, hex);
                        printf("[boot] coins_best_block %s not in "
                               "block index — resolving from UTXO "
                               "heights\n", hex);

                        int utxo_max_h = 0;
                        {
                            sqlite3_stmt *hst = NULL;
                            if (sqlite3_prepare_v2(g_node_db.db,
                                "SELECT MAX(height) FROM utxos",
                                -1, &hst, NULL) == SQLITE_OK && hst) {
                                if (sqlite3_step(hst) == SQLITE_ROW)
                                    utxo_max_h = sqlite3_column_int(hst, 0);
                                sqlite3_finalize(hst);
                            }
                        }

                        if (utxo_max_h > 0) {
                            /* Find highest HAVE_DATA block at or below
                             * the UTXO height — conservative but safe. */
                            struct block_index *best_have = NULL;
                            size_t bi = 0;
                            struct block_index *bp;
                            while (block_map_next(
                                &g_state.map_block_index,
                                &bi, NULL, &bp)) {
                                if (!bp) continue;
                                if (bp->nHeight <= utxo_max_h &&
                                    (bp->nStatus & BLOCK_HAVE_DATA) &&
                                    (!best_have ||
                                     bp->nHeight > best_have->nHeight))
                                    best_have = bp;
                            }

                            if (best_have && best_have->nHeight > 0) {
                                active_chain_set_tip(
                                    &g_state.chain_active, best_have);
                                g_state.pindex_best_header = best_have;
                                /* Update coins_best_block to match */
                                if (best_have->phashBlock) {
                                    coins_view_cache_set_best_block(
                                        &g_coins_tip,
                                        best_have->phashBlock);
                                    node_db_state_set(&g_node_db,
                                        "coins_best_block",
                                        best_have->phashBlock->data, 32);
                                }
                                printf("[boot] coins_best_block hash not "
                                       "in index — setting tip to highest "
                                       "HAVE_DATA block at h=%d\n",
                                       best_have->nHeight);
                            } else {
                                /* No HAVE_DATA blocks — create anchor
                                 * at UTXO height (same as import_ldb) */
                                struct block_index *anchor = zcl_calloc(1,
                                    sizeof(struct block_index),
                                    "boot coins_best_block anchor");
                                if (anchor) {
                                    block_index_init(anchor);
                                    anchor->nHeight = utxo_max_h;
                                    anchor->nStatus = BLOCK_VALID_TREE
                                        | BLOCK_HAVE_DATA;
                                    anchor->nChainTx = 1;
                                    anchor->nTx = 1;
                                    struct arith_uint256 max_w;
                                    arith_uint256_set_u64(&max_w, 0);
                                    size_t wi = 0;
                                    struct block_index *wp;
                                    while (block_map_next(
                                        &g_state.map_block_index,
                                        &wi, NULL, &wp)) {
                                        if (!wp) continue;
                                        if (arith_uint256_compare(
                                            &wp->nChainWork,
                                            &max_w) > 0)
                                            max_w = wp->nChainWork;
                                    }
                                    struct arith_uint256 margin;
                                    arith_uint256_set_u64(&margin,
                                        4096ULL * (uint64_t)utxo_max_h);
                                    arith_uint256_add(&anchor->nChainWork,
                                        &max_w, &margin);

                                    block_map_insert(
                                        &g_state.map_block_index,
                                        &post_scan_best, anchor);
                                    anchor->phashBlock = block_map_find_hash(
                                        &g_state.map_block_index,
                                        &post_scan_best);

                                    snapsync_set_anchor(anchor);
                                    active_chain_set_tip(
                                        &g_state.chain_active, anchor);
                                    g_state.pindex_best_header = anchor;

                                    printf("[boot] coins_best_block hash "
                                           "not in index — anchor at "
                                           "h=%d\n", utxo_max_h);
                                }
                            }
                        } else {
                            /* No UTXOs at all — safe to reset to genesis */
                            printf("[boot] No UTXOs found — resetting to "
                                   "genesis\n");
                            coins_view_cache_set_best_block(&g_coins_tip,
                                &params->consensus.hashGenesisBlock);
                            node_db_state_set(&g_node_db, "coins_best_block",
                                params->consensus.hashGenesisBlock.data, 32);
                            struct block_index *genesis = block_map_find(
                                &g_state.map_block_index,
                                &params->consensus.hashGenesisBlock);
                            if (genesis) {
                                active_chain_set_tip(
                                    &g_state.chain_active, genesis);
                                g_state.pindex_best_header = genesis;
                            }
                        }
                    }
                }
            }
        }
    }

    t_phase = boot_clock_ms();
    /* Load Sapling commitment tree from persistent storage.
     * This tree is maintained by connect_block and verified against
     * hashFinalSaplingRoot in each block header. */
    if (g_node_db.open && !g_state.sapling_tree_loaded) {
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        if (node_db_state_get(&g_node_db, "sapling_tree",
                               tree_buf, sizeof(tree_buf), &tree_len)
            && tree_len > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            sapling_tree_init(&g_state.sapling_tree);
            if (incremental_tree_deserialize(&g_state.sapling_tree, &ts)) {
                g_state.sapling_tree_loaded = true;
                set_sapling_tree_for_flush(&g_state.sapling_tree);
                printf("Sapling tree loaded: %zu commitments\n",
                       incremental_tree_size(&g_state.sapling_tree));
            } else {
                fprintf(stderr, "WARNING: Sapling tree deserialization "
                        "failed — tree will rebuild during sync\n");
                sapling_tree_init(&g_state.sapling_tree);
            }
        } else {
            printf("No saved Sapling tree — will build during sync\n");
            g_state.sapling_tree_loaded = true; /* empty tree is valid pre-Sapling */
            set_sapling_tree_for_flush(&g_state.sapling_tree);
        }
    }

    /* Verify Sapling tree root matches chain tip. If mismatched,
     * rebuild from block files before P2P starts (no concurrency risk).
     * Skip if hashFinalSaplingRoot is all-zeros (block_index.bin doesn't
     * store this field yet, so it will be zero after flat file load). */
    if (g_state.sapling_tree_loaded && g_datadir) {
        const struct block_index *tip = active_chain_tip(&g_state.chain_active);
        static const uint8_t zeros[32] = {0};
        bool tip_has_sapling_root = tip && tip->nHeight > 476969 &&
            memcmp(tip->hashFinalSaplingRoot.data, zeros, 32) != 0;
        if (tip_has_sapling_root) {
            struct uint256 tree_root;
            incremental_tree_root(&g_state.sapling_tree, &tree_root);
            if (memcmp(tree_root.data,
                       tip->hashFinalSaplingRoot.data, 32) != 0) {
                size_t old_size = incremental_tree_size(&g_state.sapling_tree);
                printf("Sapling tree root MISMATCH (size=%zu) — "
                       "rebuilding from block files...\n", old_size);
                fflush(stdout);
                atomic_store(&g_sapling_tree_rebuilding, true);
                int n = sapling_tree_rebuild(&g_node_db,
                    &g_state.chain_active, g_datadir);
                if (n >= 0) {
                    /* Reload the rebuilt tree from node_state */
                    uint8_t tbuf[8192];
                    size_t tlen = 0;
                    if (node_db_state_get(&g_node_db, "sapling_tree",
                            tbuf, sizeof(tbuf), &tlen) && tlen > 0) {
                        struct byte_stream ts2;
                        stream_init_from_data(&ts2, tbuf, tlen);
                        sapling_tree_init(&g_state.sapling_tree);
                        incremental_tree_deserialize(
                            &g_state.sapling_tree, &ts2);
                        set_sapling_tree_for_flush(&g_state.sapling_tree);
                        printf("Sapling tree rebuilt: %d commitments "
                               "(was %zu)\n", n, old_size);
                    }
                }
                atomic_store(&g_sapling_tree_rebuilding, false);
                /* Checkpoint WAL after bulk tree writes */
                node_db_wal_checkpoint(&g_node_db);
                /* Save block_index.bin after rebuild — the entries
                 * now have correct hashFinalSaplingRoot fields from
                 * the rebuild. This prevents needless 5-min rebuilds
                 * on future boots AND ensures coins_best_block will
                 * be resolvable after a crash. */
                save_block_index_flat(ctx->datadir, &g_state);
            }
        }
    }

    printf("[boot] %-30s %lldms\n", "sapling_tree_load",
           (long long)(boot_clock_ms() - t_phase));

    /* Clear BLOCK_FAILED flags above the chain tip on boot.
     * After a UTXO repair or crash recovery, blocks may be marked
     * BLOCK_FAILED_VALID/BLOCK_FAILED_CHILD from a prior session where
     * the UTXO set was incomplete. With the UTXO set now repaired,
     * these blocks should be re-validated. Without this, activate_best_chain
     * skips them and the node is permanently stuck. */
    {
        struct block_index *tip = active_chain_tip(&g_state.chain_active);
        int tip_h = tip ? tip->nHeight : 0;
        size_t iter_f = 0;
        struct block_index *bi_f;
        int cleared_failed = 0;
        while (block_map_next(&g_state.map_block_index, &iter_f, NULL, &bi_f)) {
            if (!bi_f) continue;
            if (bi_f->nHeight > tip_h &&
                (bi_f->nStatus & BLOCK_FAILED_MASK)) {
                bi_f->nStatus &= ~BLOCK_FAILED_MASK;
                cleared_failed++;
            }
        }
        if (cleared_failed > 0)
            printf("Boot: cleared BLOCK_FAILED on %d blocks above tip h=%d\n",
                   cleared_failed, tip_h);
    }

    /* Clean up UTXOs above chain tip (utxo_recovery_service) */
    utxo_recovery_clean_above_tip(&g_node_db, &g_state);

    /* Safety: verify chain tip matches UTXO set height.
     * After LDB import, the UTXO set is at ~3M but the chain tip may be
     * lower (~2M) if previous boots failed to set the anchor.  If the
     * coins tip is far above the chain tip, correct it now. */
    {
        struct uint256 coins_hash;
        coins_view_cache_get_best_block(&g_coins_tip, &coins_hash);
        int chain_h = active_chain_height(&g_state.chain_active);
        struct block_index *coins_bi = NULL;

        if (!uint256_is_null(&coins_hash))
            coins_bi = block_map_find(&g_state.map_block_index, &coins_hash);

        if (coins_bi && coins_bi->nHeight > chain_h + 100) {
            printf("[boot] UTXO/chain mismatch: coins at h=%d, "
                   "chain tip at h=%d — correcting\n",
                   coins_bi->nHeight, chain_h);
            active_chain_set_tip(&g_state.chain_active, coins_bi);
            g_state.pindex_best_header = coins_bi;
            printf("[boot] Chain tip corrected to h=%d\n",
                   coins_bi->nHeight);
        } else if (!coins_bi && !uint256_is_null(&coins_hash)) {
            /* coins_best_block hash not in block index. Find the highest
             * block with BLOCK_HAVE_DATA as our best anchor point. The
             * UTXO set should be valid somewhere near that height. */
            struct block_index *best_have_data = NULL;
            size_t iter = 0;
            struct block_index *bi;
            while (block_map_next(&g_state.map_block_index, &iter,
                                   NULL, &bi)) {
                if (!bi) continue;
                if (!(bi->nStatus & BLOCK_HAVE_DATA)) continue;
                if (!best_have_data ||
                    bi->nHeight > best_have_data->nHeight)
                    best_have_data = bi;
            }
            if (best_have_data && best_have_data->nHeight > chain_h + 100) {
                printf("[boot] coins_best_block not in index — "
                       "using highest HAVE_DATA block at h=%d\n",
                       best_have_data->nHeight);
                active_chain_set_tip(&g_state.chain_active, best_have_data);
                g_state.pindex_best_header = best_have_data;
            }
        }
    }

    /* Activate best chain via controller (single authority).
     * The controller checks: anchor state, shutdown, UTXO availability.
     * Replaces the old skip_activate boolean with state machine. */
    if (activation_get_state(&g_activation_ctl) == ACTIVATION_BOOT_PENDING)
        activation_boot_complete(&g_activation_ctl, "boot_done");

    /* If anchor exists but chain tip is already past it (previous boot
     * synced successfully), clear the anchor so blocks can connect. */
    if (activation_get_state(&g_activation_ctl) == ACTIVATION_ANCHOR_ACTIVE) {
        struct block_index *tip = active_chain_tip(&g_state.chain_active);
        struct block_index *anc = snapsync_get_anchor();
        if (tip && anc && tip->nHeight > anc->nHeight) {
            printf("Anchor at h=%d below chain tip h=%d — clearing\n",
                   anc->nHeight, tip->nHeight);
            snapsync_set_anchor(NULL);
            activation_clear_anchor(&g_activation_ctl, "tip_past_anchor");
        }
    }
    {
        struct activation_exec_outcome outcome;
        activation_request_connect(&g_activation_ctl, ACTIVATION_SRC_BOOT,
                                   NULL, &outcome);
        if (outcome.result == ACTIVATION_EXEC_FAILED)
            fprintf(stderr, "Warning: Failed to activate best chain: %s\n",
                    outcome.reason);
    }

    /* Auto-scan wallet for transactions in connected blocks.
     * This ensures balance shows immediately after LDB import or
     * snapshot sync — no manual replaywalletfromchain needed.
     * A power node should just work. */
    {
        int tip_h = active_chain_height(&g_state.chain_active);
        if (tip_h > 0 && g_node_db.open) {
            int found = wallet_scan_blocks(&g_node_db,
                &g_state.chain_active, &g_wallet, ctx->datadir,
                0, tip_h);
            if (found > 0)
                printf("Wallet: auto-discovered %d transactions "
                       "(blocks 0-%d)\n", found, tip_h);
        }
    }

    /* Restore normal SQLite settings after any IBD replay */
    if (g_node_db.open) {
        if (!boot_db_restore_normal_mode())
            fprintf(stderr, "boot: failed to restore normal mode\n");
        if (!boot_db_set_sync_batch_size(1))
            fprintf(stderr, "boot: failed to reset sync batch size\n");
    }
    /* Flush every 500 blocks during normal sync so crash/kill never
     * loses more than ~500 blocks of connected coins state. */
    set_flush_policy(3600, 500000, 500);

    struct block_index *tip = active_chain_tip(&g_state.chain_active);
    if (tip && tip->phashBlock) {
        if (g_node_db.open &&
            !node_db_sync_set_tip(&g_node_db, tip->phashBlock->data,
                                  tip->nHeight)) {
            fprintf(stderr, "boot: failed to persist final chain tip\n");
        }
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        printf("Chain tip: height=%d hash=%s\n", tip->nHeight, hex);
        event_emitf(EV_BOOT_ACTIVATE, 0, "done tip=%d", tip->nHeight);

        /* Auto-extend assumevalid to cover the startup chain tip.
         * Everything already in the chainstate was validated by either:
         * - zclassicd (via legacy import)
         * - a previous run of zclassic23
         * - the coins DB (trusted LevelDB state)
         * Only blocks received via P2P AFTER startup need new validation.
         * With Groth16 pairing fix deployed, all proofs above the checkpoint
         * are now verified correctly — no need to extend assume-valid. */
    } else {
        printf("Chain tip: genesis\n");
    }

    /* Backfill shielded values (utxo_recovery_service) */
    if (g_node_db.open && tip && tip->nHeight > 1000 && !ctx->no_services) {
        int64_t shielded_count = 0;
        {
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(g_node_db.db,
                    "SELECT COUNT(*) FROM blocks WHERE sprout_value != 0 OR sapling_value != 0",
                    -1, &s, NULL) == SQLITE_OK) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    shielded_count = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
        }
        if (shielded_count < 1000 && tip->nHeight > 100000)
            utxo_recovery_backfill_shielded(&g_node_db,
                boot_runtime_db_service(), &g_state, g_datadir);
    }

    /* Skip services if no_services flag is set (speedrun / benchmarking) */
    if (ctx->no_services) {
        printf("Boot complete (no_services mode). "
               "Chain tip: height=%d\n",
               active_chain_height(&g_state.chain_active));
        return true;
    }

    /* Runtime services: mempool, P2P, RPC, Tor, wallet sync (boot_services.c) */
    struct boot_svc_ctx svc = {
        .state = &g_state,
        .coins_sqlite = &g_coins_sqlite,
        .coins_tip = &g_coins_tip,
        .mempool = &g_mempool,
        .rpc_table = &g_rpc_table,
        .msg_processor = &g_msg_processor,
        .connman = &g_connman,
        .wallet = &g_wallet,
        .gen = &g_gen,
        .wallet_sqlite = &g_wallet_sqlite,
        .node_db = &g_node_db,
        .db_service = &g_db_service,
        .metrics = &g_metrics,
        .running = &g_running,
        .datadir = g_datadir,
        .params_thread = g_params_thread,
        .params_thread_started = g_params_thread_started,
        .params_loaded = &g_params_loaded,
        .block_tree_open = g_block_tree_open,
        .block_tree = &g_block_tree,
        .want_address_backfill = false,
        .want_snapshot_tx_index = ctx->snapshot_dir != NULL,
        .defer_payment_service = false,
        .defer_offer_service = false,
    };
    if (g_node_db.open) {
        int64_t addr_done = 0;
        node_db_state_get_int(&g_node_db, "addresses_backfilled", &addr_done);
        svc.want_address_backfill = (addr_done == 0);
    }
    /* g_svc is stored so app_shutdown can access it */
    g_svc = svc;

    /* Emit crash recovery complete if boot succeeded */
    {
        int chain_h = active_chain_height(&g_state.chain_active);
        event_emitf(EV_CRASH_RECOVERY_COMPLETE, 0,
            "chain_height=%d", chain_h);
    }

    t_phase = boot_clock_ms();
    bool svc_ok = app_init_services(ctx, params, &g_svc);
    printf("[boot] %-30s %lldms\n", "p2p_services_start",
           (long long)(boot_clock_ms() - t_phase));
    printf("[boot] %-30s %lldms\n", "total",
           (long long)(boot_clock_ms() - t_boot_start));
    return svc_ok;
}

/* Write clean shutdown marker before shutting down */
static void write_clean_shutdown_marker(void)
{
    if (!g_datadir) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.shutdown_clean", g_datadir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%ld\n", (long)time(NULL));
        fclose(f);
    }
}

/* app_shutdown delegates to boot_services.c */
void app_shutdown(void)
{
    write_clean_shutdown_marker();
    app_shutdown_svc(&g_svc);
    release_datadir_lock();
}
bool app_is_running(void) { return atomic_load(&g_running); }

/* app_add_node, app_start_metrics, app_stop_metrics: boot_services.c */
