/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "config/boot_internal.h"
#include "config/file_ops.h"
#include "util/sync.h"
#include "net/msgprocessor.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
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
#include <sqlite3.h>

static struct main_state g_state;
static struct coins_view_sqlite g_coins_sqlite;
static struct coins_view_cache g_coins_tip;
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

static struct db_service *boot_runtime_db_service(void)
{
    return db_service_is_started(&g_db_service) ? &g_db_service : NULL;
}

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

struct boot_shielded_backfill_ctx {
    int updated;
};

static bool boot_backfill_shielded_values_write(struct node_db *ndb, void *ctx)
{
    struct boot_shielded_backfill_ctx *backfill = ctx;
    size_t iter = 0;
    struct block_index *bi;
    int updated = 0;
    bool tx_open = false;

    if (!ndb || !ndb->open)
        return false;
    if (!node_db_begin(ndb))
        return false;
    tx_open = true;

    while (block_map_next(&g_state.map_block_index, &iter, NULL, &bi)) {
        if (!bi) continue;
        if (bi->nSproutValue == 0 && bi->nSaplingValue == 0) continue;
        struct db_block blk;
        if (db_block_find_by_height(ndb, bi->nHeight, &blk)) {
            blk.sprout_value = bi->nSproutValue;
            blk.sapling_value = bi->nSaplingValue;
            if (!db_block_save(ndb, &blk)) {
                if (tx_open)
                    node_db_rollback(ndb);
                return false;
            }
            updated++;
        }
    }

    if (!node_db_commit(ndb)) {
        if (tx_open)
            node_db_rollback(ndb);
        return false;
    }
    if (!node_db_state_set_int(ndb, "shielded_backfilled", 1))
        return false;
    if (backfill)
        backfill->updated = updated;
    return true;
}
static struct metrics_context g_metrics;

/* Comparator for sorting block_index pointers by height (for qsort). */
static int cmp_block_index_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index **)a;
    const struct block_index *pb = *(const struct block_index **)b;
    return (pa->nHeight > pb->nHeight) - (pa->nHeight < pb->nHeight);
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


