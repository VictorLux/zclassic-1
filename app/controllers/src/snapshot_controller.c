/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Snapshot-based import from legacy C++ ZClassic node.
 *
 * Creates timestamped snapshots of the C++ data dir using hard links
 * (instant for block files) and clean LevelDB copies. Imports the
 * snapshot into SQLite in parallel:
 *
 *   T1: block index LevelDB → blocks table (~30s for 3M blocks)
 *   T2: chainstate LevelDB  → utxos table  (~50s for 5M outputs)
 *   T3: wallet scan          → wallet_* tables (~10s)
 *
 * This replaces the old sequential catchup (~6 hours) with a
 * parallel LevelDB-to-SQLite bulk import (~1-2 minutes). */

#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "platform/time_compat.h"
#include "controllers/snapshot_controller.h"
#include "config/file_ops.h"
#include "controllers/legacy_import.h"
#include "controllers/sync_controller.h"
#include "storage/dbwrapper.h"
#include "storage/block_index_db.h"
#include "storage/coins_db.h"
#include "models/block.h"
#include "models/utxo.h"
#include "models/tx_index.h"
#include "wallet/wallet.h"
#include "chain/chain.h"
#include "core/serialize.h"
#include "core/hash.h"
#include "primitives/block.h"
#include "script/standard.h"
#include "crypto/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"

/* ZCL_MAGIC used in legacy_import.c, not needed here. */

static bool snapshot_sql_exec_checked(sqlite3 *db,
                                      const char *sql,
                                      const char *label)
{
    if (!db || !sql)
        LOG_FAIL("snapshot", "sql_exec_checked: db=%p sql=%p", (void *)db, (void *)sql);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "snapshot: %s failed: %s\n",
                label, sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool snapshot_tx_begin_checked(struct node_db *ndb,
                                      const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb)) {
        fprintf(stderr, "snapshot: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static bool snapshot_tx_commit_checked(struct node_db *ndb,
                                       const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb)) {
        fprintf(stderr, "snapshot: %s failed: %s\n",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
        return false;
    }
    return true;
}

static void snapshot_tx_rollback_best_effort(struct node_db *ndb,
                                             const char *label)
{
    if (!ndb || !ndb->open)
        return;
    if (!node_db_rollback(ndb)) {
        fprintf(stderr, "snapshot: %s failed: %s\n", // obs-ok:helper-return-path
                label, ndb->db ? sqlite3_errmsg(ndb->db) : "db unavailable");
    }
}

/* ---- Snapshot directory management ---- */

static void rotate_snapshots(const char *snapshots_dir, int max_keep)
{
    struct dirent **entries;
    int n = scandir(snapshots_dir, &entries, NULL, alphasort);
    if (n < 0) return;

    /* Count real snapshot dirs (YYYYMMDD_HHMMSS format) */
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (entries[i]->d_name[0] != '.' && strlen(entries[i]->d_name) == 15)
            count++;
    }

    /* Remove oldest if over limit */
    int to_remove = count - max_keep;
    if (to_remove > 0) {
        for (int i = 0; i < n && to_remove > 0; i++) {
            if (entries[i]->d_name[0] == '.' ||
                strlen(entries[i]->d_name) != 15)
                continue;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s",
                     snapshots_dir, entries[i]->d_name);
            printf("snapshot: removing old snapshot %s\n",
                   entries[i]->d_name);
            dir_remove_tree(path);
            to_remove--;
        }
    }

    for (int i = 0; i < n; i++) free(entries[i]);
    free(entries);
}

