/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "config/boot.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "coins/coins_view.h"
#include "storage/coins_db.h"
#include "consensus/validation.h"
#include "controllers/blockchain_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/network_controller.h"
#include "rpc/httpserver.h"
#include "controllers/mining_controller.h"
#include "controllers/transaction_controller.h"
#include "rpc/server.h"
#include "storage/block_index_db.h"
#include "storage/coins_db.h"
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
#include "wallet/wallet.h"
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
#include "storage/dbwrapper.h"
#include "net/tor_integration.h"
#include "net/https_server.h"
#include "net/fast_sync.h"
#include "net/peer_strategy.h"
#include "event/event.h"
#include "controllers/event_controller.h"
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
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>
#include <sqlite3.h>

static struct main_state g_state;
static struct coins_view_db g_coins_db;
static struct coins_view_cache g_coins_tip;
static struct block_tree_db g_block_tree;
struct block_tree_db *g_active_block_tree = NULL;
static bool g_block_tree_open = false;
static struct tx_mempool g_mempool;
static struct rpc_table g_rpc_table;
static struct msg_processor g_msg_processor;
static struct connman g_connman;
static struct wallet g_wallet;
struct wallet *g_active_wallet = NULL;
static struct gen_context g_gen;
static struct wallet_db g_wallet_db;
static struct node_db g_node_db;
struct node_db *g_active_node_db = NULL;
struct tx_mempool *g_active_mempool = NULL;
static const char *g_datadir = NULL;
const char *g_blog_datadir = NULL; /* used by connman for ZSLP peer discovery */
static _Atomic bool g_running = false;
static struct metrics_context g_metrics;

/* Background ZK param loading */
static char g_params_dir_buf[1024];
static pthread_t g_params_thread;
static _Atomic bool g_params_loaded = false;

static void *load_params_thread(void *arg)
{
    (void)arg;
    printf("Loading verification keys (background)...\n");
    fflush(stdout);
    if (sapling_init_params(g_params_dir_buf))
        atomic_store(&g_params_loaded, true);
    else
        fprintf(stderr, "Warning: Failed to load ZK params\n");
    printf("Verification keys loaded.\n");
    fflush(stdout);
    return NULL;
}

static int cmp_height(const void *a, const void *b);

/* Payment processor background thread */
extern void store_process_payments(const char *datadir);
static void *payment_processor_thread(void *arg)
{
    const char *datadir = (const char *)arg;
    while (1) {
        sleep(30);
        store_process_payments(datadir);
    }
    return NULL;
}

/* Adapter: bridges tor_request_handler_fn to onion_service_handle_request */
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

/* Background thread: build fast sync snapshot offer */
static void *build_snapshot_offer_thread(void *arg)
{
    const char *datadir = (const char *)arg;
    printf("Building fast sync snapshot offer...\n");
    fflush(stdout);

    extern struct snapshot_offer g_cached_offer;
    extern _Atomic bool g_cached_offer_valid;

    if (fast_sync_build_offer(datadir, &g_cached_offer)) {
        g_cached_offer_valid = true;
        printf("Fast sync ready: h=%d, %llu UTXOs\n",
               g_cached_offer.height,
               (unsigned long long)g_cached_offer.num_utxos);
    } else {
        printf("Fast sync: no snapshot available yet\n");
    }
    fflush(stdout);

    /* Build parallel chunk sync manifest (BitTorrent-style). */
    extern struct sync_manifest g_cached_manifest;
    extern _Atomic bool g_cached_manifest_valid;

    printf("Building chunk sync manifest...\n");
    fflush(stdout);
    if (fast_sync_build_manifest(datadir, &g_cached_manifest)) {
        g_cached_manifest_valid = true;
        printf("Chunk manifest ready: h=%d, %u chunks (%llu UTXOs)\n",
               g_cached_manifest.height, g_cached_manifest.num_chunks,
               (unsigned long long)g_cached_manifest.num_utxos);
    } else {
        printf("Chunk manifest: not available yet\n");
    }
    fflush(stdout);

    return NULL;
}

/* ── Flat block_index file: mmap for instant restart ──────── */

/* Compact on-disk format: 108 bytes per entry, height-sorted */
struct __attribute__((packed)) block_index_flat {
    uint8_t  hash[32];
    uint8_t  prev_hash[32];
    int32_t  height;
    uint32_t n_bits;
    uint32_t n_time;
    int32_t  n_version;
    uint32_t n_status;
    int32_t  n_file;
    uint32_t n_data_pos;
    uint32_t n_undo_pos;
    uint32_t n_tx;
    uint32_t n_chain_tx;
    uint8_t  chain_work[32];
    uint32_t n_cached_branch_id;
};
/* static_assert(sizeof(struct block_index_flat) == 140) -- close enough */

static void save_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);

    /* Sort by height */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = malloc(count * sizeof(void *));
    if (!sorted) return;

    size_t idx = 0, iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && idx < count) sorted[idx++] = p;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    int64_t t0 = (int64_t)time(NULL);
    FILE *f = fopen(path, "wb");
    if (!f) { free(sorted); return; }

    /* Header: magic + count */
    uint32_t magic = 0x5A434C49; /* "ZCLI" */
    fwrite(&magic, 4, 1, f);
    uint32_t cnt = (uint32_t)count;
    fwrite(&cnt, 4, 1, f);

    for (size_t i = 0; i < count; i++) {
        struct block_index_flat entry;
        memset(&entry, 0, sizeof(entry));
        if (sorted[i]->phashBlock)
            memcpy(entry.hash, sorted[i]->phashBlock->data, 32);
        if (sorted[i]->pprev && sorted[i]->pprev->phashBlock)
            memcpy(entry.prev_hash, sorted[i]->pprev->phashBlock->data, 32);
        entry.height = sorted[i]->nHeight;
        entry.n_bits = sorted[i]->nBits;
        entry.n_time = sorted[i]->nTime;
        entry.n_version = sorted[i]->nVersion;
        entry.n_status = sorted[i]->nStatus;
        entry.n_file = sorted[i]->nFile;
        entry.n_data_pos = sorted[i]->nDataPos;
        entry.n_undo_pos = sorted[i]->nUndoPos;
        entry.n_tx = sorted[i]->nTx;
        entry.n_chain_tx = sorted[i]->nChainTx;
        memcpy(entry.chain_work, sorted[i]->nChainWork.pn, 32);
        entry.n_cached_branch_id = (uint32_t)sorted[i]->nCachedBranchId;
        fwrite(&entry, sizeof(entry), 1, f);
    }
    fclose(f);
    free(sorted);

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index flat file: %zu entries, %zuMB (%llds)\n",
           count, count * sizeof(struct block_index_flat) / (1024*1024),
           (long long)elapsed);
    fflush(stdout);
}

static bool load_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return false; }
    size_t file_size = (size_t)st.st_size;
    if (file_size < 8) { close(fd); return false; }

    uint8_t *data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return false;

    /* Verify header */
    uint32_t magic, count;
    memcpy(&magic, data, 4);
    memcpy(&count, data + 4, 4);
    if (magic != 0x5A434C49) { munmap(data, file_size); return false; }

    size_t expected = 8 + (size_t)count * sizeof(struct block_index_flat);
    if (file_size < expected) { munmap(data, file_size); return false; }

    int64_t t0 = (int64_t)time(NULL);
    const struct block_index_flat *entries =
        (const struct block_index_flat *)(data + 8);

    /* Phase 1: Create all block_index entries */
    for (uint32_t i = 0; i < count; i++) {
        struct uint256 hash;
        memcpy(hash.data, entries[i].hash, 32);
        struct block_index *pindex = chainstate_insert_block_index(
            (struct chainstate *)ms, &hash);
        if (!pindex) continue;

        pindex->nHeight = entries[i].height;
        pindex->nBits = entries[i].n_bits;
        pindex->nTime = entries[i].n_time;
        pindex->nVersion = entries[i].n_version;
        pindex->nStatus = entries[i].n_status;
        pindex->nFile = entries[i].n_file;
        pindex->nDataPos = entries[i].n_data_pos;
        pindex->nUndoPos = entries[i].n_undo_pos;
        pindex->nTx = entries[i].n_tx;
        pindex->nChainTx = entries[i].n_chain_tx;
        memcpy(pindex->nChainWork.pn, entries[i].chain_work, 32);
        pindex->nCachedBranchId = entries[i].n_cached_branch_id;
    }

    /* Phase 2: Link pprev pointers (entries are height-sorted) */
    for (uint32_t i = 0; i < count; i++) {
        struct uint256 hash, prev;
        memcpy(hash.data, entries[i].hash, 32);
        memcpy(prev.data, entries[i].prev_hash, 32);
        if (uint256_is_null(&prev)) continue;

        struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
        struct block_index *pprev = block_map_find(&ms->map_block_index, &prev);
        if (pindex && pprev)
            pindex->pprev = pprev;
    }

    munmap(data, file_size);

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index flat: loaded %u entries in %llds\n",
           count, (long long)elapsed);
    fflush(stdout);

    return count > 0;
}

