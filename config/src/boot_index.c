/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Block index management: flat file save/load, SQLite cache,
 * chainstate rebuild/reindex, address backfill. */

#include "config/boot_internal.h"
#include "chain/chain.h"
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
#include "primitives/block.h"
#include "core/serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <malloc.h>
#include <sqlite3.h>

/* ZClassic mainnet block file magic (little-endian 0x6427e924) */
#define ZCL_BLOCK_MAGIC 0x6427e924

/* Max bytes to read from a block for header parsing + tx count.
 * ZClassic header = 140 fixed + ~1347 equihash solution = ~1487 bytes.
 * 1600 gives margin for the compact_size tx count after the header. */
#define BLOCK_HEADER_READ_SIZE 1600

static struct db_service *boot_index_db_service_for(struct node_db *ndb)
{
    struct db_service *dbsvc = app_runtime_db_service();

    if (!ndb || !dbsvc)
        return NULL;
    return db_service_node_db(dbsvc) == ndb ? dbsvc : NULL;
}

static bool boot_index_enter_turbo_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_ibd_turbo_mode(dbsvc);
    return node_db_ibd_turbo_mode(ndb);
}

static bool boot_index_restore_normal_mode(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_normal_mode(dbsvc);
    return node_db_normal_mode(ndb);
}

static bool boot_index_set_sync_batch_size(struct node_db *ndb, int batch_size)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_set_sync_batch_size(dbsvc, batch_size);
    node_db_set_sync_batch_size(ndb, batch_size);
    return true;
}