const char *snapshot_create(const char *legacy_datadir,
                            const char *c23_datadir,
                            int max_keep)
{
    if (max_keep < 1) max_keep = 2;

    /* Create snapshots directory */
    char snapshots_dir[2048];
    snprintf(snapshots_dir, sizeof(snapshots_dir),
             "%s/snapshots", c23_datadir);
    mkdir(snapshots_dir, 0700);

    /* Rotate old snapshots first */
    rotate_snapshots(snapshots_dir, max_keep);

    /* Create timestamped snapshot dir */
    time_t now = platform_time_wall_time_t();
    struct tm *tm = localtime(&now);
    static char snap_dir[2048];
    char ts[16];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(snap_dir, sizeof(snap_dir), "%s/%s", snapshots_dir, ts);
    mkdir(snap_dir, 0700);

    struct timespec t0;
    platform_time_monotonic_timespec(&t0);

    /* Hard-link block files (instant — same filesystem) */
    char src[2048], dst[2048];
    snprintf(dst, sizeof(dst), "%s/blocks", snap_dir);
    mkdir(dst, 0700);

    printf("snapshot: copying block files...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/blocks", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/blocks", snap_dir);
    int copied = block_files_copy(src, dst);
    if (copied < 0) {
        fprintf(stderr, // obs-ok:helper-return-path
                "snapshot: block file copy failed from %s to %s\n",
                src, dst);
        dir_remove_tree(snap_dir);
        return NULL;
    }
    if (copied == 0) {
        fprintf(stderr, // obs-ok:helper-return-path
                "snapshot: failed to copy block files from %s to %s\n",
                src, dst);
        dir_remove_tree(snap_dir);
        return NULL;
    }
    printf("snapshot: %d block files copied\n", copied);

    /* Copy block index LevelDB */
    printf("snapshot: copying blocks/index...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/blocks/index", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/blocks/index", snap_dir);
    if (!dir_copy(src, dst)) {
        fprintf(stderr, // obs-ok:helper-return-path
                "snapshot: failed to copy block index from %s to %s\n",
                src, dst);
        dir_remove_tree(snap_dir);
        return NULL;
    }
    printf(" done\n");

    /* Copy chainstate LevelDB */
    printf("snapshot: copying chainstate...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/chainstate", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/chainstate", snap_dir);
    if (!dir_copy(src, dst)) {
        fprintf(stderr, // obs-ok:helper-return-path
                "snapshot: failed to copy chainstate from %s to %s\n",
                src, dst);
        dir_remove_tree(snap_dir);
        return NULL;
    }
    printf(" done\n");

    struct timespec t1;
    platform_time_monotonic_timespec(&t1);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("snapshot: created %s in %.1fs\n", ts, elapsed);
    fflush(stdout);

    return snap_dir;
}

/* ---- Thread 1: Block index LevelDB → SQLite blocks table ---- */

struct block_index_import_args {
    const char *snapshot_dir;
    const char *db_path;
    int result;
    int count;
};

static void *import_block_index_thread(void *arg)
{
    struct block_index_import_args *a = arg;
    a->result = -1;
    a->count = 0;

    /* Open our own SQLite connection */
    struct node_db ndb;
    if (!node_db_open(&ndb, a->db_path)) {
        fprintf(stderr, "T1: failed to open SQLite\n");
        return NULL;
    }

    /* Open block index LevelDB from snapshot */
    char idx_path[1024];
    snprintf(idx_path, sizeof(idx_path), "%s/blocks/index", a->snapshot_dir);

    struct db_wrapper dbw;
    if (!db_wrapper_open(&dbw, idx_path, 256 << 20, false, false)) {
        fprintf(stderr, "T1: failed to open block index at %s\n", idx_path);
        node_db_close(&ndb);
        return NULL;
    }

    printf("T1: importing block index...\n");
    fflush(stdout);
    int64_t t_start = (int64_t)platform_time_wall_time_t();
    bool tx_open = false;
    bool ok = true;

    /* Turbo mode */
    if (!snapshot_sql_exec_checked(ndb.db, "PRAGMA synchronous=OFF",
                                   "T1 set synchronous=OFF") ||
        !snapshot_sql_exec_checked(ndb.db, "PRAGMA cache_size=-524288",
                                   "T1 set cache_size") ||
        !snapshot_sql_exec_checked(ndb.db, "PRAGMA wal_autocheckpoint=0",
                                   "T1 disable wal_autocheckpoint")) {
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }
    sqlite3_busy_timeout(ndb.db, 30000);

    /* Drop block indexes for bulk load */
    if (!snapshot_sql_exec_checked(ndb.db,
            "DROP INDEX IF EXISTS idx_blocks_prev",
            "T1 drop idx_blocks_prev") ||
        !snapshot_sql_exec_checked(ndb.db,
            "DROP INDEX IF EXISTS idx_blocks_chainwork",
            "T1 drop idx_blocks_chainwork") ||
        !snapshot_sql_exec_checked(ndb.db, "DELETE FROM blocks",
            "T1 clear blocks")) {
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }
    {
        static const uint8_t zero_hash[32] = {0};
        if (!node_db_sync_set_tip(&ndb, zero_hash, -1)) {
            fprintf(stderr, "T1: failed to reset tip state\n");
            db_wrapper_close(&dbw);
            node_db_close(&ndb);
            return NULL;
        }
    }

    /* Iterate all 'b'-prefixed entries */
    struct db_iterator it;
    db_iter_init(&it, &dbw);

    char seek_key[33];
    seek_key[0] = 'b';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    if (!snapshot_tx_begin_checked(&ndb, "T1 begin bulk load transaction")) {
        db_iter_free(&it);
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }
    tx_open = true;

    while (db_iter_valid(&it)) {
        size_t key_len;
        const char *key_data = db_iter_key(&it, &key_len);

        if (key_len < 1 || key_data[0] != 'b') break;
        if (key_len < 33) { db_iter_next(&it); continue; }

        /* Hash is in key bytes 1..32 */
        uint8_t block_hash[32];
        memcpy(block_hash, key_data + 1, 32);

        /* Deserialize disk_block_index from value */
        size_t val_len;
        const char *val_data = db_iter_value(&it, &val_len);

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        struct byte_stream s;
        stream_init_from_data(&s, (unsigned char *)val_data, val_len);

        if (!disk_block_index_deserialize(&dbi, &s)) {
            stream_free(&s);
            db_iter_next(&it);
            continue;
        }
        stream_free(&s);

        /* Insert into SQLite blocks table */
        struct db_block db_blk;
        memset(&db_blk, 0, sizeof(db_blk));
        memcpy(db_blk.hash, block_hash, 32);
        db_blk.height = dbi.nHeight;
        memcpy(db_blk.prev_hash, dbi.hashPrev.data, 32);
        db_blk.version = dbi.nVersion;
        memcpy(db_blk.merkle_root, dbi.hashMerkleRoot.data, 32);
        db_blk.time = dbi.nTime;
        db_blk.bits = dbi.nBits;
        memcpy(db_blk.nonce, dbi.nNonce.data, 32);
        db_blk.solution = dbi.nSolution;
        db_blk.solution_len = dbi.nSolutionSize;
        db_blk.status = (int)dbi.nStatus;
        db_blk.file_num = dbi.nFile;
        db_blk.data_pos = (int)dbi.nDataPos;
        db_blk.undo_pos = (int)dbi.nUndoPos;
        db_blk.num_tx = (int)dbi.nTx;

        if (!db_block_save(&ndb, &db_blk)) {
            fprintf(stderr, "T1: block save failed at height %d\n", // obs-ok:helper-return-path
                    db_blk.height);
            ok = false;
            break;
        }
        a->count++;

        if (a->count % 100000 == 0) {
            if (!snapshot_tx_commit_checked(&ndb, "T1 batch commit")) {
                ok = false;
                tx_open = false;
                break;
            }
            tx_open = false;
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            int rate = elapsed > 0 ? a->count / (int)elapsed : a->count;
            printf("T1: %d blocks (%d/s)\n", a->count, rate);
            fflush(stdout);
            if (!snapshot_tx_begin_checked(&ndb, "T1 batch reopen")) {
                ok = false;
                break;
            }
            tx_open = true;
        }

        db_iter_next(&it);
    }

    db_iter_free(&it);

    if (!ok) {
        if (tx_open)
            snapshot_tx_rollback_best_effort(&ndb, "T1 rollback after failure");
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }
    if (tx_open && !snapshot_tx_commit_checked(&ndb, "T1 final commit")) {
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }
    tx_open = false;

    /* Rebuild indexes */
    printf("T1: rebuilding block indexes...\n");
    fflush(stdout);
    if (!snapshot_sql_exec_checked(ndb.db,
            "CREATE INDEX IF NOT EXISTS idx_blocks_prev ON blocks(prev_hash)",
            "T1 rebuild idx_blocks_prev") ||
        !snapshot_sql_exec_checked(ndb.db,
            "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork"
            " ON blocks(chain_work DESC)",
            "T1 rebuild idx_blocks_chainwork") ||
        !snapshot_sql_exec_checked(ndb.db, "PRAGMA synchronous=NORMAL",
            "T1 restore synchronous=NORMAL") ||
        !snapshot_sql_exec_checked(ndb.db, "PRAGMA wal_autocheckpoint=1000",
            "T1 restore wal_autocheckpoint")) {
        db_wrapper_close(&dbw);
        node_db_close(&ndb);
        return NULL;
    }

    db_wrapper_close(&dbw);
    node_db_close(&ndb);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("T1: block index import complete: %d blocks in %llds\n",
           a->count, (long long)elapsed);
    fflush(stdout);

    a->result = 0;
    return NULL;
}

/* ---- Thread 2: Chainstate LevelDB → SQLite utxos table ---- */

struct utxo_import_args {
    const char *snapshot_dir;
    const char *db_path;
    int result;
    int count;
};

static void *import_utxos_thread(void *arg)
{
    struct utxo_import_args *a = arg;
    struct node_db_sync_import_job job;
    a->result = -1;
    a->count = 0;

    struct node_db ndb;
    if (!node_db_open(&ndb, a->db_path)) {
        fprintf(stderr, "T2: failed to open SQLite\n");
        return NULL;
    }

    char cs_path[1024];
    snprintf(cs_path, sizeof(cs_path), "%s/chainstate", a->snapshot_dir);

    struct coins_view_db cvdb;
    if (!coins_view_db_open(&cvdb, cs_path, 256 << 20, false, false)) {
        fprintf(stderr, "T2: failed to open chainstate at %s\n", cs_path);
        node_db_close(&ndb);
        return NULL;
    }

    printf("T2: importing UTXO set...\n");
    fflush(stdout);

    node_db_sync_import_job_init(&job);
    if (!node_db_sync_import_job_start(&job, &ndb, &cvdb)) {
        fprintf(stderr, "T2: failed to start UTXO import job\n");
        coins_view_db_close(&cvdb);
        node_db_close(&ndb);
        return NULL;
    }
    node_db_sync_import_job_join(&job, &a->count);

    coins_view_db_close(&cvdb);
    node_db_close(&ndb);

    printf("T2: UTXO import complete: %d outputs\n", a->count);
    fflush(stdout);

    a->result = 0;
    return NULL;
}

/* ---- Thread 3: Wallet scan of block files ---- */

struct wallet_import_args {
    const char *snapshot_dir;
    const char *db_path;
    struct wallet *wallet;
    int result;
    int count;
};

static void *import_wallet_thread(void *arg);

struct snapshot_import_job {
    pthread_t block_index_thread;
    bool block_index_started;
    pthread_t utxo_thread;
    bool utxo_started;
    pthread_t wallet_thread;
    bool wallet_started;
    struct block_index_import_args block_index_args;
    struct utxo_import_args utxo_args;
    struct wallet_import_args wallet_args;
};

static void snapshot_import_job_init(struct snapshot_import_job *job,
                                     const char *snapshot_dir,
                                     const char *db_path,
                                     struct wallet *wallet)
{
    if (!job)
        return;

    memset(job, 0, sizeof(*job));
    job->block_index_args.snapshot_dir = snapshot_dir;
    job->block_index_args.db_path = db_path;
    job->utxo_args.snapshot_dir = snapshot_dir;
    job->utxo_args.db_path = db_path;
    job->wallet_args.snapshot_dir = snapshot_dir;
    job->wallet_args.db_path = db_path;
    job->wallet_args.wallet = wallet;
}

static void snapshot_import_job_join(struct snapshot_import_job *job)
{
    if (!job)
        return;
    if (job->block_index_started) {
        pthread_join(job->block_index_thread, NULL);
        job->block_index_started = false;
    }
    if (job->utxo_started) {
        pthread_join(job->utxo_thread, NULL);
        job->utxo_started = false;
    }
    if (job->wallet_started) {
        pthread_join(job->wallet_thread, NULL);
        job->wallet_started = false;
    }
}

static bool snapshot_import_job_succeeded(const struct snapshot_import_job *job)
{
    if (!job)
        return false;
    return job->block_index_args.result == 0 &&
           job->utxo_args.result == 0 &&
           job->wallet_args.result == 0;
}

static bool snapshot_import_job_start(struct snapshot_import_job *job)
{
    if (!job)
        return false;

    if (thread_registry_spawn_ex("zcl_snap_idx",
                                  import_block_index_thread,
                                  &job->block_index_args,
                                  &job->block_index_thread) != 0) {
        fprintf(stderr,
                "snapshot_import: failed to start block-index import thread\n");
        return false;
    }
    job->block_index_started = true;

    if (thread_registry_spawn_ex("zcl_snap_utxo",
                                  import_utxos_thread,
                                  &job->utxo_args,
                                  &job->utxo_thread) != 0) {
        fprintf(stderr,
                "snapshot_import: failed to start UTXO import thread\n");
        snapshot_import_job_join(job);
        return false;
    }
    job->utxo_started = true;

    if (thread_registry_spawn_ex("zcl_snap_wallet",
                                  import_wallet_thread,
                                  &job->wallet_args,
                                  &job->wallet_thread) != 0) {
        fprintf(stderr,
                "snapshot_import: failed to start wallet import thread\n");
        snapshot_import_job_join(job);
        return false;
    }
    job->wallet_started = true;

    return true;
}

static void *import_wallet_thread(void *arg)
{
    struct wallet_import_args *a = arg;
    a->result = -1;
    a->count = 0;

    struct node_db ndb;
    if (!node_db_open(&ndb, a->db_path)) {
        fprintf(stderr, "T3: failed to open SQLite\n");
        return NULL;
    }

    int wallet_keys = 0;
    for (size_t i = 0; i < a->wallet->keystore.num_keys; i++)
        if (a->wallet->keystore.keys[i].used) wallet_keys++;

    if (wallet_keys == 0 && a->wallet->sapling_keys.num_keys == 0) {
        printf("T3: no wallet keys, skipping scan\n");
        fflush(stdout);
        node_db_sync_wallet_keys(&ndb, a->wallet);
        node_db_close(&ndb);
        a->result = 0;
        return NULL;
    }

    printf("T3: wallet scan (%d t-keys, %zu z-keys)...\n",
           wallet_keys, a->wallet->sapling_keys.num_keys);
    fflush(stdout);

    a->count = legacy_import(a->snapshot_dir, &ndb, a->wallet,
                             a->wallet->sapling_keys.num_keys > 0);

    node_db_close(&ndb);

    printf("T3: wallet scan complete: %d transactions\n", a->count);
    fflush(stdout);

    a->result = 0;
    return NULL;
}

/* ---- Main import orchestrator ---- */

int snapshot_import(const char *snapshot_dir,
                    const char *c23_datadir,
                    struct node_db *ndb,
                    struct wallet *w)
{
    (void)ndb; /* Each thread opens its own SQLite connection */
    struct timespec t0;
    struct snapshot_import_job job;
    platform_time_monotonic_timespec(&t0);

    /* SQLite database path */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", c23_datadir);

    printf("snapshot_import: parallel import from %s\n", snapshot_dir);
    printf("  T1: block index LevelDB → SQLite blocks\n");
    printf("  T2: chainstate LevelDB  → SQLite utxos\n");
    printf("  T3: wallet scan         → SQLite wallet_*\n");
    fflush(stdout);

    snapshot_import_job_init(&job, snapshot_dir, db_path, w);
    if (!snapshot_import_job_start(&job))
        LOG_ERR("snapshot", "failed to start parallel import from %s", snapshot_dir);
    snapshot_import_job_join(&job);

    struct timespec t1_end;
    platform_time_monotonic_timespec(&t1_end);
    double import_time = (double)(t1_end.tv_sec - t0.tv_sec) +
                         (double)(t1_end.tv_nsec - t0.tv_nsec) / 1e9;

    printf("snapshot_import: parallel import complete in %.1fs\n",
           import_time);
    printf("  blocks: %d, utxos: %d, wallet txs: %d\n",
           job.block_index_args.count, job.utxo_args.count,
           job.wallet_args.count);
    fflush(stdout);

    if (!snapshot_import_job_succeeded(&job)) {
        fprintf(stderr, // obs-ok:helper-return-path
                "snapshot_import: import workers failed "
                "(blocks=%d utxos=%d wallet=%d); refusing to sync files\n",
                job.block_index_args.result,
                job.utxo_args.result,
                job.wallet_args.result);
        return -1; // raw-return-ok:logged-above
    }

    /* Copy block files + LevelDB to C23 data dir for consensus engine */
    printf("snapshot_import: syncing files to %s...\n", c23_datadir);
    fflush(stdout);

    char src[2048], dst[2048];

    /* Block files: remove stale then byte-copy through checked helper. */
    snprintf(dst, sizeof(dst), "%s/blocks", c23_datadir);
    mkdir(dst, 0700);
    snprintf(src, sizeof(src), "%s/blocks", snapshot_dir);
    block_files_clean(dst);
    int copied = block_files_copy(src, dst);
    if (copied < 0) {
        fprintf(stderr,
                "snapshot_import: block file copy failed from %s to %s\n",
                src, dst);
        return -1; // raw-return-ok:logged-above
    }
    if (copied == 0) {
        fprintf(stderr,
                "snapshot_import: failed to sync block files from %s to %s\n",
                src, dst);
        return -1; // raw-return-ok:logged-above
    }

    /* Block index: clean copy */
    snprintf(dst, sizeof(dst), "%s/blocks/index", c23_datadir);
    dir_remove_tree(dst);
    mkdir(dst, 0700);
    snprintf(src, sizeof(src), "%s/blocks/index", snapshot_dir);
    if (!dir_copy(src, dst)) {
        fprintf(stderr,
                "snapshot_import: failed to sync block index from %s to %s\n",
                src, dst);
        return -1; // raw-return-ok:logged-above
    }

    /* Chainstate: clean copy */
    snprintf(dst, sizeof(dst), "%s/chainstate", c23_datadir);
    dir_remove_tree(dst);
    mkdir(dst, 0700);
    snprintf(src, sizeof(src), "%s/chainstate", snapshot_dir);
    if (!dir_copy(src, dst)) {
        fprintf(stderr,
                "snapshot_import: failed to sync chainstate from %s to %s\n",
                src, dst);
        return -1; // raw-return-ok:logged-above
    }

    struct timespec t2_end;
    platform_time_monotonic_timespec(&t2_end);
    double total_time = (double)(t2_end.tv_sec - t0.tv_sec) +
                        (double)(t2_end.tv_nsec - t0.tv_nsec) / 1e9;

    printf("snapshot_import: COMPLETE in %.1fs (import %.1fs, copy %.1fs)\n",
           total_time, import_time, total_time - import_time);
    fflush(stdout);

    return 0;
}

/* ---- Background transaction index builder ---- */

static bool snapshot_deserialize_index_block(const uint8_t *data,
                                             size_t avail,
                                             int height,
                                             struct block *blk,
                                             struct uint256 *hash_out)
{
    if (!data || avail == 0 || !blk || !hash_out)
        return false;

    block_init(blk);
    struct byte_stream bs;
    stream_init_from_data(&bs, data, avail);
    bool ok = block_deserialize(blk, &bs);
    stream_free(&bs);
    if (!ok) {
        fprintf(stderr, // obs-ok:helper-return-path
                "tx_index: skipping block after full deserialize failure "
                "height=%d\n",
                height);
        block_free(blk);
        return false;
    }

    block_get_hash(blk, hash_out);
    return true;
}

static bool snapshot_block_file_magic_ok(const uint8_t *p)
{
    if (!p)
        return false;
    return (p[0] == 0x24 && p[1] == 0xe9 &&
            p[2] == 0x27 && p[3] == 0x64) ||
           (p[0] == 0xfa && p[1] == 0x1a &&
            p[2] == 0xf9 && p[3] == 0xbf) ||
           (p[0] == 0xaa && p[1] == 0xe8 &&
            p[2] == 0x3f && p[3] == 0x5f);
}

static bool snapshot_block_file_size_ok(uint32_t block_size,
                                        size_t file_size,
                                        size_t envelope_pos)
{
    return block_size > 0 &&
           block_size <= MAX_BLOCK_SIZE &&
           envelope_pos + 8 <= file_size &&
           (size_t)block_size <= file_size - envelope_pos - 8;
}

static bool snapshot_locate_block_payload(const uint8_t *file_data,
                                          size_t file_size,
                                          size_t stored_pos,
                                          int height,
                                          const uint8_t **payload_out,
                                          size_t *payload_len_out)
{
    if (!file_data || !payload_out || !payload_len_out ||
        stored_pos >= file_size)
        return false;

    *payload_out = NULL;
    *payload_len_out = 0;

    uint32_t block_size = 0;
    if (stored_pos + 8 <= file_size &&
        snapshot_block_file_magic_ok(file_data + stored_pos)) {
        memcpy(&block_size, file_data + stored_pos + 4, 4);
        if (snapshot_block_file_size_ok(block_size, file_size, stored_pos)) {
            *payload_out = file_data + stored_pos + 8;
            *payload_len_out = block_size;
            return true;
        }
    }

    if (stored_pos >= 8 &&
        snapshot_block_file_magic_ok(file_data + stored_pos - 8)) {
        memcpy(&block_size, file_data + stored_pos - 4, 4);
        if (snapshot_block_file_size_ok(block_size, file_size,
                                        stored_pos - 8)) {
            *payload_out = file_data + stored_pos;
            *payload_len_out = block_size;
            return true;
        }
    }

    if (height >= 0)
        fprintf(stderr, // obs-ok:helper-return-path
                "tx_index: cannot locate block payload height=%d pos=%zu "
                "file_size=%zu\n",
                height, stored_pos, file_size);
    return false;
}

static int snapshot_extract_bip34_height_from_block(const struct block *blk)
{
    if (!blk || blk->num_vtx == 0 || blk->vtx[0].num_vin == 0)
        return -1; // raw-return-ok:bin-parser-empty-vin

    const struct script *sig = &blk->vtx[0].vin[0].script_sig;
    if (sig->size == 0)
        return -1; // raw-return-ok:bin-parser-bounds

    uint8_t nbytes = sig->data[0];
    if (nbytes == 0x00)
        return 0;
    if (nbytes >= 0x51 && nbytes <= 0x60)
        return nbytes - 0x50;
    if (nbytes > 8 || (size_t)nbytes + 1 > sig->size)
        return -1; // raw-return-ok:bin-parser-bounds

    int64_t h = 0;
    for (uint8_t i = 0; i < nbytes; i++)
        h |= (int64_t)sig->data[1 + i] << (8 * i);
    if (sig->data[nbytes] & 0x80)
        h = -(h & ~((int64_t)0x80 << (8 * (nbytes - 1))));
    if (h < 0 || h > INT32_MAX)
        return -1; // raw-return-ok:bin-parser-bounds
    return (int)h;
}

static bool snapshot_tx_index_maybe_commit(struct node_db *ndb,
                                           bool *tx_open,
                                           int indexed,
                                           int64_t t_start)
{
    if (!ndb || !tx_open)
        return false;
    if (indexed <= 0 || (indexed % 5000) != 0)
        return true;

    if (!snapshot_tx_commit_checked(ndb, "tx_index batch commit")) {
        *tx_open = false;
        return false;
    }
    *tx_open = false;
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    int rate = elapsed > 0 ? indexed / (int)elapsed : indexed;
    printf("tx_index: %d transactions (%d/s)\n", indexed, rate);
    fflush(stdout);
    if (!snapshot_tx_begin_checked(ndb, "tx_index batch reopen"))
        return false;
    *tx_open = true;
    return true;
}

static bool snapshot_tx_index_save_block_txs(struct node_db *ndb,
                                             struct block *blk,
                                             const struct uint256 *block_hash,
                                             int height,
                                             int file_num,
                                             int file_pos,
                                             int *indexed,
                                             int64_t t_start,
                                             bool *tx_open)
{
    if (!ndb || !blk || !block_hash || !indexed || !tx_open)
        return false;
    if (height < 0)
        return false;

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        transaction_compute_hash(&blk->vtx[ti]);

        struct db_tx_index dt;
        memset(&dt, 0, sizeof(dt));
        memcpy(dt.txid, blk->vtx[ti].hash.data, 32);
        memcpy(dt.block_hash, block_hash->data, 32);
        dt.block_height = height;
        dt.tx_index = (int)ti;
        dt.file_num = file_num;
        dt.file_pos = file_pos;
        dt.is_coinbase = (ti == 0);
        if (!db_tx_save(ndb, &dt)) {
            fprintf(stderr,
                    "tx_index: failed to save tx index at height %d tx %zu\n",
                    height, ti);
            return false;
        }
        (*indexed)++;
        if (!snapshot_tx_index_maybe_commit(ndb, tx_open, *indexed,
                                            t_start))
            return false;
    }
    return true;
}

static bool snapshot_tx_index_build_from_block_files(struct node_db *ndb,
                                                     const char *datadir,
                                                     int *indexed,
                                                     int *skipped,
                                                     int64_t t_start,
                                                     bool *tx_open)
{
    if (!ndb || !datadir || !indexed || !skipped || !tx_open)
        return false;

    int files_seen = 0;
    for (int file_num = 0; file_num < 100000; file_num++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, file_num);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return files_seen > 0;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return false;
        }
        size_t file_size = (size_t)st.st_size;
        uint8_t *file_data = mmap(NULL, file_size, PROT_READ,
                                  MAP_PRIVATE, fd, 0);
        close(fd);
        if (file_data == MAP_FAILED)
            return false;
        posix_madvise(file_data, file_size, POSIX_MADV_SEQUENTIAL);
        files_seen++;

        size_t pos = 0;
        while (pos + 8 <= file_size) {
            if (!snapshot_block_file_magic_ok(file_data + pos)) {
                pos++;
                continue;
            }
            uint32_t block_size = 0;
            memcpy(&block_size, file_data + pos + 4, 4);
            if (!snapshot_block_file_size_ok(block_size, file_size, pos)) {
                pos++;
                continue;
            }

            struct block blk;
            struct uint256 block_hash;
            if (!snapshot_deserialize_index_block(file_data + pos + 8,
                                                  block_size, -1,
                                                  &blk, &block_hash)) {
                (*skipped)++;
                pos += 8 + block_size;
                continue;
            }
            int height = snapshot_extract_bip34_height_from_block(&blk);
            if (height < 0) {
                (*skipped)++;
                block_free(&blk);
                pos += 8 + block_size;
                continue;
            }
            bool saved = snapshot_tx_index_save_block_txs(
                ndb, &blk, &block_hash, height, file_num, (int)(pos + 8),
                indexed, t_start, tx_open);
            block_free(&blk);
            if (!saved) {
                munmap(file_data, file_size);
                return false;
            }
            pos += 8 + block_size;
        }
        munmap(file_data, file_size);
    }

    return files_seen > 0;
}

