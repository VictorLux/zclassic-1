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

/* ZCL_MAGIC used in legacy_import.c, not needed here. */

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
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    static char snap_dir[2048];
    char ts[16];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(snap_dir, sizeof(snap_dir), "%s/%s", snapshots_dir, ts);
    mkdir(snap_dir, 0700);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Hard-link block files (instant — same filesystem) */
    char src[2048], dst[2048];
    snprintf(dst, sizeof(dst), "%s/blocks", snap_dir);
    mkdir(dst, 0700);

    printf("snapshot: copying block files...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/blocks", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/blocks", snap_dir);
    int copied = block_files_copy(src, dst);
    printf("snapshot: %d block files copied\n", copied);

    /* Copy block index LevelDB */
    printf("snapshot: copying blocks/index...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/blocks/index", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/blocks/index", snap_dir);
    dir_copy(src, dst);
    printf(" done\n");

    /* Copy chainstate LevelDB */
    printf("snapshot: copying chainstate...\n");
    fflush(stdout);
    snprintf(src, sizeof(src), "%s/chainstate", legacy_datadir);
    snprintf(dst, sizeof(dst), "%s/chainstate", snap_dir);
    dir_copy(src, dst);
    printf(" done\n");

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
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
    int64_t t_start = (int64_t)time(NULL);

    /* Turbo mode */
    sqlite3_exec(ndb.db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "PRAGMA cache_size=-524288", NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "PRAGMA wal_autocheckpoint=0", NULL, NULL, NULL);
    sqlite3_busy_timeout(ndb.db, 30000);

    /* Drop block indexes for bulk load */
    sqlite3_exec(ndb.db, "DROP INDEX IF EXISTS idx_blocks_prev",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "DROP INDEX IF EXISTS idx_blocks_chainwork",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "DELETE FROM blocks", NULL, NULL, NULL);
    {
        static const uint8_t zero_hash[32] = {0};
        node_db_sync_set_tip(&ndb, zero_hash, -1);
    }

    /* Iterate all 'b'-prefixed entries */
    struct db_iterator it;
    db_iter_init(&it, &dbw);

    char seek_key[33];
    seek_key[0] = 'b';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    node_db_begin(&ndb);

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

        db_block_save(&ndb, &db_blk);
        a->count++;

        if (a->count % 100000 == 0) {
            node_db_commit(&ndb);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            int rate = elapsed > 0 ? a->count / (int)elapsed : a->count;
            printf("T1: %d blocks (%d/s)\n", a->count, rate);
            fflush(stdout);
            node_db_begin(&ndb);
        }

        db_iter_next(&it);
    }

    db_iter_free(&it);

    node_db_commit(&ndb);

    /* Rebuild indexes */
    printf("T1: rebuilding block indexes...\n");
    fflush(stdout);
    sqlite3_exec(ndb.db,
        "CREATE INDEX IF NOT EXISTS idx_blocks_prev ON blocks(prev_hash)",
        NULL, NULL, NULL);
    sqlite3_exec(ndb.db,
        "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork"
        " ON blocks(chain_work DESC)",
        NULL, NULL, NULL);

    sqlite3_exec(ndb.db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb.db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);

    db_wrapper_close(&dbw);
    node_db_close(&ndb);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
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

    if (pthread_create(&job->block_index_thread, NULL,
                       import_block_index_thread,
                       &job->block_index_args) != 0) {
        fprintf(stderr,
                "snapshot_import: failed to start block-index import thread\n");
        return false;
    }
    job->block_index_started = true;

    if (pthread_create(&job->utxo_thread, NULL,
                       import_utxos_thread,
                       &job->utxo_args) != 0) {
        fprintf(stderr,
                "snapshot_import: failed to start UTXO import thread\n");
        snapshot_import_job_join(job);
        return false;
    }
    job->utxo_started = true;

    if (pthread_create(&job->wallet_thread, NULL,
                       import_wallet_thread,
                       &job->wallet_args) != 0) {
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
    clock_gettime(CLOCK_MONOTONIC, &t0);

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
        return -1;
    snapshot_import_job_join(&job);

    struct timespec t1_end;
    clock_gettime(CLOCK_MONOTONIC, &t1_end);
    double import_time = (double)(t1_end.tv_sec - t0.tv_sec) +
                         (double)(t1_end.tv_nsec - t0.tv_nsec) / 1e9;

    printf("snapshot_import: parallel import complete in %.1fs\n",
           import_time);
    printf("  blocks: %d, utxos: %d, wallet txs: %d\n",
           job.block_index_args.count, job.utxo_args.count,
           job.wallet_args.count);
    fflush(stdout);

    if (!snapshot_import_job_succeeded(&job)) {
        fprintf(stderr,
                "snapshot_import: import workers failed "
                "(blocks=%d utxos=%d wallet=%d); refusing to sync files\n",
                job.block_index_args.result,
                job.utxo_args.result,
                job.wallet_args.result);
        return -1;
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
    if (block_files_copy(src, dst) <= 0) {
        fprintf(stderr,
                "snapshot_import: failed to sync block files from %s to %s\n",
                src, dst);
        return -1;
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
        return -1;
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
        return -1;
    }

    struct timespec t2_end;
    clock_gettime(CLOCK_MONOTONIC, &t2_end);
    double total_time = (double)(t2_end.tv_sec - t0.tv_sec) +
                        (double)(t2_end.tv_nsec - t0.tv_nsec) / 1e9;

    printf("snapshot_import: COMPLETE in %.1fs (import %.1fs, copy %.1fs)\n",
           total_time, import_time, total_time - import_time);
    fflush(stdout);

    return 0;
}

/* ---- Background transaction index builder ---- */

/* Read CompactSize varint from raw bytes. */
static int read_cs(const uint8_t *d, size_t avail, uint64_t *out)
{
    if (avail < 1) return 0;
    if (d[0] < 0xfd) { *out = d[0]; return 1; }
    if (d[0] == 0xfd && avail >= 3) {
        *out = (uint64_t)d[1] | ((uint64_t)d[2] << 8); return 3;
    }
    if (d[0] == 0xfe && avail >= 5) {
        uint32_t v; memcpy(&v, d + 1, 4); *out = v; return 5;
    }
    if (d[0] == 0xff && avail >= 9) { memcpy(out, d + 1, 8); return 9; }
    return 0;
}

/* Skip a single serialized transaction, returning number of bytes consumed.
 * Also computes the txid (double-SHA256 of the serialized data). */
static size_t skip_tx_and_hash(const uint8_t *data, size_t avail,
                               uint8_t txid_out[32])
{
    const uint8_t *start = data;
    size_t pos = 0;
    if (pos + 4 > avail) return 0;

    int32_t ver;
    memcpy(&ver, data + pos, 4);
    pos += 4;
    bool overwintered = (ver & (int32_t)0x80000000) != 0;
    int32_t version = ver & 0x7FFFFFFF;
    uint32_t vg_id = 0;
    if (overwintered) {
        if (pos + 4 > avail) return 0;
        memcpy(&vg_id, data + pos, 4);
        pos += 4;
    }

    /* vin */
    uint64_t vin_count;
    int n = read_cs(data + pos, avail - pos, &vin_count);
    if (n == 0) return 0;
    pos += (size_t)n;
    for (uint64_t i = 0; i < vin_count; i++) {
        pos += 36;
        if (pos >= avail) return 0;
        uint64_t script_len;
        n = read_cs(data + pos, avail - pos, &script_len);
        if (n == 0) return 0;
        pos += (size_t)n + (size_t)script_len + 4;
    }

    /* vout */
    if (pos >= avail) return 0;
    uint64_t vout_count;
    n = read_cs(data + pos, avail - pos, &vout_count);
    if (n == 0) return 0;
    pos += (size_t)n;
    for (uint64_t i = 0; i < vout_count; i++) {
        pos += 8;
        if (pos >= avail) return 0;
        uint64_t script_len;
        n = read_cs(data + pos, avail - pos, &script_len);
        if (n == 0) return 0;
        pos += (size_t)n + (size_t)script_len;
    }

    /* nLockTime */
    if (pos + 4 > avail) return 0;
    pos += 4;

    if (overwintered) {
        if (pos + 4 > avail) return 0;
        pos += 4; /* nExpiryHeight */

        /* Sapling v4 */
        if (vg_id == 0x892F2085) {
            if (pos + 8 > avail) return 0;
            pos += 8; /* valueBalance */

            uint64_t nss;
            n = read_cs(data + pos, avail - pos, &nss);
            if (n == 0) return 0;
            pos += (size_t)n + (size_t)nss * 384;

            if (pos >= avail) return 0;
            uint64_t nso;
            n = read_cs(data + pos, avail - pos, &nso);
            if (n == 0) return 0;
            pos += (size_t)n + (size_t)nso * 948;

            if (nss > 0 || nso > 0) {
                if (pos + 64 > avail) return 0;
                pos += 64; /* bindingSig */
            }
        }
    }

    /* JoinSplits (v2+) */
    if (version >= 2 && (!overwintered || version < 5)) {
        if (pos >= avail) return 0;
        uint64_t njs;
        n = read_cs(data + pos, avail - pos, &njs);
        if (n == 0) return 0;
        pos += (size_t)n;
        if (njs > 0) {
            size_t js_size = (overwintered && vg_id == 0x892F2085)
                             ? 1698 : 1802;
            pos += (size_t)njs * js_size + 32 + 64;
        }
    }

    if (pos > avail) return 0;

    /* Compute txid = SHA256d(serialized tx) */
    {
        struct sha256_ctx ctx;
        uint8_t tmp[32];
        sha256_init(&ctx);
        sha256_write(&ctx, start, pos);
        sha256_finalize(&ctx, tmp);
        sha256_init(&ctx);
        sha256_write(&ctx, tmp, 32);
        sha256_finalize(&ctx, txid_out);
    }

    return pos;
}

static void *build_tx_index_thread(void *arg)
{
    struct snapshot_tx_index_job *job = arg;
    const char *datadir;
    const char *db_path;

    if (!job) {
        return NULL;
    }

    job->result = -1;
    datadir = job->args.datadir;
    db_path = job->args.db_path;

    struct node_db ndb;
    if (!node_db_open(&ndb, db_path)) {
        fprintf(stderr, "tx_index: failed to open SQLite\n");
        return NULL;
    }

    /* Check how many transactions already indexed */
    int existing = db_tx_count(&ndb);
    if (existing > 100000) {
        printf("tx_index: %d transactions already indexed, skipping\n",
               existing);
        node_db_close(&ndb);
        job->result = 0;
        return NULL;
    }

    printf("tx_index: building transaction index from block files...\n");
    fflush(stdout);
    int64_t t_start = (int64_t)time(NULL);

    if (!db_tx_prepare_bulk_load(&ndb)) {
        fprintf(stderr, "tx_index: failed to prepare bulk load\n");
        node_db_close(&ndb);
        return NULL;
    }

    /* Query blocks ordered by file_num, data_pos for sequential I/O */
    sqlite3_stmt *query = NULL;
    sqlite3_prepare_v2(ndb.db,
        "SELECT hash, height, file_num, data_pos, num_tx"
        " FROM blocks WHERE file_num >= 0"
        " ORDER BY file_num, data_pos",
        -1, &query, NULL);

    int indexed = 0;
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    node_db_begin(&ndb);

    while (sqlite3_step(query) == SQLITE_ROW) {
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
            if (fd < 0) { cached_data = NULL; cached_file = -1; continue; }
            struct stat st;
            if (fstat(fd, &st) != 0) { close(fd); cached_data = NULL; continue; }
            cached_size = (size_t)st.st_size;
            cached_data = mmap(NULL, cached_size, PROT_READ,
                               MAP_PRIVATE, fd, 0);
            close(fd);
            if (cached_data == MAP_FAILED) { cached_data = NULL; continue; }
            posix_madvise(cached_data, cached_size, POSIX_MADV_SEQUENTIAL);
            cached_file = file_num;
        }

        if (!cached_data || (size_t)data_pos >= cached_size) continue;

        /* Parse block header to find transaction data */
        const uint8_t *bdata = cached_data + data_pos;
        size_t bavail = cached_size - (size_t)data_pos;

        /* Skip header: 140 fixed + varint(solution) + solution */
        if (bavail < 141) continue;
        size_t pos = 140;
        uint64_t sol_size;
        int n = read_cs(bdata + pos, bavail - pos, &sol_size);
        if (n == 0) continue;
        pos += (size_t)n + (size_t)sol_size;

        /* num_tx */
        if (pos >= bavail) continue;
        uint64_t file_num_tx;
        n = read_cs(bdata + pos, bavail - pos, &file_num_tx);
        if (n == 0) continue;
        pos += (size_t)n;

        /* Parse each transaction for its txid */
        int tx_limit = num_tx > 0 ? num_tx : (int)file_num_tx;
        if (tx_limit > 50000) tx_limit = 50000;

        for (int ti = 0; ti < tx_limit && pos < bavail; ti++) {
            uint8_t txid[32];
            size_t tx_size = skip_tx_and_hash(bdata + pos, bavail - pos, txid);
            if (tx_size == 0) break;

            struct db_tx_index dt;
            memset(&dt, 0, sizeof(dt));
            memcpy(dt.txid, txid, 32);
            memcpy(dt.block_hash, block_hash, 32);
            dt.block_height = height;
            dt.tx_index = ti;
            dt.file_num = file_num;
            dt.file_pos = data_pos;
            dt.is_coinbase = (ti == 0);
            db_tx_save(&ndb, &dt);
            indexed++;

            pos += tx_size;
        }

        if (indexed % 500000 == 0 && indexed > 0) {
            node_db_commit(&ndb);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            int rate = elapsed > 0 ? indexed / (int)elapsed : indexed;
            printf("tx_index: %d transactions (%d/s)\n", indexed, rate);
            fflush(stdout);
            node_db_begin(&ndb);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);
    sqlite3_finalize(query);
    node_db_commit(&ndb);

    printf("tx_index: rebuilding indexes...\n");
    fflush(stdout);
    db_tx_finalize_bulk_load(&ndb);

    node_db_close(&ndb);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("tx_index: complete — %d transactions in %llds\n",
           indexed, (long long)elapsed);
    fflush(stdout);

    job->result = 0;
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
        return false;

    if (snprintf(job->args.db_path, sizeof(job->args.db_path),
                 "%s/node.db", c23_datadir) >=
        (int)sizeof(job->args.db_path)) {
        return false;
    }
    job->args.datadir = c23_datadir;
    job->result = -1;
    if (pthread_create(&job->thread, NULL, build_tx_index_thread, job) != 0) {
        return false;
    }
    job->started = true;
    return true;
}

bool snapshot_tx_index_job_join(struct snapshot_tx_index_job *job,
                                int *result_out)
{
    int join_rc;

    if (!job || !job->started)
        return false;

    join_rc = pthread_join(job->thread, NULL);
    if (join_rc != 0)
        return false;

    job->started = false;
    if (result_out)
        *result_out = job->result;
    return true;
}

bool snapshot_tx_index_job_is_started(const struct snapshot_tx_index_job *job)
{
    return job && job->started;
}