static bool boot_index_flush_write(struct node_db *ndb)
{
    struct db_service *dbsvc = boot_index_db_service_for(ndb);

    if (dbsvc)
        return db_service_flush_write(dbsvc);
    return node_db_sync_flush(ndb);
}

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
    if (!sorted) {
        fprintf(stderr, "save_block_index_flat: malloc failed for %zu entries\n",
                count);
        return;
    }

    size_t idx = 0, iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && idx < count) sorted[idx++] = p;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    int64_t t0 = (int64_t)time(NULL);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "save_block_index_flat: cannot create %s: %s\n",
                path, strerror(errno));
        free(sorted); return;
    }

    uint32_t magic = 0x5A434C49; /* "ZCLI" */
    if (fwrite(&magic, 4, 1, f) != 1 ||
        fwrite(&(uint32_t){(uint32_t)count}, 4, 1, f) != 1) {
        fprintf(stderr, "save_block_index_flat: header write failed\n");
        fclose(f); free(sorted); return;
    }

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
        if (fwrite(&entry, sizeof(entry), 1, f) != 1) {
            fprintf(stderr, "save_block_index_flat: write failed at entry "
                    "%zu/%zu: %s\n", i, count, strerror(errno));
            fclose(f); free(sorted); return;
        }
    }
    fflush(f);
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
    if (fd < 0) {
        fprintf(stderr, "block_index_flat: cannot open %s: %s\n",
                path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "block_index_flat: fstat failed: %s\n", strerror(errno));
        close(fd); return false;
    }
    size_t file_size = (size_t)st.st_size;
    if (file_size < 8) {
        fprintf(stderr, "block_index_flat: file too small (%zu bytes)\n", file_size);
        close(fd); return false;
    }

    uint8_t *data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "block_index_flat: mmap failed (%zu bytes): %s\n",
                file_size, strerror(errno));
        return false;
    }

    uint32_t magic, count;
    memcpy(&magic, data, 4);
    memcpy(&count, data + 4, 4);
    if (magic != 0x5A434C49) {
        fprintf(stderr, "block_index_flat: bad magic 0x%08x (expected 0x5A434C49)\n",
                magic);
        munmap(data, file_size); return false;
    }
    if (count > 10000000) {
        fprintf(stderr, "block_index_flat: count %u too large (max 10M)\n", count);
        munmap(data, file_size); return false;
    }

    size_t expected = 8 + (size_t)count * sizeof(struct block_index_flat);
    if (file_size < expected) {
        fprintf(stderr, "block_index_flat: truncated — %zu bytes < %zu expected "
                "(%u entries)\n", file_size, expected, count);
        munmap(data, file_size); return false;
    }

    int64_t t0 = (int64_t)time(NULL);
    const struct block_index_flat *entries =
        (const struct block_index_flat *)(data + 8);

    /* Pre-size hash map + arena. Pre-fault memory. */
    block_map_reserve(&ms->map_block_index, count);
    struct block_index *arena = calloc(count, sizeof(struct block_index));
    if (!arena) {
        fprintf(stderr, "block_index_flat: calloc failed for %u entries "
                "(%zu bytes)\n", count, (size_t)count * sizeof(struct block_index));
        munmap(data, file_size); return false;
    }
    memset(arena, 0, count * sizeof(struct block_index)); /* pre-fault */

    /* Height→index for O(1) pprev linking */
    int32_t max_h = entries[count - 1].height;
    struct block_index **by_height = NULL;
    if (max_h > 0 && max_h < 10000000) {
        by_height = calloc((size_t)(max_h + 1), sizeof(struct block_index *));
        if (!by_height)
            fprintf(stderr, "block_index_flat: by_height calloc failed "
                    "(%d entries) — pprev linking will be slow\n", max_h + 1);
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

    /* Phase 2: Link pprev via prev_hash lookup (handles orphans correctly).
     * Height-based linking breaks when orphan blocks at the same height
     * overwrite the main chain entry in by_height[], causing the pprev
     * chain to follow orphan forks instead of the main chain. */
    for (uint32_t i = 0; i < count; i++) {
        struct block_index *pindex = &arena[i];
        if (pindex->nHeight > 0) {
            /* Look up prev_hash in block_map */
            struct uint256 prev;
            memcpy(prev.data, entries[i].prev_hash, 32);
            struct block_index *pp = block_map_find(bm, &prev);
            if (pp)
                pindex->pprev = pp;
            else if (by_height && pindex->nHeight - 1 >= 0 &&
                     pindex->nHeight - 1 <= max_h) {
                pindex->pprev = by_height[pindex->nHeight - 1]; /* fallback */
                printf("WARNING: pprev for height %d resolved via height fallback "
                       "(prev_hash not found in block_map)\n", pindex->nHeight);
            }
        }
    }
    free(by_height);

    /* Recompute nChainWork and nChainTx from pprev chain.
     * The flat file may have stale values for blocks that were connected
     * via P2P after the LevelDB post-load but before the file was saved.
     * These blocks can have truncated chain_work (only low 32 bits set)
     * because connect_tip computed them with a pprev that had wrong state.
     * Recomputing from the sorted array with correct pprev fixes this. */
    {
        int fixed_work = 0, fixed_tx = 0;
        for (uint32_t i = 0; i < count; i++) {
            struct block_index *pi = &arena[i];
            if (!pi->pprev) continue;

            /* Recompute chain_work from pprev */
            struct arith_uint256 proof = GetBlockProof(pi);
            struct arith_uint256 expected;
            arith_uint256_add(&expected, &pi->pprev->nChainWork, &proof);
            if (arith_uint256_compare(&expected, &pi->nChainWork) != 0) {
                pi->nChainWork = expected;
                fixed_work++;
            }

            /* Fix nChainTx */
            if (pi->nTx > 0 && pi->pprev->nChainTx > 0) {
                uint32_t expected_ctx = pi->pprev->nChainTx + pi->nTx;
                if (pi->nChainTx != expected_ctx) {
                    pi->nChainTx = expected_ctx;
                    fixed_tx++;
                }
            }
        }
        if (fixed_work > 0 || fixed_tx > 0)
            printf("Block index flat: fixed %d chain_work, %d chain_tx\n",
                   fixed_work, fixed_tx);
    }

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
    bool tx_open = false;
    int exec_rc = sqlite3_exec(ndb->db, "DELETE FROM block_index_cache",
                               NULL, NULL, NULL);
    if (exec_rc != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to clear block_index_cache: %s\n",
                sqlite3_errmsg(ndb->db));
        return;
    }
    exec_rc = sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
    if (exec_rc != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to begin block_index_cache save: %s\n",
                sqlite3_errmsg(ndb->db));
        return;
    }
    tx_open = true;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO block_index_cache "
        "(hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
        "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
        "n_cached_branch_id,n_chain_tx) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        fprintf(stderr, "boot-index: failed to prepare block_index_cache insert: %s\n",
                sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    static const unsigned char zero32[32] = {0};
    size_t iter = 0, count = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || !p->phashBlock) continue;

        sqlite3_reset(ins);
        if (sqlite3_bind_blob(ins, 1, p->phashBlock->data, 32, SQLITE_STATIC) != SQLITE_OK)
            goto fail;
        const unsigned char *prev = (p->pprev && p->pprev->phashBlock)
            ? p->pprev->phashBlock->data : zero32;
        if (sqlite3_bind_blob(ins, 2, prev, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 3, p->nHeight) != SQLITE_OK ||
            sqlite3_bind_int(ins, 4, (int)p->nBits) != SQLITE_OK ||
            sqlite3_bind_int(ins, 5, (int)p->nTime) != SQLITE_OK ||
            sqlite3_bind_int(ins, 6, p->nVersion) != SQLITE_OK ||
            sqlite3_bind_int(ins, 7, (int)p->nStatus) != SQLITE_OK ||
            sqlite3_bind_int(ins, 8, p->nFile) != SQLITE_OK ||
            sqlite3_bind_int(ins, 9, (int)p->nDataPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 10, (int)p->nUndoPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 11, (int)p->nTx) != SQLITE_OK ||
            sqlite3_bind_blob(ins, 12, p->nChainWork.pn, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 13, (int)p->nCachedBranchId) != SQLITE_OK ||
            sqlite3_bind_int(ins, 14, (int)p->nChainTx) != SQLITE_OK ||
            sqlite3_step(ins) != SQLITE_DONE)
            goto fail;
        count++;

        if (count % 50000 == 0) {
            if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = false;
            if (sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = true;
        }
    }
    sqlite3_finalize(ins);
    ins = NULL;
    if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to commit block_index_cache save: %s\n",
                sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("Block index: cached %zu/%zu entries in SQLite (%llds)\n",
           count, total, (long long)elapsed);
    return;