/* Save ALL block_index entries to SQLite for instant restart.
 * Replaces the previous approach of saving only 200 recent blocks.
 * On restart, load from SQLite instead of 10-15s LevelDB scan. */
static void save_block_index_recent(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) return;

    size_t total = ms->map_block_index.size;
    if (total == 0) return;

    int64_t t0 = (int64_t)time(NULL);
    sqlite3_exec(ndb->db, "DELETE FROM block_index_cache", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO block_index_cache "
        "(hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
        "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
        "n_cached_branch_id,n_chain_tx) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &ins, NULL);

    static const unsigned char zero32[32] = {0};
    size_t iter = 0, count = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || !p->phashBlock) continue;

        sqlite3_reset(ins);
        sqlite3_bind_blob(ins, 1, p->phashBlock->data, 32, SQLITE_STATIC);
        const unsigned char *prev = (p->pprev && p->pprev->phashBlock)
            ? p->pprev->phashBlock->data : zero32;
        sqlite3_bind_blob(ins, 2, prev, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 3, p->nHeight);
        sqlite3_bind_int(ins, 4, (int)p->nBits);
        sqlite3_bind_int(ins, 5, (int)p->nTime);
        sqlite3_bind_int(ins, 6, p->nVersion);
        sqlite3_bind_int(ins, 7, (int)p->nStatus);
        sqlite3_bind_int(ins, 8, p->nFile);
        sqlite3_bind_int(ins, 9, (int)p->nDataPos);
        sqlite3_bind_int(ins, 10, (int)p->nUndoPos);
        sqlite3_bind_int(ins, 11, (int)p->nTx);
        sqlite3_bind_blob(ins, 12, p->nChainWork.pn, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 13, (int)p->nCachedBranchId);
        sqlite3_bind_int(ins, 14, (int)p->nChainTx);
        sqlite3_step(ins);
        count++;

        /* Commit in batches of 50000 to avoid holding a huge transaction */
        if (count % 50000 == 0) {
            sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);
            sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
        }
    }
    sqlite3_finalize(ins);
    sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index: cached %zu/%zu entries in SQLite (%llds)\n",
           count, total, (long long)elapsed);
    fflush(stdout);
}

/* Load ALL block_index entries from SQLite.
 * Returns true if loaded successfully, false to fall back to LevelDB. */
static bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) return false;

    /* Check if block_index_cache has substantial data */
    int64_t cached_count = 0;
    sqlite3_stmt *cnt = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(*) FROM block_index_cache", -1, &cnt, NULL) == SQLITE_OK && cnt) {
        if (sqlite3_step(cnt) == SQLITE_ROW)
            cached_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (cached_count < 1000) return false;

    int64_t t0 = (int64_t)time(NULL);
    printf("Loading block index from SQLite (%lld entries)...\n",
           (long long)cached_count);
    fflush(stdout);

    /* Phase 1: Create all block_index entries */
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
            "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
            "n_cached_branch_id,n_chain_tx "
            "FROM block_index_cache ORDER BY height",
            -1, &sel, NULL) != SQLITE_OK || !sel)
        return false;

    size_t loaded = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const void *hash_blob = sqlite3_column_blob(sel, 0);
        if (!hash_blob || sqlite3_column_bytes(sel, 0) < 32) continue;

        struct uint256 hash;
        memcpy(hash.data, hash_blob, 32);
        struct block_index *pindex = chainstate_insert_block_index(
            (struct chainstate *)ms, &hash);
        if (!pindex) continue;

        pindex->nHeight         = sqlite3_column_int(sel, 2);
        pindex->nBits           = (uint32_t)sqlite3_column_int(sel, 3);
        pindex->nTime           = (uint32_t)sqlite3_column_int(sel, 4);
        pindex->nVersion        = sqlite3_column_int(sel, 5);
        pindex->nStatus         = (uint32_t)sqlite3_column_int(sel, 6);
        pindex->nFile           = sqlite3_column_int(sel, 7);
        pindex->nDataPos        = (uint32_t)sqlite3_column_int(sel, 8);
        pindex->nUndoPos        = (uint32_t)sqlite3_column_int(sel, 9);
        pindex->nTx             = (uint32_t)sqlite3_column_int(sel, 10);

        const void *cw = sqlite3_column_blob(sel, 11);
        if (cw && sqlite3_column_bytes(sel, 11) >= 32)
            memcpy(pindex->nChainWork.pn, cw, 32);

        pindex->nCachedBranchId = (uint32_t)sqlite3_column_int(sel, 12);
        pindex->nChainTx        = (uint32_t)sqlite3_column_int(sel, 13);
        loaded++;
    }
    sqlite3_finalize(sel);

    /* Phase 2: Link pprev pointers */
    sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash FROM block_index_cache "
            "WHERE prev_hash != X'0000000000000000000000000000000000000000000000000000000000000000'",
            -1, &sel, NULL) == SQLITE_OK && sel) {
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(sel, 0);
            const void *ph = sqlite3_column_blob(sel, 1);
            if (!h || !ph) continue;
            if (sqlite3_column_bytes(sel, 0) < 32 ||
                sqlite3_column_bytes(sel, 1) < 32) continue;

            struct uint256 hash, prev;
            memcpy(hash.data, h, 32);
            memcpy(prev.data, ph, 32);

            struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
            struct block_index *pprev = block_map_find(&ms->map_block_index, &prev);
            if (pindex && pprev)
                pindex->pprev = pprev;
        }
        sqlite3_finalize(sel);
    }

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index SQLite: loaded %zu entries in %llds\n",
           loaded, (long long)elapsed);
    fflush(stdout);

    return loaded > 0;
}

void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 8232;
    ctx->p2p_port = 8033;
    ctx->listen = false;
    ctx->checkpoints_enabled = true;
}

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)ms, hash);
}

static int cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1;
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

static bool load_block_index(struct main_state *ms,
                              const struct chain_params *params)
{
    if (g_block_tree_open) {
        if (!block_tree_db_load_block_index_guts(&g_block_tree,
                                                  insert_block_index_cb, ms))
            return false;
    }

    /* Fix phashBlock pointers — block_map rehashing invalidates them */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi)
                pi->phashBlock = hash;
        }
    }

    if (ms->map_block_index.size == 0) {
        struct block_index *genesis = chainstate_insert_block_index(
            (struct chainstate *)ms,
            &params->consensus.hashGenesisBlock);
        if (genesis) {
            genesis->nHeight = 0;
            genesis->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            genesis->nTx = 1;
            genesis->nChainTx = 1;
            genesis->nBits = 0x1f07ffff;
            genesis->nChainWork = GetBlockProof(genesis);
            /* Set genesis as active chain tip directly */
            active_chain_set_tip(&ms->chain_active, genesis);
            ms->pindex_best_header = genesis;
        }
        return true;
    }

    /* Post-load: compute nChainWork, nChainTx, skip links (like C++ LoadBlockIndexDB) */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = malloc(count * sizeof(struct block_index *));
    if (!sorted)
        return false;

    size_t idx = 0;
    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (pindex && idx < count)
            sorted[idx++] = pindex;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    for (size_t i = 0; i < count; i++) {
        pindex = sorted[i];

        /* Compute chain work */
        struct arith_uint256 proof = GetBlockProof(pindex);
        if (pindex->pprev)
            arith_uint256_add(&pindex->nChainWork,
                              &pindex->pprev->nChainWork, &proof);
        else
            pindex->nChainWork = proof;

        /* Compute chain tx count */
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx)
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                else
                    pindex->nChainTx = 0;
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }

        /* Build skip list */
        block_index_build_skip(pindex);

        /* Propagate cached branch ID */
        if (pindex->pprev) {
            if (block_index_is_valid(pindex, BLOCK_VALID_CONSENSUS) &&
                !pindex->nCachedBranchId)
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        /* Mark children of failed blocks */
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev &&
            (pindex->pprev->nStatus & BLOCK_FAILED_MASK))
            pindex->nStatus |= BLOCK_FAILED_CHILD;
    }

    free(sorted);
    return true;
}

