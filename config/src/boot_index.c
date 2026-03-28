/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Block index management: flat file save/load, SQLite cache,
 * chainstate rebuild/reindex, address backfill. */

#include "config/boot_internal.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "validation/connect_block.h"
#include "storage/block_index_db.h"
#include "storage/disk_block_io.h"
#include "coins/coins_view.h"
#include "event/event.h"
#include "crypto/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <malloc.h>
#include <sqlite3.h>

/* ── Flat block_index file: mmap for instant restart ──────── */

/* Compact on-disk format: height-sorted */
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

static int cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1;
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

void save_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);

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
}

bool load_block_index_flat(const char *datadir, struct main_state *ms)
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

    uint32_t magic, count;
    memcpy(&magic, data, 4);
    memcpy(&count, data + 4, 4);
    if (magic != 0x5A434C49) { munmap(data, file_size); return false; }

    size_t expected = 8 + (size_t)count * sizeof(struct block_index_flat);
    if (file_size < expected) { munmap(data, file_size); return false; }

    int64_t t0 = (int64_t)time(NULL);
    const struct block_index_flat *entries =
        (const struct block_index_flat *)(data + 8);

    /* Pre-size hash map + arena. Pre-fault memory. */
    block_map_reserve(&ms->map_block_index, count);
    struct block_index *arena = calloc(count, sizeof(struct block_index));
    if (!arena) { munmap(data, file_size); return false; }
    memset(arena, 0, count * sizeof(struct block_index)); /* pre-fault */

    /* Height→index for O(1) pprev linking */
    int32_t max_h = entries[count - 1].height;
    struct block_index **by_height = NULL;
    if (max_h > 0 && max_h < 10000000) {
        by_height = calloc((size_t)(max_h + 1), sizeof(struct block_index *));
    }

    /* Phase 1: bulk insert — direct hash table, no locks */
    struct block_map *bm = &ms->map_block_index;
    for (uint32_t i = 0; i < count; i++) {
        if (uint256_is_null((const struct uint256 *)entries[i].hash))
            continue;

        struct block_index *pindex = &arena[i];
        block_index_init(pindex);

        uint64_t h;
        memcpy(&h, entries[i].hash, 8);
        size_t slot = h & (bm->capacity - 1);
        while (bm->buckets[slot].occupied)
            slot = (slot + 1) & (bm->capacity - 1);
        memcpy(bm->buckets[slot].hash.data, entries[i].hash, 32);
        bm->buckets[slot].index = pindex;
        bm->buckets[slot].occupied = true;
        bm->size++;

        pindex->phashBlock = &bm->buckets[slot].hash;
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

        if (by_height && pindex->nHeight >= 0 && pindex->nHeight <= max_h)
            by_height[pindex->nHeight] = pindex;
    }

    /* Phase 2: Link pprev via height index (O(1) per entry) */
    for (uint32_t i = 0; i < count; i++) {
        struct block_index *pindex = &arena[i];
        if (pindex->nHeight > 0 && by_height) {
            struct block_index *pp = by_height[pindex->nHeight - 1];
            if (pp) pindex->pprev = pp;
        }
    }
    free(by_height);

    munmap(data, file_size);

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index flat: loaded %u entries in %llds\n",
           count, (long long)elapsed);

    return count > 0;
}

/* Save ALL block_index entries to SQLite for instant restart. */
void save_block_index_recent(struct node_db *ndb, struct main_state *ms)
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
}

/* Load ALL block_index entries from SQLite. */
bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) return false;

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

    return loaded > 0;
}

/* ── load_block_index: post-load chain work, skip list ─────── */

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)ms, hash);
}

bool load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open)
{
    if (btdb_open) {
        if (!block_tree_db_load_block_index_guts(btdb,
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
            active_chain_set_tip(&ms->chain_active, genesis);
            ms->pindex_best_header = genesis;
        }
        return true;
    }

    /* Post-load: compute nChainWork, nChainTx, skip links */
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

        struct arith_uint256 proof = GetBlockProof(pindex);
        if (pindex->pprev)
            arith_uint256_add(&pindex->nChainWork,
                              &pindex->pprev->nChainWork, &proof);
        else
            pindex->nChainWork = proof;

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

        block_index_build_skip(pindex);

        if (pindex->pprev) {
            if (block_index_is_valid(pindex, BLOCK_VALID_CONSENSUS) &&
                !pindex->nCachedBranchId)
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev &&
            (pindex->pprev->nStatus & BLOCK_FAILED_MASK))
            pindex->nStatus |= BLOCK_FAILED_CHILD;
    }

    free(sorted);
    return true;
}