static void *build_tx_index_thread(void *arg)
{
    struct snapshot_tx_index_job *job = arg;
    const char *datadir;
    const char *db_path;
    bool ok = false;
    bool tx_open = false;
    int rc;
    sqlite3 *read_db = NULL;

    if (!job) {
        LOG_NULL("snapshot", "build_tx_index_thread called with NULL job");
    }

    job->result = -1;
    datadir = job->args.datadir;
    db_path = job->args.db_path;

    struct node_db ndb;
    if (!node_db_open(&ndb, db_path)) {
        fprintf(stderr, "tx_index: failed to open SQLite\n");
        return NULL;
    }

    /* Check how many transactions already indexed. A nonzero count is not
     * proof that the index is complete; older boots skipped after 100k rows,
     * which left historical spends unrecoverable during replay. Only skip
     * when a previous additive build explicitly marked completion. */
    int existing = db_tx_count(&ndb);
    int64_t complete = 0;
    node_db_state_get_int(&ndb, "tx_index_complete", &complete);
    if (complete >= 3 && existing > 100000) {
        printf("tx_index: complete marker v%lld present (%d transactions), skipping\n",
               (long long)complete, existing);
        node_db_close(&ndb);
        job->result = 0;
        return NULL;
    }

    printf("tx_index: additive build from block files (existing=%d, marker=%lld)...\n",
           existing, (long long)complete);
    fflush(stdout);
    int64_t t_start = (int64_t)platform_time_wall_time_t();

    sqlite3_busy_timeout(ndb.db, 30000);
    sqlite3_exec(ndb.db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "PRAGMA wal_autocheckpoint=0", NULL, NULL, NULL);

    if (sqlite3_open_v2(db_path, &read_db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        NULL) != SQLITE_OK || !read_db) {
        fprintf(stderr, "tx_index: failed to open read-only SQLite: %s\n", // obs-ok:helper-return-path
                read_db ? sqlite3_errmsg(read_db) : "db unavailable");
        if (read_db)
            sqlite3_close(read_db);
        node_db_close(&ndb);
        return NULL;
    }
    sqlite3_busy_timeout(read_db, 30000);

    /* Query blocks ordered by file_num, data_pos for sequential I/O */
    sqlite3_stmt *query = NULL;
    int query_rc = sqlite3_prepare_v2(read_db,
        "SELECT hash, height, file_num, data_pos, num_tx"
        " FROM blocks WHERE file_num >= 0"
        " ORDER BY file_num, data_pos",
        -1, &query, NULL);
    if (query_rc != SQLITE_OK || !query) {
        fprintf(stderr, "tx_index: failed to prepare block query: %s\n", // obs-ok:helper-return-path
                sqlite3_errmsg(read_db));
        if (query)
            sqlite3_finalize(query);
        sqlite3_close(read_db);
        node_db_close(&ndb);
        return NULL;
    }

    int indexed = 0;
    int skipped = 0;
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    if (!snapshot_tx_begin_checked(&ndb,
            "tx_index begin bulk load transaction")) {
        sqlite3_finalize(query);
        node_db_close(&ndb);
        return NULL;
    }
    tx_open = true;
    ok = true;

    for (rc = AR_STEP_ROW_READONLY(query);
         rc == SQLITE_ROW;
         rc = AR_STEP_ROW_READONLY(query)) {
        const uint8_t *block_hash = sqlite3_column_blob(query, 0);
        int height = sqlite3_column_int(query, 1);
        int file_num = sqlite3_column_int(query, 2);
        int data_pos = sqlite3_column_int(query, 3);
        int num_tx = sqlite3_column_int(query, 4);

        if (!block_hash || file_num < 0 || data_pos < 0) continue;

        /* mmap block file */
        if (file_num != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            char path[512];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     datadir, file_num);
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "tx_index: failed to open %s\n", path); // obs-ok:helper-return-path
                ok = false;
                break;
            }
            struct stat st;
            if (fstat(fd, &st) != 0) {
                fprintf(stderr, "tx_index: failed to stat %s\n", path); // obs-ok:helper-return-path
                close(fd);
                ok = false;
                break;
            }
            cached_size = (size_t)st.st_size;
            cached_data = mmap(NULL, cached_size, PROT_READ,
                               MAP_PRIVATE, fd, 0);
            close(fd);
            if (cached_data == MAP_FAILED) {
                fprintf(stderr, "tx_index: failed to mmap %s\n", path); // obs-ok:helper-return-path
                cached_data = NULL;
                ok = false;
                break;
            }
            posix_madvise(cached_data, cached_size, POSIX_MADV_SEQUENTIAL);
            cached_file = file_num;
        }

        if (!cached_data || (size_t)data_pos >= cached_size) {
            if (skipped < 20 || (skipped % 10000) == 0)
                fprintf(stderr, // obs-ok:helper-return-path
                        "tx_index: skipping invalid block offset file=%d "
                        "height=%d pos=%d size=%zu\n",
                        file_num, height, data_pos, cached_size);
            skipped++;
            continue;
        }

        const uint8_t *bdata = NULL;
        size_t bavail = 0;
        if (!snapshot_locate_block_payload(cached_data, cached_size,
                                           (size_t)data_pos, height,
                                           &bdata, &bavail)) {
            if (skipped < 20 || (skipped % 10000) == 0)
                fprintf(stderr, // obs-ok:helper-return-path
                        "tx_index: skipping unlocatable block file=%d "
                        "height=%d pos=%d size=%zu\n",
                        file_num, height, data_pos, cached_size);
            skipped++;
            continue;
        }

        struct block blk;
        struct uint256 disk_hash;
        if (!snapshot_deserialize_index_block(bdata, bavail, height,
                                              &blk, &disk_hash)) {
            skipped++;
            continue;
        }
        struct uint256 expected_hash;
        memcpy(expected_hash.data, block_hash, 32);
        if (uint256_cmp(&disk_hash, &expected_hash) != 0) {
            char got[65], want[65];
            uint256_get_hex(&disk_hash, got);
            uint256_get_hex(&expected_hash, want);
            if (skipped < 20 || (skipped % 10000) == 0)
                fprintf(stderr, // obs-ok:helper-return-path
                        "tx_index: skipping block hash mismatch height=%d "
                        "got=%s want=%s\n",
                        height, got, want);
            block_free(&blk);
            skipped++;
            continue;
        }

        size_t tx_limit = blk.num_vtx;
        if (num_tx > 0 && (size_t)num_tx < tx_limit)
            tx_limit = (size_t)num_tx;

        size_t saved_num_vtx = blk.num_vtx;
        blk.num_vtx = tx_limit;
        ok = snapshot_tx_index_save_block_txs(
            &ndb, &blk, &expected_hash, height, file_num, data_pos,
            &indexed, t_start, &tx_open);
        blk.num_vtx = saved_num_vtx;
        block_free(&blk);
        if (!ok)
            break;

    }

    if (rc != SQLITE_DONE && ok) {
        fprintf(stderr, "tx_index: block query failed: %s\n", // obs-ok:helper-return-path
                sqlite3_errmsg(read_db));
        ok = false;
    }

    if (tx_open && !ok)
        snapshot_tx_rollback_best_effort(&ndb, "tx_index rollback after failure");
    else if (tx_open && !snapshot_tx_commit_checked(&ndb, "tx_index final commit")) {
        ok = false;
        snapshot_tx_rollback_best_effort(&ndb, "tx_index rollback after final commit failure");
    }

    if (cached_data) munmap(cached_data, cached_size);
    sqlite3_finalize(query);
    sqlite3_close(read_db);
    query = NULL;
    read_db = NULL;
    cached_data = NULL;
    tx_open = false;

    if (ok && skipped > 0) {
        fprintf(stderr, // obs-ok:helper-return-path
                "tx_index: SQLite block rows skipped %d blocks; falling "
                "back to raw blk*.dat walk\n",
                skipped);
        skipped = 0;
        if (!snapshot_tx_begin_checked(&ndb,
                "tx_index raw fallback begin")) {
            ok = false;
        } else {
            tx_open = true;
        }
        if (ok)
            ok = snapshot_tx_index_build_from_block_files(
                &ndb, datadir, &indexed, &skipped, t_start, &tx_open);
    }

    if (ok && skipped > 0) {
        fprintf(stderr, // obs-ok:helper-return-path
                "tx_index: incomplete — raw block-file walk skipped %d "
                "blocks; leaving marker unset so a safer builder can retry\n",
                skipped);
        ok = false;
    }

    if (tx_open && !ok) {
        snapshot_tx_rollback_best_effort(&ndb,
            "tx_index raw fallback rollback after failure");
        tx_open = false;
    } else if (tx_open && !snapshot_tx_commit_checked(&ndb,
                   "tx_index raw fallback final commit")) {
        ok = false;
        snapshot_tx_rollback_best_effort(&ndb,
            "tx_index raw fallback rollback after final commit failure");
        tx_open = false;
    } else {
        tx_open = false;
    }

    if (ok && !db_tx_finalize_bulk_load(&ndb)) {
        fprintf(stderr, "tx_index: failed to finalize bulk load indexes\n"); // obs-ok:helper-return-path
        ok = false;
    }
    if (ok)
        node_db_state_set_int(&ndb, "tx_index_complete", 3);

    node_db_close(&ndb);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    if (ok) {
        printf("tx_index: complete — %d transactions indexed, %d blocks "
               "skipped in %llds\n",
               indexed, skipped, (long long)elapsed);
        fflush(stdout);
        job->result = 0;
    } else {
        printf("tx_index: failed after %d transactions in %llds\n",
               indexed, (long long)elapsed);
        fflush(stdout);
    }
    return NULL;
}