/* ── Fast chainstate rebuild from SQLite UTXOs ─────────────
 * Instead of replaying 3M blocks (~2 hours), write the 1.6M UTXOs
 * from SQLite directly to LevelDB (~10 seconds). The UTXO set in
 * SQLite was populated during the original sync and is authoritative. */
static bool fast_rebuild_chainstate(struct coins_view_db *cvdb,
                                      struct coins_view_cache *cvtip,
                                      const char *datadir)
{
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return false;

    /* Count UTXOs */
    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM utxos", -1, &cnt, NULL);
    int64_t total = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW) total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    if (total == 0) { sqlite3_close(db); return false; }

    printf("fast-rebuild: %lld UTXOs from SQLite → LevelDB...\n",
           (long long)total);
    fflush(stdout);

    /* Wipe and reopen coins DB */
    coins_view_cache_flush(cvtip);
    coins_view_cache_free(cvtip);
    coins_view_db_close(cvdb);

    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", datadir);
    if (!coins_view_db_open(cvdb, coins_path, 450 << 20, false, true)) {
        sqlite3_close(db);
        return false;
    }
    coins_view_cache_init(cvtip, &cvdb->view);

    /* Stream UTXOs from SQLite into the coins cache */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(db,
        "SELECT txid, vout, value, script, height, is_coinbase "
        "FROM utxos ORDER BY txid, vout",
        -1, &s, NULL);

    int64_t count = 0;
    int64_t t_start = (int64_t)time(NULL);
    struct uint256 prev_txid;
    uint256_set_null(&prev_txid);
    struct coins_cache_entry *cur_entry = NULL;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const void *txid_blob = sqlite3_column_blob(s, 0);
        int vout = sqlite3_column_int(s, 1);
        int64_t value = sqlite3_column_int64(s, 2);
        const void *script = sqlite3_column_blob(s, 3);
        int script_len = sqlite3_column_bytes(s, 3);
        int height = sqlite3_column_int(s, 4);
        int is_cb = sqlite3_column_int(s, 5);

        if (!txid_blob || vout < 0) continue;

        struct uint256 txid;
        memcpy(txid.data, txid_blob, 32);

        /* New txid — flush previous, start new coins entry */
        if (!uint256_eq(&txid, &prev_txid)) {
            cur_entry = coins_view_cache_modify_new(cvtip, &txid);
            if (!cur_entry) continue;
            cur_entry->coins.height = height;
            cur_entry->coins.is_coinbase = (is_cb != 0);
            cur_entry->coins.version = 1;
            prev_txid = txid;
        }

        if (!cur_entry) continue;

        /* Ensure vout array is large enough */
        if ((size_t)vout >= cur_entry->coins.num_vout) {
            size_t new_size = (size_t)vout + 1;
            struct tx_out *new_vout = realloc(cur_entry->coins.vout,
                new_size * sizeof(struct tx_out));
            if (!new_vout) continue;
            for (size_t i = cur_entry->coins.num_vout; i < new_size; i++)
                tx_out_set_null(&new_vout[i]);
            cur_entry->coins.vout = new_vout;
            cur_entry->coins.num_vout = new_size;
        }

        cur_entry->coins.vout[vout].value = value;
        if (script && script_len > 0 &&
            (size_t)script_len <= MAX_SCRIPT_SIZE) {
            memcpy(cur_entry->coins.vout[vout].script_pub_key.data,
                   script, (size_t)script_len);
            cur_entry->coins.vout[vout].script_pub_key.size = (size_t)script_len;
        }

        count++;
        if (count % 100000 == 0) {
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            printf("  %lld / %lld UTXOs (%lld/s)\n",
                   (long long)count, (long long)total,
                   elapsed > 0 ? (long long)(count / elapsed) : (long long)count);
            fflush(stdout);
        }

        /* Flush cache periodically to avoid memory bloat */
        if (count % 500000 == 0)
            coins_view_cache_flush(cvtip);
    }
    sqlite3_finalize(s);

    /* Get tip hash from node_state */
    sqlite3_stmt *th = NULL;
    sqlite3_prepare_v2(db, "SELECT value FROM node_state WHERE key='tip_hash'",
                        -1, &th, NULL);
    struct uint256 tip_hash;
    uint256_set_null(&tip_hash);
    if (sqlite3_step(th) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(th, 0);
        if (h && sqlite3_column_bytes(th, 0) >= 32)
            memcpy(tip_hash.data, h, 32);
    }
    sqlite3_finalize(th);
    sqlite3_close(db);

    /* Final flush with tip hash */
    coins_view_cache_set_best_block(cvtip, &tip_hash);
    coins_view_cache_flush(cvtip);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("fast-rebuild: done — %lld UTXOs in %llds\n",
           (long long)count, (long long)elapsed);
    fflush(stdout);
    return true;
}

/* ── Populate SQLite UTXOs from LevelDB chainstate ─────────
 * Scans every coin entry in LevelDB and INSERTs into SQLite.
 * Runs after fast_rebuild_chainstate to ensure SQLite matches LevelDB. */
static void populate_sqlite_utxos_from_chainstate(struct coins_view_db *cvdb,
                                                     struct node_db *ndb)
{
    if (!ndb || !ndb->db || !cvdb) return;

    /* Clear stale UTXOs */
    sqlite3_exec(ndb->db, "DELETE FROM utxos", NULL, NULL, NULL);

    struct db_iterator it;
    db_iter_init(&it, &cvdb->db);

    char coins_prefix = 'c';
    db_iter_seek(&it, &coins_prefix, 1);

    int64_t count = 0;
    int64_t t_start = (int64_t)time(NULL);
    sqlite3_exec(ndb->db, "BEGIN TRANSACTION", NULL, NULL, NULL);

    while (db_iter_valid(&it)) {
        size_t klen = 0;
        const char *key = db_iter_key(&it, &klen);
        if (!key || klen < 1 || key[0] != 'c') break;

        /* Key = 'c' + 32-byte txid */
        if (klen != 33) { db_iter_next(&it); continue; }
        const uint8_t *txid = (const uint8_t *)key + 1;

        /* Read coins via the normal API using the txid */
        struct uint256 tid;
        memcpy(tid.data, txid, 32);
        struct coins coins;
        coins_init(&coins);

        if (coins_view_db_get_coins(cvdb, &tid, &coins)) {
            for (size_t i = 0; i < coins.num_vout; i++) {
                if (tx_out_is_null(&coins.vout[i])) continue;

                struct db_utxo u;
                memset(&u, 0, sizeof(u));
                memcpy(u.txid, txid, 32);
                u.vout = (uint32_t)i;
                u.value = coins.vout[i].value;
                memcpy(u.script, coins.vout[i].script_pub_key.data,
                       coins.vout[i].script_pub_key.size);
                u.script_len = coins.vout[i].script_pub_key.size;
                u.height = coins.height;
                u.is_coinbase = coins.is_coinbase;
                u.has_address = false;
                db_utxo_save(ndb, &u);
                count++;
            }
        }
        coins_free(&coins);

        if (count % 100000 == 0 && count > 0) {
            sqlite3_exec(ndb->db, "COMMIT; BEGIN TRANSACTION", NULL, NULL, NULL);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            printf("  populate utxos: %lld (%lld/s)\n",
                   (long long)count,
                   elapsed > 0 ? (long long)(count / elapsed) : (long long)count);
            fflush(stdout);
        }

        db_iter_next(&it);
    }

    sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);
    db_iter_free(&it);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("populate_sqlite_utxos: %lld UTXOs in %llds\n",
           (long long)count, (long long)elapsed);
    fflush(stdout);
}