fail:
    fprintf(stderr, "boot-index: block_index_cache save aborted: %s\n",
            sqlite3_errmsg(ndb->db));
    if (ins)
        sqlite3_finalize(ins);
    if (tx_open)
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
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
    if (sqlite3_prepare_v2(cvs->db, "SELECT count(*) FROM utxos",
                           -1, &cnt, NULL) != SQLITE_OK || !cnt) {
        fprintf(stderr, "fast_rebuild_chainstate: count prepare failed: %s\n",
                sqlite3_errmsg(cvs->db));
        return false;
    }
    int64_t total = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW)
        total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    if (total == 0) return false;

    printf("SQLite UTXO set: %lld UTXOs (canonical)\n", (long long)total);

    struct uint256 best;
    if (!coins_view_sqlite_get_best_block(cvs, &best) || uint256_is_null(&best)) {
        sqlite3_stmt *th = NULL;
        if (sqlite3_prepare_v2(cvs->db,
            "SELECT value FROM node_state WHERE key='tip_hash'",
            -1, &th, NULL) != SQLITE_OK || !th) {
            fprintf(stderr, "fast_rebuild_chainstate: tip_hash prepare failed: %s\n",
                    sqlite3_errmsg(cvs->db));
            return false;
        }
        int th_rc = sqlite3_step(th);
        if (th_rc == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(th, 0);
            if (h && sqlite3_column_bytes(th, 0) >= 32) {
                memcpy(best.data, h, 32);
                sqlite3_stmt *set = NULL;
                if (sqlite3_prepare_v2(cvs->db,
                    "INSERT OR REPLACE INTO node_state(key,value)"
                    " VALUES('coins_best_block',?)", -1, &set, NULL) != SQLITE_OK || !set) {
                    sqlite3_finalize(th);
                    fprintf(stderr, "fast_rebuild_chainstate: coins_best_block prepare failed: %s\n",
                            sqlite3_errmsg(cvs->db));
                    return false;
                }
                if (sqlite3_bind_blob(set, 1, best.data, 32, SQLITE_STATIC) != SQLITE_OK ||
                    sqlite3_step(set) != SQLITE_DONE) {
                    sqlite3_finalize(set);
                    sqlite3_finalize(th);
                    fprintf(stderr, "fast_rebuild_chainstate: coins_best_block write failed: %s\n",
                            sqlite3_errmsg(cvs->db));
                    return false;
                }
                sqlite3_finalize(set);
            }
        } else if (th_rc != SQLITE_DONE) {
            sqlite3_finalize(th);
            fprintf(stderr, "fast_rebuild_chainstate: tip_hash read failed: %s\n",
                    sqlite3_errmsg(cvs->db));
            return false;
        }
        sqlite3_finalize(th);
        if (uint256_is_null(&best))
            return false;
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
        if (!boot_index_enter_turbo_mode(ndb))
            fprintf(stderr, "reindex-chainstate: failed to enter turbo mode\n");
        if (!boot_index_set_sync_batch_size(ndb, 1000))
            fprintf(stderr, "reindex-chainstate: failed to set sync batch size\n");
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
            break;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, datadir)) {
            fprintf(stderr, "reindex-chainstate: failed to read block at "
                    "height %d — stopping to prevent UTXO corruption\n", h);
            errors++;
            break; /* Can't skip blocks during UTXO replay */
        }

        struct validation_state state;
        validation_state_init(&state);
        if (!connect_block(&blk, &state, pindex, cvtip, cparams, false)) {
            fprintf(stderr, "reindex-chainstate: connect_block FATAL at "
                    "height %d: %s — stopping to prevent UTXO corruption\n",
                    h, state.reject_reason);
            block_free(&blk);
            errors++;
            break; /* MUST stop — continuing would skip this block's UTXOs */
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
        if (!boot_index_flush_write(ndb))
            fprintf(stderr, "reindex-chainstate: flush failed\n");
        if (!boot_index_restore_normal_mode(ndb))
            fprintf(stderr, "reindex-chainstate: failed to restore normal mode\n");
        if (!boot_index_set_sync_batch_size(ndb, 1))
            fprintf(stderr, "reindex-chainstate: failed to reset sync batch size\n");
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
/* Scan block files on disk, parse proper ZClassic headers (with
 * equihash solution), create block_index entries if missing, set
 * nTx, mark BLOCK_HAVE_DATA, and propagate nChainTx so
 * find_most_work_chain can find the best tip.
 *
 * This is the critical bridge between file_service (downloads
 * block files) and activate_best_chain (needs BLOCK_HAVE_DATA +
 * nChainTx > 0 to connect blocks). Without this, downloaded
 * blocks sit unused on disk while P2P re-downloads them. */

/* Helper: create a block_index entry directly from a parsed header.
 * Skips PoW/equihash validation — blocks from disk are trusted
 * and verified later via SHA3 UTXO checkpoint. This is 1000x
 * faster than accept_block_header (no equihash solve check). */
static struct block_index *create_block_index_fast(
    struct main_state *ms, const struct block_header *hdr,
    const struct uint256 *hash)
{
    struct block_index *pindex = calloc(1, sizeof(struct block_index));
    if (!pindex) return NULL;
    block_index_init(pindex);

    pindex->nVersion = hdr->nVersion;
    pindex->hashMerkleRoot = hdr->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = hdr->hashFinalSaplingRoot;
    pindex->nTime = hdr->nTime;
    pindex->nBits = hdr->nBits;
    pindex->nNonce = hdr->nNonce;
    uint32_t sol_copy = hdr->nSolutionSize;
    if (sol_copy > MAX_SOLUTION_SIZE) sol_copy = MAX_SOLUTION_SIZE;
    memcpy(pindex->nSolution, hdr->nSolution, sol_copy);
    pindex->nSolutionSize = sol_copy;

    if (!block_map_insert(&ms->map_block_index, hash, pindex)) {
        free(pindex);
        return block_map_find(&ms->map_block_index, hash);
    }

    /* phashBlock must point to stable storage inside the block map */
    struct block_index *found = block_map_find(&ms->map_block_index, hash);
    if (found) {
        const struct uint256 *stored = block_map_find_hash(
            &ms->map_block_index, hash);
        if (stored) found->phashBlock = stored;
    }

    /* Link to previous block */
    struct block_index *pprev = block_map_find(
        &ms->map_block_index, &hdr->hashPrevBlock);
    if (pprev) {
        pindex->pprev = pprev;
        pindex->nHeight = pprev->nHeight + 1;
        block_index_build_skip(pindex);
        struct arith_uint256 proof = GetBlockProof(pindex);
        arith_uint256_add(&pindex->nChainWork,
                          &pprev->nChainWork, &proof);
    } else {
        /* Genesis or orphan — height determined on retry pass */
        pindex->nHeight = 0;
        pindex->nChainWork = GetBlockProof(pindex);
    }

    pindex->nStatus = BLOCK_VALID_TREE;
    return pindex;
}

/* Helper: scan one block file, return number of blocks marked.
 * If params != NULL, creates block_index entries for unknown blocks
 * using fast path (no equihash verification — SHA3 checkpoint
 * validates the entire chain later). */
static int scan_one_block_file(struct main_state *ms,
                                const char *filepath, int file_idx,
                                const struct chain_params *params,
                                int *created_out)
{
    int marked = 0, created = 0, consec_errors = 0;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "scan: cannot open %s: %s\n", filepath, strerror(errno));
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    long pos = 0;
    while (pos + 8 + 140 <= file_size) {
        /* Block file format: [4-byte magic][4-byte size][block data] */
        uint8_t frame[8];
        if (fread(frame, 1, 8, f) != 8) break;

        uint32_t magic = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
                         ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24);
        uint32_t blk_size = (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) |
                            ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 24);

        /* Validate magic (mainnet: 24e92764) */
        if (magic != ZCL_BLOCK_MAGIC) {
            pos += 1;
            fseek(f, pos, SEEK_SET);
            continue;
        }

        if (blk_size < 140 || blk_size > 2000000 ||
            pos + 8 + (long)blk_size > file_size) {
            pos += 8;
            fseek(f, pos, SEEK_SET);
            continue;
        }

        /* Read enough for header + tx count. ZClassic header = ~1487 bytes
         * (140 fixed + compact_size + 1344 equihash solution).
         * Read 1600 bytes to also capture the tx count compact_size. */
        size_t read_sz = (blk_size < BLOCK_HEADER_READ_SIZE) ? blk_size : BLOCK_HEADER_READ_SIZE;
        uint8_t buf[BLOCK_HEADER_READ_SIZE];
        if (fread(buf, 1, read_sz, f) != read_sz) break;

        /* Parse the block header using proper deserializer */
        struct block_header bhdr;
        block_header_init(&bhdr);
        struct byte_stream bs;
        stream_init_from_data(&bs, buf, read_sz);
        if (!block_header_deserialize(&bhdr, &bs)) {
            /* Corrupt or truncated header — skip this block */
            consec_errors++;
            if (consec_errors > 20) {
                fprintf(stderr, "scan: %d consecutive corrupt blocks in "
                        "blk%05d.dat at pos %ld — aborting file\n",
                        consec_errors, file_idx, pos);
                break;
            }
            pos += 8 + (long)blk_size;
            fseek(f, pos, SEEK_SET);
            continue;
        }
        consec_errors = 0; /* reset on success */

        /* Read tx count (compact size right after header) */
        uint64_t num_tx = 0;
        if (!stream_read_compact_size(&bs, &num_tx) || num_tx == 0)
            num_tx = 1; /* minimum: at least coinbase tx */
        if (num_tx > 100000) {
            fprintf(stderr, "scan: suspicious num_tx=%llu at file %d pos %ld, "
                    "clamping to 1\n", (unsigned long long)num_tx,
                    file_idx, pos);
            num_tx = 1;
        }

        /* Compute proper block hash (SHA256d of full serialized header) */
        struct uint256 hash;
        block_header_get_hash(&bhdr, &hash);

        /* Look up or create block_index entry */
        struct block_index *bi = block_map_find(&ms->map_block_index, &hash);

        if (!bi && params) {
            /* Block not in index — create directly (no equihash check).
             * Blocks from disk are trusted; SHA3 UTXO checkpoint at
             * height 3,056,758 validates the entire chain integrity.
             * This is ~1000x faster than accept_block_header. */
            bi = create_block_index_fast(ms, &bhdr, &hash);
            if (bi) created++;
        }

        if (bi) {
            /* Fix missing pprev link (block was created out of order
             * in a previous pass — its pprev is now in the map) */
            if (!bi->pprev && bi->nHeight == 0 && params) {
                struct block_index *pprev = block_map_find(
                    &ms->map_block_index, &bhdr.hashPrevBlock);
                if (pprev) {
                    bi->pprev = pprev;
                    bi->nHeight = pprev->nHeight + 1;
                    block_index_build_skip(bi);
                    struct arith_uint256 proof = GetBlockProof(bi);
                    arith_uint256_add(&bi->nChainWork,
                                      &pprev->nChainWork, &proof);
                }
            }

            if (!(bi->nStatus & BLOCK_HAVE_DATA)) {
                bi->nStatus |= BLOCK_HAVE_DATA;
                bi->nStatus = (bi->nStatus & ~(unsigned)BLOCK_VALID_MASK) |
                               BLOCK_VALID_TRANSACTIONS;
                bi->nFile = file_idx;
                bi->nDataPos = (unsigned int)(pos + 8);
                if (bi->nTx == 0)
                    bi->nTx = (unsigned int)num_tx;
                marked++;
            } else if (bi->nTx == 0) {
                bi->nTx = (unsigned int)num_tx;
            }
        }

        pos += 8 + (long)blk_size;
        fseek(f, pos, SEEK_SET);
    }

    fclose(f);
    if (created_out) *created_out += created;
    return marked;
}