/* ── Fast chainstate rebuild from SQLite UTXOs ─────────────── */

bool fast_rebuild_chainstate(struct coins_view_sqlite *cvs,
                              struct coins_view_cache *cvtip,
                              const char *datadir)
{
    (void)datadir;
    if (!cvs->db) return false;

    sqlite3_stmt *cnt = NULL;
    sqlite3_prepare_v2(cvs->db, "SELECT count(*) FROM utxos", -1, &cnt, NULL);
    int64_t total = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW) total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    if (total == 0) return false;

    printf("SQLite UTXO set: %lld UTXOs (canonical)\n", (long long)total);

    struct uint256 best;
    if (!coins_view_sqlite_get_best_block(cvs, &best) || uint256_is_null(&best)) {
        sqlite3_stmt *th = NULL;
        sqlite3_prepare_v2(cvs->db,
            "SELECT value FROM node_state WHERE key='tip_hash'",
            -1, &th, NULL);
        if (sqlite3_step(th) == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(th, 0);
            if (h && sqlite3_column_bytes(th, 0) >= 32) {
                memcpy(best.data, h, 32);
                sqlite3_stmt *set = NULL;
                sqlite3_prepare_v2(cvs->db,
                    "INSERT OR REPLACE INTO node_state(key,value)"
                    " VALUES('coins_best_block',?)", -1, &set, NULL);
                sqlite3_bind_blob(set, 1, best.data, 32, SQLITE_STATIC);
                sqlite3_step(set);
                sqlite3_finalize(set);
            }
        }
        sqlite3_finalize(th);
        coins_view_cache_set_best_block(cvtip, &best);
    }

    return true;
}

/* ── Full chainstate reindex: replay all blocks ────────────── */

bool reindex_chainstate(struct main_state *ms,
                          struct coins_view_sqlite *cvs,
                          struct coins_view_cache *cvtip,
                          struct node_db *ndb,
                          const char *datadir)
{
    int tip_height = active_chain_height(&ms->chain_active);
    if (tip_height < 0) {
        fprintf(stderr, "reindex-chainstate: no active chain\n");
        return false;
    }

    printf("reindex-chainstate: rebuilding UTXO set (%d blocks)...\n",
           tip_height + 1);
    event_emitf(EV_SYNC_STATE_CHANGE, 0, "reindex start blocks=%d",
                tip_height + 1);

    mallopt(M_MMAP_THRESHOLD, 32768);

    coins_view_cache_flush(cvtip);
    coins_view_cache_free(cvtip);

    (void)cvs;
    if (ndb->open) {
        sqlite3_exec(ndb->db, "DELETE FROM utxos", NULL, NULL, NULL);
        sqlite3_exec(ndb->db,
            "DELETE FROM node_state WHERE key='coins_best_block'",
            NULL, NULL, NULL);
        sqlite3_exec(ndb->db,
            "DELETE FROM node_state WHERE key='utxo_commitment'",
            NULL, NULL, NULL);
        printf("reindex-chainstate: wiped SQLite UTXO set\n");
    }

    coins_view_cache_init(cvtip, &cvs->view);

    set_flush_policy(3600, 1000000, 100000);
    if (ndb->open) {
        sqlite3_exec(ndb->db, "PRAGMA synchronous=OFF",
                     NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA cache_size=-524288",
                     NULL, NULL, NULL);
        sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=0",
                     NULL, NULL, NULL);
        node_db_set_sync_batch_size(ndb, 1000);
    }

    extern _Atomic bool g_utxo_commitment_skip;
    atomic_store(&g_utxo_commitment_skip, true);

    if (ndb->open) {
        node_db_state_set(ndb, "sapling_tree", NULL, 0);
        node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);
    }

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

        struct validation_state state;
        validation_state_init(&state);
        if (!connect_block(&blk, &state, pindex, cvtip, cparams, false)) {
            printf("reindex-chainstate: connect_block failed at height %d: %s\n",
                   h, state.reject_reason);
            errors++;
        }

        block_free(&blk);

        bool need_flush = (h % 10000 == 0) ||
                          (cvtip->cache_coins.size > 200000);
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
            }
        }
    }

    coins_view_cache_flush(cvtip);

    atomic_store(&g_utxo_commitment_skip, false);

    /* Restore normal mode — flush every 500 blocks */
    set_flush_policy(3600, 500000, 500);
    if (ndb->open) {
        node_db_sync_flush(ndb);
        node_db_set_sync_batch_size(ndb, 1);
    }

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("reindex-chainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    event_emitf(EV_SYNC_STATE_CHANGE, 0, "reindex complete %dm%ds errors=%d",
                (int)(elapsed / 60), (int)(elapsed % 60), errors);

    return errors == 0;
}