static bool reindex_chainstate(struct main_state *ms,
                                struct coins_view_db *cvdb,
                                struct coins_view_cache *cvtip,
                                const char *datadir)
{
    int tip_height = active_chain_height(&ms->chain_active);
    if (tip_height < 0) {
        fprintf(stderr, "reindex-chainstate: no active chain\n");
        return false;
    }

    printf("reindex-chainstate: rebuilding UTXO set (%d blocks)...\n",
           tip_height + 1);
    fflush(stdout);

    /* Reduce mmap threshold to prevent heap fragmentation.
     * Each tx_out is ~10KB due to script[10000]. Coins with 4+ vouts
     * allocate >= 40KB. Using mmap for these larger allocations ensures
     * freed pages return to OS. Combined with malloc_trim after each
     * flush, this keeps RSS stable at ~6GB. */
    mallopt(M_MMAP_THRESHOLD, 32768);

    /* Flush and free the in-memory cache */
    coins_view_cache_flush(cvtip);
    coins_view_cache_free(cvtip);

    /* Close and reopen coins DB with wipe=true */
    coins_view_db_close(cvdb);

    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", datadir);
    if (!coins_view_db_open(cvdb, coins_path, 450 << 20, false, true)) {
        fprintf(stderr, "reindex-chainstate: failed to reopen coins DB\n");
        return false;
    }

    /* Reinitialize coins cache */
    coins_view_cache_init(cvtip, &cvdb->view);

    /* IBD mode: aggressive batching for throughput */
    set_flush_policy(3600, 1000000, 1000);
    if (g_node_db.open)
        node_db_set_sync_batch_size(&g_node_db, 100);

    const struct chain_params *cparams = chain_params_get();
    int64_t t_start = (int64_t)time(NULL);
    int errors = 0;

    for (int h = 0; h <= tip_height; h++) {
        struct block_index *pindex = active_chain_at(
            &ms->chain_active, h);
        if (!pindex) {
            printf("reindex-chainstate: missing block_index at height %d\n", h);
            errors++;
            continue;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, datadir)) {
            printf("reindex-chainstate: failed to read block at height %d\n", h);
            errors++;
            continue;
        }

        /* Connect through a nested cache so failed blocks are discarded
         * atomically. Without this, a block that fails partway through
         * connect_block leaves partial UTXO updates in the cache. */
        struct validation_state state;
        validation_state_init(&state);
        {
            struct coins_view_cache view;
            struct coins_view backing;
            coins_view_cache_as_view(&backing, cvtip);
            coins_view_cache_init(&view, &backing);

            if (!connect_block(&blk, &state, pindex, &view, cparams, false)) {
                printf("reindex-chainstate: connect_block failed at height %d: %s\n",
                       h, state.reject_reason);
                errors++;
                coins_view_cache_free(&view);
            } else {
                coins_view_cache_flush(&view);
                coins_view_cache_free(&view);
            }
        }

        block_free(&blk);

        /* Flush every 100 blocks or when cache exceeds 20K entries.
         * Each tx_out is ~10KB due to fixed script buffer, so 20K entries
         * uses ~400MB. Flushing at 100-block intervals keeps RSS under 200MB
         * for typical block sizes. */
        bool need_flush = (h % 100 == 0) ||
                          (cvtip->cache_coins.size > 20000);
        if (need_flush) {
            coins_view_cache_flush(cvtip);
            malloc_trim(0);
            if (h % 1000 == 0) {
                int64_t elapsed = (int64_t)time(NULL) - t_start;
                double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
                int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
                printf("  height %d/%d (%.0f blk/s, ETA %dm%ds, cache %zu)\n",
                       h, tip_height, rate, eta / 60, eta % 60,
                       cvtip->cache_coins.size);
                fflush(stdout);
            }
        }
    }

    /* Final flush */
    coins_view_cache_flush(cvtip);

    /* Restore normal mode after reindex */
    set_flush_policy(3600, 500000, 0);
    if (g_node_db.open) {
        node_db_sync_flush(&g_node_db);
        node_db_set_sync_batch_size(&g_node_db, 1);
    }

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("reindex-chainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    fflush(stdout);

    return errors == 0;
}