/* (constants defined at top of file) */

int scan_block_files_mark_data(struct main_state *ms, const char *datadir,
                                const struct chain_params *params)
{
    if (!ms || !datadir) {
        fprintf(stderr, "scan_block_files_mark_data: NULL argument\n");
        return 0;
    }

    int marked = 0, created = 0;
    char path[576];
    int64_t t0 = (int64_t)time(NULL);

    /* Pass 1: scan all block files.
     * Don't break on first gap — blk00000.dat may be empty (0 bytes)
     * while blk00001.dat+ have data. Stop after 3 consecutive misses. */
    int consecutive_misses = 0;
    for (int file_idx = 0; file_idx < 256; file_idx++) {
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, file_idx);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size == 0) {
            if (++consecutive_misses >= 3) break;
            continue;
        }
        consecutive_misses = 0;

        int m = scan_one_block_file(ms, path, file_idx, params, &created);
        marked += m;

        if ((file_idx % 20 == 0) && (marked > 0 || created > 0))
            printf("  blk%05d.dat: %d marked, %d created so far\n",
                   file_idx, marked, created);
    }

    /* Also scan blk_sync.dat when it exists.
     * File-service bootstrap does not always create it, so missing
     * sync spool should not look like a scan failure. */
    snprintf(path, sizeof(path), "%s/blocks/blk_sync.dat", datadir);
    struct stat sync_st;
    if (stat(path, &sync_st) == 0 && sync_st.st_size > 0)
        marked += scan_one_block_file(ms, path, 255, params, &created);

    /* Pass 2: retry for out-of-order blocks (prevblock now in map).
     * Block files from zclassicd are 99%+ in order, so pass 1 catches
     * nearly everything. Pass 2 picks up stragglers. */
    if (created > 0 && params) {
        for (int retry = 0; retry < 3; retry++) {
            int prev_marked = marked;
            int rmiss = 0;
            for (int file_idx = 0; file_idx < 256; file_idx++) {
                snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                         datadir, file_idx);
                struct stat st;
                if (stat(path, &st) != 0 || st.st_size == 0) {
                    if (++rmiss >= 3) break;
                    continue;
                }
                rmiss = 0;
                marked += scan_one_block_file(ms, path, file_idx, params,
                                               &created);
            }
            int delta = marked - prev_marked;
            if (delta == 0) break;
            printf("  Retry pass %d: %d additional blocks\n", retry + 1, delta);
        }
    }

    /* Propagate nChainTx along the chain. This is REQUIRED for
     * find_most_work_chain to consider these blocks as candidates.
     * Collect all blocks with BLOCK_HAVE_DATA, sort by height,
     * compute nChainTx = pprev->nChainTx + nTx.
     * Multiple passes handle gaps (e.g., retry-created blocks
     * whose pprev was missing in earlier passes). */
    if (marked > 0) {
        size_t total = ms->map_block_index.size;
        struct block_index **sorted = malloc(total * sizeof(struct block_index *));
        if (sorted) {
            size_t n = 0, iter = 0;
            struct block_index *bi;
            while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
                if (bi && (bi->nStatus & BLOCK_HAVE_DATA) && bi->nTx > 0)
                    sorted[n++] = bi;
            }

            qsort(sorted, n, sizeof(struct block_index *), block_index_cmp_height);

            int total_propagated = 0;
            for (int pass = 0; pass < 5; pass++) {
                int propagated = 0;
                for (size_t i = 0; i < n; i++) {
                    struct block_index *b = sorted[i];
                    if (b->nHeight == 0) {
                        if (b->nChainTx == 0) {
                            b->nChainTx = b->nTx;
                            propagated++;
                        }
                    } else if (b->pprev && b->pprev->nChainTx > 0) {
                        unsigned int expected = b->pprev->nChainTx + b->nTx;
                        if (b->nChainTx != expected) {
                            b->nChainTx = expected;
                            propagated++;
                        }
                    }
                }
                total_propagated += propagated;
                if (propagated == 0) break;
                if (pass == 4)
                    fprintf(stderr, "WARNING: nChainTx did not converge in "
                            "5 passes (%d blocks still pending) — possible "
                            "gap in block chain\n", propagated);
            }

            /* Count blocks with HAVE_DATA but no nChainTx — these are
             * unreachable from genesis (orphans or broken pprev links) */
            int orphans = 0;
            for (size_t i = 0; i < n; i++)
                if (sorted[i]->nChainTx == 0 && sorted[i]->nHeight > 0)
                    orphans++;
            free(sorted);
            if (total_propagated > 0)
                printf("  nChainTx propagated for %d blocks",
                       total_propagated);
            if (orphans > 0)
                printf(" (%d orphan blocks without chain)", orphans);
            if (total_propagated > 0 || orphans > 0)
                printf("\n");
        }
    }

    int64_t elapsed = (int64_t)time(NULL) - t0;
    if (marked > 0 || created > 0)
        printf("Block file scan: %d blocks marked, %d created in %llds\n",
               marked, created, (long long)elapsed);

    return marked;
}