/* ── Background address backfill from UTXOs ────────────────── */

void *backfill_addresses_thread(void *arg)
{
    const char *db_path = (const char *)arg;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        printf("Address backfill: failed to open db\n");
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 60000);

    printf("Address backfill: aggregating UTXOs...\n");
    fflush(stdout);
    int64_t t0 = (int64_t)time(NULL);

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
        NULL, NULL, NULL);

    sqlite3_exec(db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);

    int64_t elapsed = (int64_t)time(NULL) - t0;

    int64_t new_count = 0;
    sqlite3_stmt *chk = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM addresses",
                           -1, &chk, NULL) == SQLITE_OK && chk) {
        if (sqlite3_step(chk) == SQLITE_ROW)
            new_count = sqlite3_column_int64(chk, 0);
        sqlite3_finalize(chk);
    }

    sqlite3_exec(db,
        "INSERT OR REPLACE INTO node_state(key,value) "
        "VALUES('addresses_backfilled', X'01')", NULL, NULL, NULL);

    sqlite3_close(db);
    printf("Address backfill: %lld addresses in %llds\n",
           (long long)new_count, (long long)elapsed);
    fflush(stdout);
    return NULL;
}

/* ── scan_block_files_mark_data ──────────────────────────────── */

int scan_block_files_mark_data(struct main_state *ms, const char *datadir)
{
    int marked = 0;
    char path[576];

    for (int file_idx = 0; file_idx < 256; file_idx++) {
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, file_idx);
        FILE *f = fopen(path, "rb");
        if (!f) break;

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        long pos = 0;
        while (pos + 4 + 4 + 80 <= file_size) {
            /* Block file format: [4-byte magic][4-byte size][block data] */
            uint8_t hdr[8];
            if (fread(hdr, 1, 8, f) != 8) break;

            uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                             ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            uint32_t blk_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                                ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);

            /* Validate magic (mainnet: 24e92764) */
            if (magic != 0x6427e924 && magic != 0x6427E924) {
                /* Try skipping to find next magic */
                pos += 1;
                fseek(f, pos, SEEK_SET);
                continue;
            }

            if (blk_size < 80 || blk_size > 2000000 ||
                pos + 8 + (long)blk_size > file_size) {
                pos += 8;
                fseek(f, pos, SEEK_SET);
                continue;
            }

            /* Read just the 80-byte block header to get the hash */
            uint8_t block_hdr[80];
            if (fread(block_hdr, 1, 80, f) != 80) break;

            /* Compute block hash (SHA-256d of first 80 bytes,
             * but we need to use our block_header_hash function).
             * For speed, use the raw double-SHA256: */
            struct uint256 hash;
            {
                struct sha256_ctx sctx;
                uint8_t tmp[32];
                sha256_init(&sctx);
                sha256_write(&sctx, block_hdr, 80);
                sha256_finalize(&sctx, tmp);
                sha256_init(&sctx);
                sha256_write(&sctx, tmp, 32);
                sha256_finalize(&sctx, hash.data);
            }

            /* Look up in block_index */
            struct block_index *bi = block_map_find(
                &ms->map_block_index, &hash);
            if (bi && !(bi->nStatus & BLOCK_HAVE_DATA)) {
                bi->nStatus |= BLOCK_HAVE_DATA;
                bi->nFile = file_idx;
                bi->nDataPos = (unsigned int)(pos + 8);
                marked++;
            }

            /* Skip to next block */
            pos += 8 + (long)blk_size;
            fseek(f, pos, SEEK_SET);
        }

        fclose(f);
        if (marked > 0 && (file_idx % 10 == 0))
            printf("  Scanned blk%05d.dat (%d blocks marked so far)\n",
                   file_idx, marked);
    }

    return marked;
}