bool app_init(struct app_context *ctx)
{
    /* Initialize event log first — everything after this is observable */
    event_log_init();
    event_install_crash_handler();
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
    if (ctx->params_dir) {
        snprintf(g_params_dir_buf, sizeof(g_params_dir_buf), "%s", ctx->params_dir);
        pthread_create(&g_params_thread, NULL, load_params_thread, NULL);
    }

    /* Initialize wallet (before block index — needed for -importlegacy) */
    wallet_init(&g_wallet);

    char wallet_path[1024];
    snprintf(wallet_path, sizeof(wallet_path), "%s/wallet", ctx->datadir);
    if (wallet_db_open(&g_wallet_db, wallet_path)) {
        wallet_db_read_keys(&g_wallet_db, &g_wallet);
        wallet_db_read_txs(&g_wallet_db, &g_wallet);
        wallet_rebuild_spent_set(&g_wallet);
        wallet_db_read_sapling_keys(&g_wallet_db, &g_wallet);
        wallet_db_read_scripts(&g_wallet_db, &g_wallet);
        int saved_height = 0;
        if (wallet_db_read_scan_height(&g_wallet_db, &saved_height))
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

    if (g_wallet.keystore.num_keys == 0)
        wallet_top_up_key_pool(&g_wallet, DEFAULT_KEYPOOL_SIZE);
    printf("Wallet has %zu keys.\n", g_wallet.keystore.num_keys);
    g_active_wallet = &g_wallet;

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
        g_active_node_db = &g_node_db;
        node_db_migrate(&g_node_db, ctx->datadir);
        int db_tip = node_db_sync_get_tip_height(&g_node_db);
        if (db_tip >= 0)
            printf("SQLite tip: height=%d\n", db_tip);
    } else {
        fprintf(stderr, "Warning: SQLite database unavailable\n");
        event_emitf(EV_DB_ERROR, 0, "SQLite open failed at %s/node.db",
                    ctx->datadir);
    }

    /* Fast path: -importlegacy imports wallet data from legacy block files
     * and exits. No block index, no P2P, no RPC needed. */
    if (ctx->import_legacy_dir) {
        if (!g_active_node_db) {
            fprintf(stderr, "Error: SQLite database required for import\n");
            return false;
        }
        int result = legacy_import(ctx->import_legacy_dir,
                                    g_active_node_db, &g_wallet,
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
        if (!g_active_node_db) {
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
                            g_active_node_db, &g_wallet) < 0) {
            fprintf(stderr, "Warning: Snapshot import had errors\n");
        }

        /* Step 3: Build transaction index in background */
        snapshot_build_tx_index_bg(ctx->datadir);
    }

    /* -fastsync: copy block data + chainstate from legacy C++ node.
     * Stops zclassicd if running, copies all data, then starts normally.
     *
     * Usage: ./zclassic23 -fastsync /path/to/.zclassic
     *
     * The zclassicd LevelDB format (block_index + chainstate) is
     * wire-compatible — same CCoins serialization. So we can directly
     * load their databases. Block files are raw block data, identical
     * format. This gets us to the zclassicd tip instantly, then P2P
     * sync handles the delta. */
    if (ctx->fastsync_dir) {
        int64_t t_fs_start = (int64_t)time(NULL);
        printf("═══ Fast Sync from Legacy Node ═══\n");
        printf("Source: %s\n", ctx->fastsync_dir);
        printf("Target: %s\n\n", ctx->datadir);
        fflush(stdout);

        /* Check source exists */
        char src_test[1024];
        snprintf(src_test, sizeof(src_test), "%s/blocks", ctx->fastsync_dir);
        struct stat st_check;
        if (stat(src_test, &st_check) != 0) {
            fprintf(stderr, "ERROR: Source blocks dir not found: %s\n"
                    "  Usage: ./zclassic23 -fastsync ~/.zclassic\n",
                    src_test);
            return false;
        }

        char src[1024], dst[1024], cmd[4096];

        /* Step 1: Block files (largest — use rsync if available for speed) */
        snprintf(dst, sizeof(dst), "%s/blocks", ctx->datadir);
        mkdir(dst, 0700);
        snprintf(src, sizeof(src), "%s/blocks", ctx->fastsync_dir);

        /* Count source block files for progress */
        int num_blk = 0;
        int64_t total_bytes = 0;
        {
            char glob_path[1024];
            snprintf(glob_path, sizeof(glob_path), "%s/blk00000.dat", src);
            if (stat(glob_path, &st_check) == 0) {
                for (int f = 0; f < 9999; f++) {
                    snprintf(glob_path, sizeof(glob_path),
                             "%s/blk%05d.dat", src, f);
                    struct stat fst;
                    if (stat(glob_path, &fst) != 0) break;
                    total_bytes += fst.st_size;
                    num_blk++;
                }
            }
        }
        printf("  Block files: %d files, %.1f GB\n",
               num_blk, (double)total_bytes / (1024.0*1024.0*1024.0));
        printf("  Copying block files...\n");
        fflush(stdout);

        /* Use cp -al (hardlink) first, fall back to cp -a */
        snprintf(cmd, sizeof(cmd),
                 "cp -aln '%s'/blk*.dat '%s'/ 2>/dev/null || "
                 "cp -au '%s'/blk*.dat '%s'/ 2>/dev/null",
                 src, dst, src, dst);
        system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "cp -aln '%s'/rev*.dat '%s'/ 2>/dev/null || "
                 "cp -au '%s'/rev*.dat '%s'/ 2>/dev/null",
                 src, dst, src, dst);
        system(cmd);
        printf("  Block files: done\n");
        fflush(stdout);

        /* Step 2: Block index (LevelDB — must be clean copy) */
        snprintf(dst, sizeof(dst), "%s/blocks/index", ctx->datadir);
        printf("  Block index: clean copy...\n");
        fflush(stdout);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dst, dst);
        system(cmd);
        snprintf(src, sizeof(src), "%s/blocks/index", ctx->fastsync_dir);
        snprintf(cmd, sizeof(cmd), "cp -a '%s'/. '%s'/ 2>/dev/null", src, dst);
        system(cmd);
        printf("  Block index: done\n");
        fflush(stdout);

        /* Step 3: Chainstate (LevelDB — must be clean copy) */
        snprintf(dst, sizeof(dst), "%s/chainstate", ctx->datadir);
        printf("  Chainstate: clean copy...\n");
        fflush(stdout);
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dst, dst);
        system(cmd);
        snprintf(src, sizeof(src), "%s/chainstate", ctx->fastsync_dir);
        snprintf(cmd, sizeof(cmd), "cp -a '%s'/. '%s'/ 2>/dev/null", src, dst);
        system(cmd);
        printf("  Chainstate: done\n");
        fflush(stdout);

        int64_t t_fs_elapsed = (int64_t)time(NULL) - t_fs_start;
        printf("\n═══ Fast sync complete in %llds ═══\n\n",
               (long long)t_fs_elapsed);
        fflush(stdout);
    }

    /* Open block index database */
    char blocktree_path[1024];
    snprintf(blocktree_path, sizeof(blocktree_path), "%s/blocks/index",
             ctx->datadir);
    if (block_tree_db_open(&g_block_tree, blocktree_path,
                           256 << 20, false, false)) {
        g_block_tree_open = true;
        g_active_block_tree = &g_block_tree;
    } else {
        fprintf(stderr, "Warning: Could not open block tree DB at %s\n",
                blocktree_path);
    }

    /* Open coins database */
    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", ctx->datadir);
    if (!coins_view_db_open(&g_coins_db, coins_path,
                            DEFAULT_DB_CACHE << 20, false, false)) {
        fprintf(stderr, "Warning: Could not open coins DB at %s\n", coins_path);
    }

    coins_view_cache_init(&g_coins_tip, &g_coins_db.view);

    /* Wire UTXO commitment: load from LevelDB and set db pointer for
     * persistence on flush. This connects the incremental commitment
     * (updated per-UTXO in update_coins) to its LevelDB backing store. */
    set_coins_db_for_commitment(&g_coins_db);
    if (coins_view_db_read_commitment(&g_coins_db, &g_coins_tip.commitment)) {
        printf("Loaded UTXO commitment from LevelDB (count=%llu)\n",
               (unsigned long long)g_coins_tip.commitment.count);
    }

    /* Fast warm-restart check: verify SQLite tip matches coins DB tip.
     * If they match, skip the slow block index load (3M LevelDB reads)
     * and restore chain state from SQLite + coins DB directly. */
    bool skip_activate = false;
    bool fast_restart = false;
    int sqlite_tip_height = g_active_node_db ?
        node_db_sync_get_tip_height(&g_node_db) : -1;

    struct uint256 coins_best_hash;
    coins_view_cache_get_best_block(&g_coins_tip, &coins_best_hash);

    /* Block index is now cached in SQLite (load_block_index_sqlite).
     * The full index is saved on shutdown/save, enabling instant restart
     * without the 10-15s LevelDB scan. */
    (void)sqlite_tip_height;
    (void)coins_best_hash;

    /* Block index load: try SQLite first, then flat file, fall back to LevelDB */
    {
        bool loaded = false;
        if (!ctx->reindex_chainstate && g_active_node_db)
            loaded = load_block_index_sqlite(&g_node_db, &g_state);
        if (!loaded && !ctx->reindex_chainstate)
            loaded = load_block_index_flat(ctx->datadir, &g_state);

        /* Check if flat file is stale — if it loaded but has far fewer
         * entries than the chain (checked via SQLite), reload from LevelDB.
         * This fixes the case where an old flat file with 6K entries
         * prevents loading the full 3M+ entry index. */
        if (loaded && g_node_db.open) {
            int64_t db_height = 0;
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(g_node_db.db,
                    "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    db_height = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
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
            fflush(stdout);
            if (!load_block_index(&g_state, params)) {
                fprintf(stderr, "Warning: Failed to load block index\n");
            }
            int64_t t_idx_elapsed = (int64_t)time(NULL) - t_idx_start;
            printf("Block index loaded: %zu entries in %llds\n",
                   g_state.map_block_index.size, (long long)t_idx_elapsed);

            /* Save flat file for next restart */
            if (g_state.map_block_index.size > 1000)
                save_block_index_flat(ctx->datadir, &g_state);
        }

        /* Save recent blocks to SQLite */
        if (g_active_node_db && g_state.map_block_index.size > 1000)
            save_block_index_recent(&g_node_db, &g_state);
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

    /* Restore chain tip from coins DB best block hash */
    if (ctx->reindex_chainstate) {
        struct block_index *best = NULL;
        size_t fi = 0;
        struct block_index *fp;
        while (block_map_next(&g_state.map_block_index, &fi, NULL, &fp)) {
            if (fp && (fp->nStatus & BLOCK_HAVE_DATA) &&
                fp->nChainTx > 0 &&
                (!best || arith_uint256_compare(
                    &fp->nChainWork, &best->nChainWork) > 0))
                best = fp;
        }
        if (best) {
            active_chain_set_tip(&g_state.chain_active, best);
            g_state.pindex_best_header = best;
            printf("Chain tip from block index: height=%d\n", best->nHeight);
        }
        skip_activate = true;

        /* Fast path: rebuild from SQLite UTXOs (~10s vs ~2h).
         * Then repopulate SQLite from the authoritative LevelDB chainstate. */
        if (!fast_rebuild_chainstate(&g_coins_db, &g_coins_tip,
                                      ctx->datadir)) {
            printf("Fast rebuild unavailable, falling back to full reindex\n");
            if (!reindex_chainstate(&g_state, &g_coins_db, &g_coins_tip,
                                     ctx->datadir)) {
                fprintf(stderr, "Warning: Chainstate reindex had errors\n");
            }
        }
        /* Repopulate SQLite UTXOs from the authoritative LevelDB chainstate */
        if (g_active_node_db) {
            printf("Populating SQLite UTXOs from chainstate...\n");
            fflush(stdout);
            populate_sqlite_utxos_from_chainstate(&g_coins_db, g_active_node_db);
        }
    } else if (fast_restart) {
    } else if (g_state.map_block_index.size > 1) {
        struct uint256 best_hash;
        coins_view_cache_get_best_block(&g_coins_tip, &best_hash);
        if (!uint256_is_null(&best_hash)) {
            struct block_index *best = block_map_find(
                &g_state.map_block_index, &best_hash);
            if (best) {
                active_chain_set_tip(&g_state.chain_active, best);
                g_state.pindex_best_header = best;
                printf("Restored chain tip from coins DB: height=%d\n",
                       best->nHeight);
            } else {
                char hex[65];
                uint256_get_hex(&best_hash, hex);
                printf("Coins DB best block %s not in index.\n", hex);
                struct block_index *fallback = NULL;
                size_t fi = 0;
                struct block_index *fp;
                while (block_map_next(&g_state.map_block_index, &fi,
                                       NULL, &fp)) {
                    if (fp && (fp->nStatus & BLOCK_HAVE_DATA) &&
                        fp->nChainTx > 0 &&
                        (!fallback || arith_uint256_compare(
                            &fp->nChainWork, &fallback->nChainWork) > 0))
                        fallback = fp;
                }
                if (fallback) {
                    active_chain_set_tip(&g_state.chain_active, fallback);
                    g_state.pindex_best_header = fallback;
                    printf("Fallback chain tip: height=%d\n",
                           fallback->nHeight);
                    skip_activate = true;
                }
            }
        }

        /* If coins DB had no best block (null hash), fall back to the
         * highest-work block with data in the index. */
        if (uint256_is_null(&best_hash) ||
            active_chain_tip(&g_state.chain_active) == NULL) {
            struct block_index *fallback = NULL;
            size_t fi = 0;
            struct block_index *fp;
            while (block_map_next(&g_state.map_block_index, &fi,
                                   NULL, &fp)) {
                if (fp && (fp->nStatus & BLOCK_HAVE_DATA) &&
                    fp->nChainTx > 0 &&
                    (!fallback || arith_uint256_compare(
                        &fp->nChainWork, &fallback->nChainWork) > 0))
                    fallback = fp;
            }
            if (fallback) {
                active_chain_set_tip(&g_state.chain_active, fallback);
                g_state.pindex_best_header = fallback;
                printf("Chain tip from block index: height=%d\n",
                       fallback->nHeight);
                skip_activate = true;
            }
        }

        /* Find the best header (most chain work) */
        struct block_index *best_hdr = NULL;
        size_t iter = 0;
        struct block_index *pi;
        while (block_map_next(&g_state.map_block_index, &iter, NULL, &pi)) {
            if (pi && (!best_hdr ||
                arith_uint256_compare(&pi->nChainWork,
                                      &best_hdr->nChainWork) > 0))
                best_hdr = pi;
        }
        if (best_hdr)
            g_state.pindex_best_header = best_hdr;
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

    /* Detect chain tip / coins DB mismatch.
     * If chainstate/ was deleted but block index survives, the chain tip
     * is at height N but the UTXO set is empty. This causes all blocks
     * with non-coinbase txs to fail with "bad-txns-inputs-missingorspent".
     * Fix: reset chain tip to genesis and let activate_best_chain replay. */
    {
        struct block_index *chain_tip = active_chain_tip(&g_state.chain_active);
        struct uint256 coins_best;
        coins_view_cache_get_best_block(&g_coins_tip, &coins_best);

        if (chain_tip && chain_tip->nHeight > 0 &&
            uint256_is_null(&coins_best)) {
            printf("WARNING: Chain tip at height %d but coins DB is empty!\n"
                   "  Resetting chain to genesis — will replay %d blocks.\n",
                   chain_tip->nHeight, chain_tip->nHeight);
            fflush(stdout);

            struct block_index *genesis = block_map_find(
                &g_state.map_block_index, &params->consensus.hashGenesisBlock);
            if (genesis) {
                active_chain_set_tip(&g_state.chain_active, genesis);
                g_state.pindex_best_header = genesis;
            }
            skip_activate = false; /* MUST replay to rebuild UTXO set */

            /* Flush coins every 50K blocks during replay for crash safety */
            set_flush_policy(3600, 500000, 50000);
        } else if (chain_tip && chain_tip->nHeight > 0 &&
                   chain_tip->phashBlock &&
                   uint256_cmp(chain_tip->phashBlock, &coins_best) != 0) {
            /* Chain tip and coins DB disagree on best block.
             * Find the coins DB block in our index and reset to it. */
            struct block_index *coins_block = block_map_find(
                &g_state.map_block_index, &coins_best);
            if (coins_block && coins_block->nHeight < chain_tip->nHeight) {
                printf("Chain tip/coins mismatch: chain=%d coins=%d\n"
                       "  Resetting chain to coins DB tip — will replay %d blocks.\n",
                       chain_tip->nHeight, coins_block->nHeight,
                       chain_tip->nHeight - coins_block->nHeight);
                active_chain_set_tip(&g_state.chain_active, coins_block);
                skip_activate = false; /* replay delta */
            }
        }
    }

    /* Activate best chain (connects any new blocks beyond saved tip).
     * Skip for -fastsync and -reindex-chainstate. */
    if (ctx->fastsync_dir)
        skip_activate = true;
    if (!skip_activate) {
        struct validation_state vs;
        validation_state_init(&vs);
        if (!activate_best_chain(&vs, &g_state, &g_coins_tip, params, NULL,
                                 ctx->datadir)) {
            fprintf(stderr, "Warning: Failed to activate best chain\n");
        }
    }

    struct block_index *tip = active_chain_tip(&g_state.chain_active);
    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        printf("Chain tip: height=%d hash=%s\n", tip->nHeight, hex);
    } else {
        printf("Chain tip: genesis\n");
    }

    /* Backfill shielded values in SQLite from block_index.
     * The legacy importer didn't populate sprout_value/sapling_value.
     * Walk the in-memory block_index and UPDATE blocks that have nonzero values. */
    if (g_active_node_db && tip && tip->nHeight > 1000) {
        int64_t has_shielded = 0;
        {
            sqlite3_stmt *chk = NULL;
            if (sqlite3_prepare_v2(g_node_db.db,
                    "SELECT count(*) FROM blocks WHERE sprout_value != 0 OR sapling_value != 0",
                    -1, &chk, NULL) == SQLITE_OK && chk) {
                if (sqlite3_step(chk) == SQLITE_ROW)
                    has_shielded = sqlite3_column_int64(chk, 0);
                sqlite3_finalize(chk);
            }
        }
        if (has_shielded == 0) {
            printf("Backfilling shielded values from block_index...\n");
            fflush(stdout);
            sqlite3_exec(g_node_db.db, "BEGIN", NULL, NULL, NULL);
            sqlite3_stmt *upd = NULL;
            sqlite3_prepare_v2(g_node_db.db,
                "UPDATE blocks SET sprout_value=?, sapling_value=? "
                "WHERE height=?", -1, &upd, NULL);
            int updated = 0;
            size_t iter = 0;
            struct block_index *bi;
            while (block_map_next(&g_state.map_block_index, &iter, NULL, &bi)) {
                if (!bi) continue;
                if (bi->nSproutValue == 0 && bi->nSaplingValue == 0) continue;
                sqlite3_reset(upd);
                sqlite3_bind_int64(upd, 1, bi->nSproutValue);
                sqlite3_bind_int64(upd, 2, bi->nSaplingValue);
                sqlite3_bind_int(upd, 3, bi->nHeight);
                sqlite3_step(upd);
                updated++;
            }
            sqlite3_finalize(upd);
            sqlite3_exec(g_node_db.db, "COMMIT", NULL, NULL, NULL);
            printf("Backfill: updated %d blocks with shielded values\n", updated);
            fflush(stdout);
        }
    }

    /* Backfill addresses table from UTXOs if empty */
    if (g_active_node_db) {
        int64_t addr_count = 0;
        {
            sqlite3_stmt *chk = NULL;
            if (sqlite3_prepare_v2(g_node_db.db,
                    "SELECT count(*) FROM addresses", -1, &chk, NULL) == SQLITE_OK && chk) {
                if (sqlite3_step(chk) == SQLITE_ROW)
                    addr_count = sqlite3_column_int64(chk, 0);
                sqlite3_finalize(chk);
            }
        }
        if (addr_count == 0) {
            printf("Backfilling addresses from UTXO set...\n");
            fflush(stdout);
            sqlite3_exec(g_node_db.db,
                "INSERT OR REPLACE INTO addresses "
                "(address_hash, script_type, balance, utxo_count, "
                "first_seen_height, last_seen_height) "
                "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
                "MIN(height), MAX(height) "
                "FROM utxos WHERE address_hash IS NOT NULL "
                "GROUP BY address_hash",
                NULL, NULL, NULL);
            int64_t new_count = 0;
            {
                sqlite3_stmt *chk = NULL;
                if (sqlite3_prepare_v2(g_node_db.db,
                        "SELECT count(*) FROM addresses", -1, &chk, NULL) == SQLITE_OK && chk) {
                    if (sqlite3_step(chk) == SQLITE_ROW)
                        new_count = sqlite3_column_int64(chk, 0);
                    sqlite3_finalize(chk);
                }
            }
            printf("Backfill: populated %lld addresses\n", (long long)new_count);
            fflush(stdout);
        }
    }

    /* Initialize mempool */
    tx_mempool_init(&g_mempool, 1000);
    g_active_mempool = &g_mempool;

    /* Load persisted mempool from SQLite */
    if (g_active_node_db)
        node_db_sync_mempool_load(g_active_node_db, &g_mempool);

    /* Rescan blockchain for wallet transactions if wallet is behind chain tip.
     * Uses time_first_key with 2-hour safety margin to skip irrelevant blocks.
     * If no time_first_key is set (imported keys), scan from saved height only. */
    {
        struct block_index *chain_tip = active_chain_tip(&g_state.chain_active);
        int tip_height = active_chain_height(&g_state.chain_active);
        if (chain_tip && g_wallet.best_block_height < tip_height) {
            int scan_from = g_wallet.best_block_height > 0
                ? g_wallet.best_block_height + 1 : 0;
            if (g_wallet.time_first_key > 0 && scan_from == 0) {
                int64_t scan_time = g_wallet.time_first_key - 7200;
                for (int h = tip_height; h >= 0; h--) {
                    struct block_index *bi = active_chain_at(
                        &g_state.chain_active, h);
                    if (bi && (int64_t)bi->nTime < scan_time) {
                        scan_from = h + 1;
                        break;
                    }
                }
            }
            if (scan_from == 0 && g_wallet.best_block_height == 0 &&
                tip_height > 1000) {
                printf("Wallet scan height is 0 with %d blocks. "
                       "Use rescanblockchain RPC for targeted rescan.\n",
                       tip_height);
            } else if (tip_height - scan_from < 50000) {
                /* Small rescan: do it now */
                wallet_rescan(&g_wallet, &g_state.chain_active,
                              scan_from, tip_height, ctx->datadir);
            } else {
                /* Large rescan: defer to avoid blocking startup.
                 * Use rescanblockchain RPC to trigger manually. */
                printf("Wallet needs rescan from %d to %d (%d blocks). "
                       "Deferring — use rescanblockchain RPC.\n",
                       scan_from, tip_height, tip_height - scan_from);
            }
        }
    }

    wallet_verify_utxos(&g_wallet, &g_coins_tip);

    /* Rebuild wallet_utxos from ground truth:
     * global UTXO set × wallet_keys = authoritative balance.
     * This ensures getbalance is always correct, even after
     * fast sync, import, or stale data.
     * ATOMIC: wrapped in a single transaction so crash between
     * DELETE and INSERT can't leave wallet_utxos empty. */
    if (g_active_node_db && g_active_node_db->open) {
        int64_t t0 = (int64_t)time(NULL);
        char *err = NULL;
        sqlite3_exec(g_active_node_db->db, "BEGIN", NULL, NULL, NULL);
        sqlite3_exec(g_active_node_db->db,
            "DELETE FROM wallet_utxos", NULL, NULL, &err);
        if (err) { printf("wallet_utxos DELETE: %s\n", err); sqlite3_free(err); err = NULL; }
        int rc = sqlite3_exec(g_active_node_db->db,
            "INSERT INTO wallet_utxos "
            "(txid, vout, value, address_hash, script, height, is_coinbase) "
            "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
            "u.height, u.is_coinbase "
            "FROM utxos u INNER JOIN wallet_keys wk "
            "ON u.address_hash = wk.pubkey_hash",
            NULL, NULL, &err);
        if (err) { printf("wallet_utxos INSERT: %s\n", err); sqlite3_free(err); err = NULL; }
        if (rc != SQLITE_OK)
            sqlite3_exec(g_active_node_db->db, "ROLLBACK", NULL, NULL, NULL);
        else
            sqlite3_exec(g_active_node_db->db, "COMMIT", NULL, NULL, NULL);
        int64_t bal = 0;
        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT COALESCE(sum(value),0) FROM wallet_utxos "
            "WHERE spent_txid IS NULL", -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            bal = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        int cnt = 0;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            cnt = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        printf("Wallet: %.8f ZCL (%d UTXOs, %lldms)\n",
               (double)bal / 1e8, cnt,
               (long long)((int64_t)time(NULL) - t0) * 1000);
        fflush(stdout);

        /* ── Supply invariant check ──────────────────────────────
         * Fundamental blockchain identity:
         *   SUM(transparent UTXOs) + shielded_pool = mined_supply
         * If this doesn't hold, the UTXO index is corrupt.
         * This is the cryptographic proof that our data is correct. */
        int64_t utxo_sum = 0, shielded_pool = 0;
        int64_t idx_height = 0;
        s = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT COALESCE(SUM(value),0) FROM utxos",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            utxo_sum = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        s = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT COALESCE(SUM(sapling_value),0) FROM blocks",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            shielded_pool = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        s = NULL;
        sqlite3_prepare_v2(g_active_node_db->db,
            "SELECT MAX(height) FROM blocks WHERE status >= 3",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            idx_height = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);

        if (idx_height > 0) {
            int64_t mined = zcl_total_supply_zatoshi(idx_height);
            int64_t actual = utxo_sum + shielded_pool;
            int64_t delta = actual - mined;
            if (delta == 0) {
                printf("Supply check: UTXO+shielded = mined = %.8f ZCL "
                       "(height %lld) ✓\n",
                       (double)mined / 1e8, (long long)idx_height);
            } else {
                printf("WARNING: Supply mismatch at height %lld!\n"
                       "  UTXO sum:       %.8f ZCL\n"
                       "  Shielded pool:  %.8f ZCL\n"
                       "  Actual total:   %.8f ZCL\n"
                       "  Expected mined: %.8f ZCL\n"
                       "  Delta:          %.8f ZCL (%lld zatoshi)\n",
                       (long long)idx_height,
                       (double)utxo_sum / 1e8,
                       (double)shielded_pool / 1e8,
                       (double)actual / 1e8,
                       (double)mined / 1e8,
                       (double)delta / 1e8, (long long)delta);
            }
            fflush(stdout);
        }
    }

    /* Sync wallet keys to SQLite */
    if (g_active_node_db)
        node_db_sync_wallet_keys(g_active_node_db, &g_wallet);

    /* Initialize message processor */
    msg_processor_init(&g_msg_processor, &g_state, &g_mempool,
                       &g_coins_tip, params, ctx->datadir,
                       &g_connman.manager);

    /* Initialize P2P connection manager */
    struct node_signals signals = {
        .get_height = msg_get_height,
        .process_messages = msg_process_messages,
        .send_messages = msg_send_messages,
        .initialize_node = NULL,
        .finalize_node = NULL,
        .ctx = &g_msg_processor,
    };
    connman_init(&g_connman, params, &signals);

    /* Load saved peer addresses */
    addr_db_read(&g_connman.manager, ctx->datadir);

    if (ctx->listen) {
        /* Bind IPv4 0.0.0.0 */
        struct net_service bind4;
        net_service_init(&bind4);
        unsigned char any4[4] = {0, 0, 0, 0};
        net_addr_set_ipv4(&bind4.addr, any4);
        bind4.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&g_connman.manager, &bind4, false))
            printf("P2P listening on 0.0.0.0:%d\n", ctx->p2p_port);
        /* Bind IPv6 [::] */
        struct net_service bind6;
        net_service_init(&bind6);
        bind6.port = (uint16_t)ctx->p2p_port;
        if (bind_listen_port(&g_connman.manager, &bind6, false))
            printf("P2P listening on [::]:%d\n", ctx->p2p_port);
    }

    /* Wait for ZK params before P2P (needed for block verification) */
    if (ctx->params_dir) {
        pthread_join(g_params_thread, NULL);
        if (!atomic_load(&g_params_loaded))
            fprintf(stderr, "Warning: ZK params not loaded\n");
    }

    connman_start(&g_connman);
    sync_set_state(SYNC_FINDING_PEERS, "P2P started");

    /* Initialize RPC */
    rpc_table_init(&g_rpc_table);
    rpc_blockchain_set_state(&g_state, &g_mempool, ctx->datadir);
    rpc_blockchain_set_coins_db(&g_coins_db, &g_coins_tip);
    rpc_blockchain_set_node_db(g_active_node_db);
    register_blockchain_rpc_commands(&g_rpc_table);

    rpc_chain_inspect_set_state(&g_state, ctx->datadir,
                                 &g_coins_db, &g_coins_tip, g_active_node_db);
    register_chain_inspect_rpc_commands(&g_rpc_table);

    explorer_set_state(&g_state, &g_mempool, &g_coins_tip,
                        g_active_node_db, ctx->datadir);

    api_set_state(&g_state, &g_mempool, &g_coins_tip,
                   g_active_node_db, ctx->datadir);

    rpc_rawtx_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    rpc_rawtx_set_keystore(&g_wallet.keystore);
    rpc_rawtx_set_connman(&g_connman);
    register_rawtransaction_rpc_commands(&g_rpc_table);

    rpc_mining_set_state(&g_state, &g_mempool, &g_coins_tip, ctx->datadir);
    register_mining_rpc_commands(&g_rpc_table);

    rpc_misc_set_state(&g_state);
    rpc_misc_set_wallet(&g_wallet);
    register_misc_rpc_commands(&g_rpc_table);
    rpc_net_set_connman(&g_connman);
    register_net_rpc_commands(&g_rpc_table);

    rpc_wallet_set_state(&g_wallet, &g_state, ctx->datadir, &g_wallet_db,
                         &g_mempool, &g_connman);
    rpc_wallet_set_coins_tip(&g_coins_tip);
    rpc_wallet_set_node_db(g_active_node_db);
    register_wallet_rpc_commands(&g_rpc_table);
    register_event_rpc_commands(&g_rpc_table);

    /* Pre-compute fast sync snapshot offer in background */
    {
        static char s_offer_datadir[1024];
        snprintf(s_offer_datadir, sizeof(s_offer_datadir), "%s", ctx->datadir);
        static pthread_t offer_thread;
        pthread_create(&offer_thread, NULL, build_snapshot_offer_thread,
                        s_offer_datadir);
        pthread_detach(offer_thread);
    }

    /* Start RPC HTTP server (clearnet: auth RPC only, no blog) */
    set_rpc_warmup_finished();
    rpc_http_start(&g_rpc_table, (uint16_t)ctx->rpc_port,
                    ctx->rpc_user, ctx->rpc_password, ctx->datadir);

    /* Start public HTTPS block explorer on port 443 */
    {
        char cert_path[1024], key_path[1024];
        snprintf(cert_path, sizeof(cert_path), "%s/ssl/fullchain.pem",
                 ctx->datadir);
        snprintf(key_path, sizeof(key_path), "%s/ssl/privkey.pem",
                 ctx->datadir);
        if (access(cert_path, R_OK) == 0 && access(key_path, R_OK) == 0) {
            https_server_start(cert_path, key_path, "zclnet.net");
        } else {
            printf("HTTPS: no cert at %s — block explorer not on clearnet\n",
                   cert_path);
        }
    }

    /* Start miner if -gen */
    if (ctx->gen) {
        g_gen.ms = &g_state;
        g_gen.coins_tip = &g_coins_tip;
        g_gen.mempool = &g_mempool;
        g_gen.params = params;
        g_gen.datadir = ctx->datadir;
        g_gen.num_threads = ctx->gen_threads > 0 ? ctx->gen_threads : 1;
        g_gen.coinbase_script.size = 0;

        if (ctx->miner_address) {
            size_t pk_pfx_len, sc_pfx_len;
            const unsigned char *pk_pfx = chain_params_base58_prefix(
                params, B58_PUBKEY_ADDRESS, &pk_pfx_len);
            const unsigned char *sc_pfx = chain_params_base58_prefix(
                params, B58_SCRIPT_ADDRESS, &sc_pfx_len);
            struct tx_destination dest;
            if (decode_destination(ctx->miner_address, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, &dest))
                script_for_destination(&g_gen.coinbase_script, &dest);
        }

        gen_start(&g_gen);
    }

    /* Start embedded Tor with .onion hosting if -tor flag set */
    if (ctx->tor) {
        /* Initialize onion service handler before Tor starts */
        extern const char *onion_service_start(const char *);
        onion_service_start(ctx->datadir);

        tor_integration_set_handler(onion_request_adapter, NULL);

        printf("Starting embedded Tor...\n");
        fflush(stdout);
        if (!tor_integration_start(ctx->datadir, (uint16_t)ctx->p2p_port))
            fprintf(stderr, "Warning: Tor failed to start\n");

        /* Auto-announce .onion address on-chain via ZSLP */
        const char *onion = tor_integration_get_onion_address();
        if (onion) {
            extern bool blog_auto_announce_onion(const char *, const char *);
            if (blog_auto_announce_onion(ctx->datadir, onion))
                printf("Published .onion address on-chain: %s\n", onion);
        }
    }

    /* Discover peer reachability (NAT-PMP, UPnP, Tor) */
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
        fflush(stdout);
    }

    /* Start store payment processor (checks every 30s) */
    {
        static char s_pay_datadir[1024];
        snprintf(s_pay_datadir, sizeof(s_pay_datadir), "%s", ctx->datadir);
        static pthread_t payment_thread;
        pthread_create(&payment_thread, NULL, payment_processor_thread,
                        s_pay_datadir);
        pthread_detach(payment_thread);
    }

    atomic_store(&g_running, true);
    {
        struct block_index *tip = active_chain_tip(&g_state.chain_active);
        int h = tip ? tip->nHeight : 0;
        event_emitf(EV_NODE_READY, 0, "height=%d peers=%zu",
                    h, g_connman.manager.num_nodes);
    }
    printf("ZClassic C23 node initialized.\n");

    /* SQLite catchup: synchronous for -fastsync (full import before P2P),
     * background thread otherwise. */
    if (g_active_node_db) {
        if (ctx->fastsync_dir) {
            /* Full synchronous import: block index + UTXO set.
             * Must complete before P2P starts so the node is fully
             * indexed and ready to serve RPC queries immediately. */
            printf("Running full synchronous SQLite import...\n");
            fflush(stdout);
            node_db_sync_catchup(g_active_node_db,
                                 &g_state.chain_active,
                                 &g_wallet, ctx->datadir);
            node_db_sync_import_utxos(g_active_node_db, &g_coins_db);
            printf("Full import complete. Node ready.\n");
            fflush(stdout);
        } else {
            static pthread_t catchup_thread;
            static struct {
                struct node_db *ndb;
                const struct active_chain *chain;
                const struct wallet *w;
                const char *datadir;
            } catchup_args;
            catchup_args.ndb = g_active_node_db;
            catchup_args.chain = &g_state.chain_active;
            catchup_args.w = &g_wallet;
            catchup_args.datadir = ctx->datadir;
            pthread_create(&catchup_thread, NULL, (void *(*)(void *))
                node_db_sync_catchup_thread, &catchup_args);
            pthread_detach(catchup_thread);
        }
    }

    return true;
}