/* ── Boot-time Validation ───────────────────────────────────────
 *
 * ActiveRecord-style validation for coins/chain agreement.
 * Called once at boot after block index load + chain restore.
 *
 * Validates: coins_best_block must match active chain tip hash.
 * Returns: recovery action enum + diagnostic info.
 * Emits: EV_BOOT_VALIDATION_FAILED on mismatch. */

struct boot_validation_result validate_coins_chain_agreement(
    struct main_state *ms,
    struct coins_view_cache *cvtip,
    const char *datadir)
{
    struct boot_validation_result r = {0};

    struct block_index *chain_tip = active_chain_tip(&ms->chain_active);
    struct uint256 coins_best;
    coins_view_cache_get_best_block(cvtip, &coins_best);

    r.chain_height = chain_tip ? chain_tip->nHeight : 0;
    memcpy(&r.coins_hash, &coins_best, sizeof(r.coins_hash));
    r.coins_height = -1;

    /* Case 1: Chain at genesis or empty — nothing to validate */
    if (!chain_tip || chain_tip->nHeight <= 0) {
        r.action = BOOT_OK;
        return r;
    }

    /* Case 2: Coins DB empty but chain has blocks */
    if (uint256_is_null(&coins_best)) {
        /* Check if LevelDB chainstate exists for reimport */
        char cs_path[1024];
        snprintf(cs_path, sizeof(cs_path), "%s/chainstate", datadir);
        struct stat st;
        if (stat(cs_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            r.action = BOOT_RECOVER_REIMPORT;
        } else {
            r.action = BOOT_RECOVER_WIPE_WAIT;
        }
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "coins_empty chain_h=%d action=%s",
            r.chain_height,
            r.action == BOOT_RECOVER_REIMPORT ? "reimport" : "wipe_wait");
        return r;
    }

    /* Case 3: Coins and chain agree — all good */
    if (chain_tip->phashBlock &&
        uint256_cmp(chain_tip->phashBlock, &coins_best) == 0) {
        r.action = BOOT_OK;
        r.coins_height = r.chain_height;
        return r;
    }

    /* Case 4: Coins and chain disagree — find coins block in index */
    struct block_index *coins_block = block_map_find(
        &ms->map_block_index, &coins_best);

    if (coins_block) {
        r.coins_height = coins_block->nHeight;
        if (coins_block->nHeight <= chain_tip->nHeight) {
            /* Coins behind chain — reset chain to coins tip */
            r.action = BOOT_RECOVER_RESET_CHAIN;
        } else {
            /* Coins ahead of chain — unusual, wipe and resync */
            r.action = BOOT_RECOVER_WIPE_WAIT;
        }
    } else {
        /* Coins best block not in our index — unknown state */
        r.action = BOOT_RECOVER_WIPE_WAIT;
    }

    event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
        "coins_chain_mismatch chain_h=%d coins_h=%d action=%s",
        r.chain_height, r.coins_height,
        r.action == BOOT_RECOVER_RESET_CHAIN ? "reset_chain" :
        r.action == BOOT_RECOVER_REIMPORT ? "reimport" : "wipe_wait");

    return r;
}