void snapshot_tx_index_job_init(struct snapshot_tx_index_job *job)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->result = -1;
}

bool snapshot_tx_index_job_start(struct snapshot_tx_index_job *job,
                                 const char *c23_datadir)
{
    if (!job || job->started || !c23_datadir)
        LOG_FAIL("snapshot", "tx_index_job_start: invalid args job=%p started=%d datadir=%p",
                 (void *)job, job ? job->started : 0, (void *)c23_datadir);

    if (snprintf(job->args.db_path, sizeof(job->args.db_path),
                 "%s/node.db", c23_datadir) >=
        (int)sizeof(job->args.db_path)) {
        LOG_FAIL("snapshot", "tx_index_job_start: db_path truncated for datadir %s", c23_datadir);
    }
    job->args.datadir = c23_datadir;
    job->result = -1;
    if (thread_registry_spawn_ex("zcl_snap_txidx",
                                  build_tx_index_thread, job,
                                  &job->thread) != 0) {
        LOG_FAIL("snapshot", "tx_index_job_start: thread_registry_spawn_ex failed for datadir %s", c23_datadir);
    }
    job->started = true;
    return true;
}

bool snapshot_tx_index_job_join(struct snapshot_tx_index_job *job,
                                int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        LOG_FAIL("snapshot", "tx_index_job_join: invalid state job=%p started=%d",
                 (void *)job, job ? job->started : 0);

    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        LOG_FAIL("snapshot", "tx_index_job_join: pthread_join failed rc=%d", join_rc);

    job->started = false;
    if (result_out)
        *result_out = job->result;
    return true;
}

bool snapshot_tx_index_job_is_started(const struct snapshot_tx_index_job *job)
{
    return job && job->started;
}