void app_shutdown(void)
{
    atomic_store(&g_running, false);
    event_emitf(EV_NODE_SHUTDOWN, 0, "graceful");

    printf("Shutting down...\n");

    tor_integration_stop();

    if (g_gen.running)
        gen_stop(&g_gen);

    rpc_http_stop();

    /* Save block index flat file for instant next restart */
    if (g_state.map_block_index.size > 1000) {
        printf("Saving block index flat file (%zu entries)...\n",
               g_state.map_block_index.size);
        fflush(stdout);
        save_block_index_flat(g_datadir, &g_state);
    }

    /* Save peer addresses */
    addr_db_write(&g_connman.manager, g_datadir);

    connman_stop(&g_connman);
    connman_free(&g_connman);

    coins_view_cache_flush(&g_coins_tip);
    coins_view_cache_free(&g_coins_tip);
    coins_view_db_close(&g_coins_db);

    if (g_block_tree_open) {
        block_tree_db_close(&g_block_tree);
        g_block_tree_open = false;
    }

    if (g_wallet_db.open) {
        wallet_db_flush(&g_wallet_db, &g_wallet);
        wallet_db_close(&g_wallet_db);
    }
    if (g_node_db.open) {
        node_db_sync_flush(&g_node_db);
        node_db_sync_mempool_save(&g_node_db, &g_mempool);
        node_db_close(&g_node_db);
    }
    g_active_node_db = NULL;
    wallet_free(&g_wallet);
    tx_mempool_free(&g_mempool);
    main_state_free(&g_state);
    sapling_free_params();

    ecc_verify_destroy();
    ecc_stop();

    printf("Shutdown complete.\n");
}

bool app_is_running(void)
{
    return atomic_load(&g_running);
}

void app_add_node(const char *host, int port)
{
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

    /* Parse host:port if port embedded in string */
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
                                 : g_connman.manager.default_port;

    /* Resolve and connect directly (don't rely on addrman random selection) */
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
        connman_open_connection(&g_connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

void app_start_metrics(bool mining)
{
    g_metrics.ms = &g_state;
    g_metrics.cm = &g_connman;
    g_metrics.params = chain_params_get();
    g_metrics.mining = mining;
    metrics_start(&g_metrics);
}

void app_stop_metrics(void)
{
    metrics_stop(&g_metrics);
}