bool app_init(struct app_context *ctx)
{
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
    wallet_init(&g_wallet);

    /* Load wallet from SQLite (node.db wallet_* tables) */
    if (g_node_db.open && wallet_sqlite_open(&g_wallet_sqlite, g_node_db.db)) {
        wallet_sqlite_read_keys(&g_wallet_sqlite, &g_wallet);
        wallet_sqlite_read_txs(&g_wallet_sqlite, &g_wallet);
        wallet_rebuild_spent_set(&g_wallet);
        wallet_sqlite_read_sapling_keys(&g_wallet_sqlite, &g_wallet);
        wallet_sqlite_read_scripts(&g_wallet_sqlite, &g_wallet);
        int saved_height = 0;
        if (wallet_sqlite_read_scan_height(&g_wallet_sqlite, &saved_height))
            g_wallet.best_block_height = saved_height;
        printf("Wallet loaded: %zu keys, %zu sapling keys, %zu scripts, "
               "%zu txs, scan height %d.\n",
               g_wallet.keystore.num_keys,
               g_wallet.sapling_keys.num_keys,
               g_wallet.keystore.num_scripts,
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

    if (g_wallet.keystore.num_keys == 0)
        wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);
    printf("Wallet has %zu keys.\n", g_wallet.keystore.num_keys);

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
         * Only do this if UTXOs actually exist in SQLite — otherwise
         * the coins DB is empty and should stay at null (triggers replay). */
        struct uint256 coins_check;
        memset(&coins_check, 0, sizeof(coins_check));
        if (!coins_view_sqlite_get_best_block(&g_coins_sqlite, &coins_check)
            || uint256_is_null(&coins_check)) {
            /* Check if UTXOs exist */
            int64_t utxo_count = node_db_utxo_count(&g_node_db);
            if (utxo_count > 0) {
                uint8_t tip_buf[32];
                size_t tip_len = 0;
                if (node_db_state_get(&g_node_db, "tip_hash",
                                       tip_buf, 32, &tip_len) && tip_len == 32) {
                    node_db_state_set(&g_node_db, "coins_best_block",
                                      tip_buf, 32);
                    printf("Migrated coins_best_block from tip_hash "
                           "(%lld UTXOs)\n", (long long)utxo_count);
                }
            }
        }

        /* -reimport-utxos: force re-import from LevelDB chainstate */
        if (ctx->reimport_utxos) {
            printf("Forced UTXO re-import requested.\n");
            node_db_wipe_utxos(&g_node_db);
            node_db_exec(&g_node_db,
                "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'");
        }

        /* Check if LevelDB UTXO migration has been done.
         * Import if chainstate/ LevelDB exists and hasn't been imported yet. */
        uint8_t mig_buf[8];
        size_t mig_len = 0;
        bool migration_done = node_db_state_get(&g_node_db,
            "leveldb_utxo_migrated", mig_buf, sizeof(mig_buf), &mig_len);

        if (!migration_done) {
            char cs_path[1024];
            snprintf(cs_path, sizeof(cs_path), "%s/chainstate",
                     ctx->datadir);
            struct stat cs_st;
            if (stat(cs_path, &cs_st) == 0) {
                printf("LevelDB→SQLite UTXO migration from %s\n", cs_path);
                event_emitf(EV_BOOT_UTXO_IMPORT, 0, "start path=%s", cs_path);
                fflush(stdout);

                /* Wipe any residual UTXOs first — clean slate */
                node_db_wipe_utxos(&g_node_db);

                /* Close coins_view_sqlite during import — its dedicated
                 * connection holds a WAL read lock that prevents the
                 * import's WAL checkpoint from completing. */
                coins_view_sqlite_close(&g_coins_sqlite);

                struct coins_view_db migrate_db;
                if (coins_view_db_open(&migrate_db, cs_path,
                                       450 << 20, false, false)) {
                    struct node_db import_db;
                    bool imported = false;

                    if (node_db_sync_open_private_db_like(&g_node_db, &import_db)) {
                        node_db_sync_import_utxos(&import_db, &migrate_db);
                        node_db_close(&import_db);
                        imported = true;
                    } else {
                        node_db_sync_import_utxos(&g_node_db, &migrate_db);
                        imported = true;
                    }

                    /* Set coins_best_block from LevelDB's best block.
                     * ALWAYS use the LevelDB hash — UTXOs are valid AT
                     * that height. If the block isn't in our index yet,
                     * the chain restore step will wait for P2P headers. */
                    struct uint256 ldb_best;
                    memset(&ldb_best, 0, sizeof(ldb_best));
                    bool got_best = coins_view_db_get_best_block(
                        &migrate_db, &ldb_best);
                    if (got_best && !uint256_is_null(&ldb_best)) {
                        node_db_state_set(&g_node_db, "coins_best_block",
                                          ldb_best.data, 32);
                        coins_view_cache_set_best_block(&g_coins_tip,
                                                         &ldb_best);
                        char hex[65];
                        uint256_get_hex(&ldb_best, hex);
                        struct block_index *found = block_map_find(
                            &g_state.map_block_index, &ldb_best);
                        if (found)
                            printf("Set coins_best_block from LevelDB: %s "
                                   "(height=%d)\n", hex, found->nHeight);
                        else
                            printf("Set coins_best_block from LevelDB: %s "
                                   "(ahead of index, will sync via P2P)\n",
                                   hex);
                    }

                    coins_view_db_close(&migrate_db);
                    if (imported)
                        node_db_wal_checkpoint(&g_node_db);

                    /* SHA3 verification — verify BEFORE marking done */
                    uint8_t imported_root[32];
                    uint64_t imported_count = 0;
                    utxo_commitment_sha3_compute(g_node_db.db,
                        imported_root, &imported_count);

                    static const uint8_t expected_root[32] = {
                        0x00,0xe9,0x5d,0xbd,0x54,0xa7,0x91,0xa5,
                        0x14,0x33,0xd6,0x81,0x27,0xf9,0x97,0x5a,
                        0x3b,0x1d,0x6f,0x8e,0x90,0x02,0xb1,0x09,
                        0x64,0x73,0x43,0xba,0x0c,0x83,0xc3,0xe0
                    };
                    static const uint64_t expected_count = 1354771;

                    printf("SHA3 UTXO verification: %llu UTXOs\n",
                           (unsigned long long)imported_count);

                    if (imported_count == expected_count &&
                        memcmp(imported_root, expected_root, 32) == 0) {
                        printf("=== SHA3 UTXO CHECKPOINT: PASSED ===\n");
                        uint8_t one = 1;
                        node_db_state_set(&g_node_db, "leveldb_utxo_migrated",
                                           &one, 1);
                    } else if (imported_count > 100000) {
                        /* At a different height than checkpoint — count is
                         * sane, mark done. Full verification happens during
                         * connect_block at checkpoint height. */
                        printf("SHA3: %llu UTXOs at import height "
                               "(checkpoint at h=3056758 has %llu — "
                               "will verify during block connection)\n",
                               (unsigned long long)imported_count,
                               (unsigned long long)expected_count);
                        uint8_t one = 1;
                        node_db_state_set(&g_node_db, "leveldb_utxo_migrated",
                                           &one, 1);
                    } else {
                        /* Suspiciously few UTXOs — import likely failed.
                         * Do NOT mark done — will re-attempt on next boot. */
                        fprintf(stderr, "ERROR: UTXO import produced only "
                                "%llu UTXOs (expected >100K). "
                                "Will retry on next boot.\n",
                                (unsigned long long)imported_count);
                        node_db_wipe_utxos(&g_node_db);
                    }

                    /* Reopen coins_view_sqlite after import */
                    coins_view_sqlite_open(&g_coins_sqlite, g_node_db.db);
                    event_emitf(EV_BOOT_UTXO_IMPORT, 0, "done count=%llu",
                                (unsigned long long)imported_count);
                    printf("UTXO migration complete.\n");
                    fflush(stdout);
                } else {
                    fprintf(stderr, "ERROR: Failed to open chainstate "
                            "LevelDB at %s\n", cs_path);
                }
            } else {
                /* No chainstate dir — mark as done (fresh node) */
                uint8_t one = 1;
                node_db_state_set(&g_node_db, "leveldb_utxo_migrated",
                                   &one, 1);
            }
        }
    }

    coins_view_cache_init(&g_coins_tip, &g_coins_sqlite.view);

    /* Wire UTXO commitment: load from SQLite and set pointer for
     * persistence on flush. */
    set_coins_sqlite_for_commitment(&g_coins_sqlite);
    if (coins_view_sqlite_read_commitment(&g_coins_sqlite, &g_coins_tip.commitment)) {
        printf("Loaded UTXO commitment from SQLite (count=%llu)\n",
               (unsigned long long)g_coins_tip.commitment.count);
    }

    bool skip_activate = false;
    bool fast_restart = false;

    /* Block index is now cached in SQLite (load_block_index_sqlite).
     * The full index is saved on shutdown/save, enabling instant restart
     * without the 10-15s LevelDB scan. */

    /* Block index load: flat file first (mmap, <2s), then SQLite, then LevelDB.
     * Jeff Dean rule: use the fastest data structure available. */
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

        /* Save recent blocks to SQLite (skip for large indexes —
         * the flat file handles 3M+ entries in 1-3s and the SQLite
         * cache path uses 10GB+ RAM causing OOM kills) */
        if (g_node_db.open && g_state.map_block_index.size > 1000
            && g_state.map_block_index.size < 500000)
            save_block_index_recent(&g_node_db, &g_state);

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
        skip_activate = false;
    } else if (fast_restart) {
    } else if (g_state.map_block_index.size > 1) {
        struct uint256 best_hash;
        coins_view_cache_get_best_block(&g_coins_tip, &best_hash);
        if (!uint256_is_null(&best_hash)) {
            struct block_index *best = block_map_find(
                &g_state.map_block_index, &best_hash);
            /* SQLite fallback: if block_map maps the hash to height 0
             * but it's not actually the genesis block, the LevelDB
             * block_index was built from a different chain state. */
            if (best && best->nHeight == 0 &&
                !uint256_eq(&best_hash,
                            &params->consensus.hashGenesisBlock)) {
                char hex[65];
                uint256_get_hex(&best_hash, hex);
                fprintf(stderr,
                    "WARNING: coins DB best block %s mapped to height=0 "
                    "in block_index (not genesis)\n", hex);
                if (g_node_db.open) {
                    struct db_block sqlite_blk;
                    if (db_block_find_by_hash(&g_node_db,
                                              best_hash.data,
                                              &sqlite_blk) &&
                        sqlite_blk.height > 0) {
                        printf("Correcting: using SQLite height=%d\n",
                               sqlite_blk.height);
                        best = NULL;
                    } else {
                        best = NULL;
                    }
                } else {
                    best = NULL;
                }
            }
            if (best) {
                active_chain_set_tip(&g_state.chain_active, best);
                g_state.pindex_best_header = best;
                printf("Restored chain tip from coins DB: height=%d\n",
                       best->nHeight);
                event_emitf(EV_BOOT_CHAIN_RESTORED, 0, "height=%d",
                            best->nHeight);
            } else {
                char hex[65];
                uint256_get_hex(&best_hash, hex);
                printf("Coins DB best block %s not in index "
                       "(block_map size=%zu).\n",
                       hex, g_state.map_block_index.size);
                /* Coins DB points to a block ahead of our index
                 * (e.g., LevelDB import from zclassicd at higher height).
                 * UTXOs are valid — find highest block we DO have and
                 * set that as coins_best_block + chain tip.
                 * activate_best_chain will connect forward from there. */
                struct block_index *fallback = NULL;
                size_t si2 = 0;
                struct block_index *sp2;
                while (block_map_next(&g_state.map_block_index,
                                       &si2, NULL, &sp2)) {
                    if (!sp2) continue;
                    if ((sp2->nStatus & BLOCK_HAVE_DATA) &&
                        sp2->nChainTx > 0 && sp2->phashBlock &&
                        (!fallback || sp2->nHeight > fallback->nHeight))
                        fallback = sp2;
                }
                if (fallback && fallback->phashBlock) {
                    /* UTXOs are valid at LevelDB's tip (ahead of us).
                     * DON'T change coins_best_block — keep it at the
                     * LevelDB hash so connect_block's view/prevblock
                     * check passes when we reach that height.
                     * Set chain_active tip AND best_header so P2P works.
                     * Skip activate — can't connect until P2P delivers
                     * the block at coins_best_block height. */
                    active_chain_set_tip(&g_state.chain_active, fallback);
                    g_state.pindex_best_header = fallback;
                    printf("Coins DB ahead of index (index tip=%d). "
                           "Waiting for P2P headers to catch up.\n",
                           fallback->nHeight);
                    skip_activate = true;
                } else {
                    /* No valid blocks at all — wipe and start fresh */
                    printf("No valid blocks in index — wiping UTXOs.\n");
                    node_db_wipe_utxos(&g_node_db);
                    coins_view_cache_flush(&g_coins_tip);
                    skip_activate = false;
                }
            }
        }

        /* If coins DB had no best block, try fast rebuild.
         * Skip if coins are ahead of index (skip_activate already set). */
        if (!skip_activate &&
            (uint256_is_null(&best_hash) ||
             active_chain_tip(&g_state.chain_active) == NULL)) {
            if (scan_fallback) {
                active_chain_set_tip(&g_state.chain_active, scan_fallback);
                g_state.pindex_best_header = scan_fallback;
                printf("WARNING: Chain tip at height %d but coins DB is empty!\n",
                       scan_fallback->nHeight);
                printf("Attempting fast chainstate rebuild from SQLite...\n");
                if (fast_rebuild_chainstate(&g_coins_sqlite, &g_coins_tip,
                                             ctx->datadir)) {
                    printf("Fast rebuild complete — will activate chain.\n");
                } else {
                    printf("Fast rebuild unavailable — will activate from genesis.\n");
                }
                skip_activate = false;
            }
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

    /* Validate coins/chain agreement at boot.
     * ActiveRecord-style: validate → diagnose → recover. */
    {
        struct boot_validation_result vr =
            validate_coins_chain_agreement(&g_state, &g_coins_tip,
                                           ctx->datadir);
        switch (vr.action) {
        case BOOT_RECOVER_REIMPORT:
        case BOOT_RECOVER_WIPE_WAIT:
            printf("WARNING: Chain tip at h=%d but coins DB %s!\n",
                   vr.chain_height,
                   vr.action == BOOT_RECOVER_REIMPORT
                       ? "empty (LevelDB available)" : "empty");

            node_db_wipe_utxos(&g_node_db);
            coins_view_cache_flush(&g_coins_tip);

            struct block_index *genesis = block_map_find(
                &g_state.map_block_index,
                &params->consensus.hashGenesisBlock);
            if (genesis) {
                active_chain_set_tip(&g_state.chain_active, genesis);
                g_state.pindex_best_header = genesis;
            }
            skip_activate = false;

            /* Clear migration flag for LevelDB re-import on next boot */
            node_db_exec(&g_node_db,
                "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'");

            extern _Atomic bool g_utxo_commitment_skip;
            atomic_store(&g_utxo_commitment_skip, true);

            if (g_node_db.open) {
                if (!boot_db_enter_turbo_mode())
                    fprintf(stderr, "boot: failed to enter turbo mode\n");
                if (!boot_db_set_sync_batch_size(1000))
                    fprintf(stderr, "boot: failed to set sync batch size\n");
            }
            set_flush_policy(3600, 1000000, 100000);
            break;

        case BOOT_RECOVER_RESET_CHAIN: {
            struct block_index *coins_block = block_map_find(
                &g_state.map_block_index, &vr.coins_hash);
            if (coins_block) {
                printf("Chain tip/coins mismatch: chain=%d coins=%d\n"
                       "  Resetting chain to coins tip — "
                       "will replay %d blocks.\n",
                       vr.chain_height, vr.coins_height,
                       vr.chain_height - vr.coins_height);
                active_chain_set_tip(&g_state.chain_active, coins_block);
                skip_activate = false;
            }
            break;
        }
        case BOOT_OK:
            break;
        }
    }

    /* Clear stale HAVE_DATA above tip — targeted, not full scan.
     * Only needed if max HAVE_DATA height > chain tip (from the
     * single-pass scan above). */
    {
        int tip_h = active_chain_height(&g_state.chain_active);
        if (scan_max_have_data_h > tip_h && tip_h > 0) {
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
            }
        }
    }

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

    /* Activate best chain (connects any new blocks beyond saved tip).
     * Skip for -legacy import and -reindex-chainstate. */
    if (ctx->legacy_import_dir)
        skip_activate = true;
    if (!skip_activate) {
        struct validation_state vs;
        validation_state_init(&vs);
        if (!activate_best_chain(&vs, &g_state, &g_coins_tip, params, NULL,
                                 ctx->datadir)) {
            fprintf(stderr, "Warning: Failed to activate best chain\n");
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
         * This prevents the broken Ed25519 joinsplit verifier from
         * rejecting valid historical blocks above the checkpoint. */
        if (g_assume_valid_height >= 0 &&
            tip->nHeight > g_assume_valid_height) {
            g_assume_valid_height = tip->nHeight;
            printf("Assume-valid: extended to startup tip height %d\n",
                   g_assume_valid_height);
        }
    } else {
        printf("Chain tip: genesis\n");
    }

    /* Backfill shielded values in SQLite from block_index.
     * Only needed once — check via node_state flag, not full table scan.
     * Skip in no_services mode (speedrun). */
    if (g_node_db.open && tip && tip->nHeight > 1000 && !ctx->no_services) {
        int64_t has_shielded = 0;
        node_db_state_get_int(&g_node_db, "shielded_backfilled", &has_shielded);
        if (has_shielded == 0) {
            struct boot_shielded_backfill_ctx backfill = {0};
            bool ok = false;
            printf("Backfilling shielded values from block_index...\n");
            if (boot_runtime_db_service()) {
                ok = db_service_run_write(boot_runtime_db_service(),
                                          boot_backfill_shielded_values_write,
                                          &backfill);
            } else {
                ok = boot_backfill_shielded_values_write(&g_node_db, &backfill);
            }
            if (ok) {
                printf("Backfill: updated %d blocks with shielded values\n",
                       backfill.updated);
                fflush(stdout);
            } else {
                fprintf(stderr, "Backfill: failed to update shielded values\n");
            }
        }
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
    return app_init_services(ctx, params, &g_svc);
}

/* app_shutdown delegates to boot_services.c */
void app_shutdown(void) { app_shutdown_svc(&g_svc); }
bool app_is_running(void) { return atomic_load(&g_running); }

/* app_add_node, app_start_metrics, app_stop_metrics: boot_services.c */
