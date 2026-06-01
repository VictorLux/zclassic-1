/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Block index management: chainstate rebuild/reindex, address backfill,
 * block file scanning.
 *
 * Block index flat file save/load and SQLite cache functions have been
 * extracted to app/services/src/block_index_loader.c (Phase A). */

#include "platform/time_compat.h"
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
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"
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
#include <pthread.h>
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

/* Block index flat file save/load, SQLite cache, and LevelDB loading
 * have been extracted to app/services/src/block_index_loader.c.
 * See services/block_index_loader.h for the public API. */


/* ── Fast chainstate rebuild from SQLite UTXOs ─────────────── */

bool fast_rebuild_chainstate(struct coins_view_sqlite *cvs,
                              struct coins_view_cache *cvtip,
                              const char *datadir)
{
    (void)cvtip;
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
    if (!coins_view_sqlite_get_best_block(cvs, &best) ||
        uint256_is_null(&best)) {
        fprintf(stderr,
                "fast_rebuild_chainstate: coins_best_block missing; "
                "refusing legacy tip_hash fallback\n");
        return false;
    }

    return true;
}

/* ── Full chainstate reindex: replay all blocks ────────────── */

static bool boot_index_flush_reindex_coins(struct coins_view_sqlite *cvs,
                                           struct coins_view_cache *cvtip)
{
    if (!cvs || !cvtip)
        LOG_FAIL("boot_index",
                 "reindex coins flush: NULL arg cvs=%p cvtip=%p",
                 (void *)cvs, (void *)cvtip);

    bool ok = coins_view_sqlite_batch_write_ex( // one-write-path-ok:boot-reindex-single-writer
        cvs, &cvtip->cache_coins, &cvtip->hash_block, &cvtip->commitment);
    if (!ok) {
        fprintf(stderr, // obs-ok:helper-context-logged
                "reindex-chainstate: coins flush failed; retaining %zu "
                "dirty entries\n",
                cvtip->cache_coins.size);
        return false;
    }

    coins_map_free(&cvtip->cache_coins);
    coins_map_init(&cvtip->cache_coins);
    utxo_commitment_init(&cvtip->commitment);
    return true;
}

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

    if (!boot_index_flush_reindex_coins(cvs, cvtip))
        return false;
    coins_view_cache_free(cvtip);

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

    set_flush_policy(3600, 1000000, 500);
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
    int64_t t_start = (int64_t)platform_time_wall_time_t();
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
            if (!boot_index_flush_reindex_coins(cvs, cvtip)) {
                errors++;
                break;
            }
            malloc_trim(0);
            if (h % 1000 == 0) {
                int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
                double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
                int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
                printf("  height %d/%d (%.0f blk/s, ETA %dm%ds, cache %zu)\n",
                       h, tip_height, rate, eta / 60, eta % 60,
                       cvtip->cache_coins.size);
            }
        }
    }

    if (!boot_index_flush_reindex_coins(cvs, cvtip))
        errors++;

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

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
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

    /* Disable mmap entirely for this background thread.
     * The previous mmap_size=64MB caused SIGSEGV after ~64K addresses:
     * when the main thread writes via WAL, the kernel may invalidate
     * mmap pages that this thread's sort cursor is scanning, triggering
     * a fault in SQLite's mmap read path. With mmap_size=0, SQLite
     * falls back to read() which is safe under concurrent WAL writes.
     * Performance is irrelevant — this is a one-time background job. */
    sqlite3_exec(db, "PRAGMA mmap_size=0", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 60000);
    /* Reduce temp store pressure — force temp tables to disk */
    sqlite3_exec(db, "PRAGMA temp_store=FILE", NULL, NULL, NULL);
    /* Use a modest cache to avoid memory bloat during aggregation */
    sqlite3_exec(db, "PRAGMA cache_size=-32768", NULL, NULL, NULL); /* 32MB */

    printf("Address backfill: aggregating UTXOs...\n");
    fflush(stdout);
    int64_t t0 = (int64_t)platform_time_wall_time_t();

    /* Ensure addresses table exists (it should, but be safe) */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS addresses ("
        "address_hash BLOB PRIMARY KEY,"
        "script_type INTEGER NOT NULL DEFAULT 0,"
        "balance INTEGER NOT NULL DEFAULT 0,"
        "utxo_count INTEGER NOT NULL DEFAULT 0,"
        "first_seen_height INTEGER NOT NULL DEFAULT 0,"
        "last_seen_height INTEGER NOT NULL DEFAULT 0"
        ")", NULL, NULL, NULL);

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_utxo_address"
        " ON utxos(address_hash) WHERE address_hash IS NOT NULL",
        NULL, NULL, NULL);

    /* Process in batches using a cursor over distinct address_hash values.
     * The old single-query approach (INSERT SELECT GROUP BY over 1.3M UTXOs)
     * caused SIGSEGV after ~64K addresses due to SQLite sort buffer / mmap
     * memory pressure. Batching keeps peak memory bounded. */
    int rc;
    int64_t processed = 0;
    static const int BATCH_SIZE = 10000;

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    sqlite3_stmt *cursor = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT DISTINCT address_hash FROM utxos "
        "WHERE address_hash IS NOT NULL "
        "ORDER BY address_hash",
        -1, &cursor, NULL);
    if (rc != SQLITE_OK || !cursor) {
        fprintf(stderr, "Address backfill: failed to prepare cursor: %s\n",
                sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_stmt *upsert = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash = ?1",
        -1, &upsert, NULL);
    if (rc != SQLITE_OK || !upsert) {
        fprintf(stderr, "Address backfill: failed to prepare upsert: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(cursor);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(db);
        return NULL;
    }

    while ((rc = sqlite3_step(cursor)) == SQLITE_ROW) {
        const void *addr_hash = sqlite3_column_blob(cursor, 0);
        int addr_len = sqlite3_column_bytes(cursor, 0);
        if (!addr_hash || addr_len <= 0)
            continue;

        sqlite3_reset(upsert);
        sqlite3_bind_blob(upsert, 1, addr_hash, addr_len, SQLITE_STATIC);
        sqlite3_step(upsert);
        processed++;

        /* Commit every BATCH_SIZE rows to release locks and memory */
        if (processed % BATCH_SIZE == 0) {
            sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
            sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
            if (processed % 100000 == 0) {
                printf("Address backfill: %lld addresses processed...\n",
                       (long long)processed);
                fflush(stdout);
            }
        }
    }

    sqlite3_finalize(cursor);
    sqlite3_finalize(upsert);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;

    sqlite3_exec(db,
        "INSERT OR REPLACE INTO node_state(key,value) "
        "VALUES('addresses_backfilled', X'01')", NULL, NULL, NULL);

    sqlite3_close(db);
    printf("Address backfill: %lld addresses in %llds\n",
           (long long)processed, (long long)elapsed);
    fflush(stdout);
    return NULL;
}

/* ── scan_block_files_mark_data ──────────────────────────────── */
/* Scan block files on disk, parse proper ZClassic headers (with
 * equihash solution), create block_index entries if missing, set
 * nTx, mark BLOCK_HAVE_DATA, and propagate nChainTx so
 * find_most_work_chain can find the best tip.
 *
 * This is the critical bridge between file_service (downloads block files)
 * and reducer activation (needs BLOCK_HAVE_DATA + nChainTx > 0 to connect
 * blocks). Without this, downloaded blocks sit unused on disk while P2P
 * re-downloads them. */

    /* Helper: create a block_index entry directly from a parsed header.
     * Skips PoW/equihash validation here; local disk blocks are checked
     * later against the SHA3 UTXO checkpoint. This is 1000x faster than
     * accept_block_header (no equihash solve check). */
struct boot_scan_block_meta {
    struct uint256 hash;
    struct uint256 hashPrevBlock;
    struct uint256 hashMerkleRoot;
    struct uint256 hashFinalSaplingRoot;
    struct uint256 nNonce;
    int32_t nVersion;
    uint32_t nTime;
    uint32_t nBits;
    unsigned int nTx;
    unsigned int nDataPos;
};

struct boot_scan_file_result {
    char path[576];
    int file_idx;
    long file_size;
    struct boot_scan_block_meta *blocks;
    size_t count;
    size_t cap;
    int skipped;
    int corrupt;
    bool ok;
};

struct boot_scan_apply_counts {
    int marked;
    int created;
    int header_fixed;
};

static struct block_index *create_block_index_fast(
    struct main_state *ms, const struct boot_scan_block_meta *meta)
{
    struct block_index *pindex = zcl_calloc(1, sizeof(struct block_index), "boot.index.block_index");
    if (!pindex) return NULL;
    block_index_init(pindex);

    pindex->nVersion = meta->nVersion;
    pindex->hashMerkleRoot = meta->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = meta->hashFinalSaplingRoot;
    pindex->nTime = meta->nTime;
    pindex->nBits = meta->nBits;
    pindex->nNonce = meta->nNonce;
    /* Don't store solution in block_index — saves 1.3KB per entry
     * (4GB total for 3M entries). Read from disk when needed. */
    pindex->nSolution = NULL;
    pindex->nSolutionSize = 0;

    if (!block_map_insert(&ms->map_block_index, &meta->hash, pindex)) {
        free(pindex);
        return block_map_find(&ms->map_block_index, &meta->hash);
    }

    /* phashBlock must point to stable storage inside the block map */
    struct block_index *found = block_map_find(&ms->map_block_index,
                                               &meta->hash);
    if (found) {
        const struct uint256 *stored = block_map_find_hash(
            &ms->map_block_index, &meta->hash);
        if (stored) found->phashBlock = stored;
    }

    /* Link to previous block */
    struct block_index *pprev = block_map_find(
        &ms->map_block_index, &meta->hashPrevBlock);
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

struct boot_index_recompute_entry {
    struct block_index *bi;
    unsigned char state; /* 0 unknown, 1 visiting, 2 fixed, 3 unreachable */
};

static int boot_index_cmp_entry_ptr(const void *a, const void *b)
{
    const struct boot_index_recompute_entry *ea = a;
    const struct boot_index_recompute_entry *eb = b;
    if (ea->bi < eb->bi) return -1;
    if (ea->bi > eb->bi) return 1;
    return 0;
}

static struct boot_index_recompute_entry *boot_index_find_entry(
    struct boot_index_recompute_entry *entries, size_t n,
    const struct block_index *bi)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].bi < bi)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < n && entries[lo].bi == bi)
        return &entries[lo];
    return NULL;
}

/* Recompute every reachable block's height and cumulative metadata from the
 * genesis pprev chain.  This deliberately ignores existing height labels:
 * after stale flat-cache or SQLite recovery a block can have a locally
 * consistent but globally wrong height, which keeps header sync stuck. */
static int recompute_index_from_genesis(struct main_state *ms,
                                        const struct chain_params *params)
{
    if (!ms || !params || ms->map_block_index.size == 0)
        return 0;

    size_t cap = ms->map_block_index.size;
    struct boot_index_recompute_entry *entries =
        zcl_calloc(cap, sizeof(*entries), "boot.index.recompute_entries");
    if (!entries)
        return 0;

    size_t n = 0, iter = 0;
    struct block_index *bi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && bi->phashBlock && n < cap)
            entries[n++].bi = bi;
    }
    qsort(entries, n, sizeof(*entries), boot_index_cmp_entry_ptr);

    struct boot_index_recompute_entry *genesis = NULL;
    for (size_t i = 0; i < n; i++) {
        if (uint256_eq(entries[i].bi->phashBlock,
                       &params->consensus.hashGenesisBlock)) {
            genesis = &entries[i];
            break;
        }
    }
    if (!genesis) {
        free(entries);
        return 0;
    }

    genesis->state = 2;
    genesis->bi->nHeight = 0;
    genesis->bi->nChainWork = GetBlockProof(genesis->bi);
    if (genesis->bi->nChainTx == 0)
        genesis->bi->nChainTx = genesis->bi->nTx > 0 ? genesis->bi->nTx : 1;
    block_index_build_skip(genesis->bi);

    size_t stack_cap = 4096;
    struct boot_index_recompute_entry **stack =
        zcl_malloc(stack_cap * sizeof(*stack), "boot.index.recompute_stack");
    if (!stack) {
        free(entries);
        return 0;
    }

    int heights_fixed = 0, work_fixed = 0, tx_fixed = 0;
    int reachable = 1, unresolved = 0, cycles = 0;

    for (size_t i = 0; i < n; i++) {
        struct boot_index_recompute_entry *cur = &entries[i];
        if (cur->state == 2)
            continue;

        size_t depth = 0;
        bool ok = false;
        while (cur) {
            if (cur->state == 2) {
                ok = true;
                break;
            }
            if (cur->state == 3)
                break;
            if (cur->state == 1) {
                cycles++;
                break;
            }
            cur->state = 1;
            if (depth >= stack_cap) {
                size_t new_cap = stack_cap * 2;
                struct boot_index_recompute_entry **tmp =
                    zcl_realloc(stack, new_cap * sizeof(*stack), "boot.index.recompute_stack");
                if (!tmp)
                    break;
                stack = tmp;
                stack_cap = new_cap;
            }
            stack[depth++] = cur;
            if (!cur->bi->pprev)
                break;
            cur = boot_index_find_entry(entries, n, cur->bi->pprev);
        }

        if (ok) {
            for (size_t ri = depth; ri > 0; ri--) {
                struct block_index *fix = stack[ri - 1]->bi;
                struct block_index *prev = fix->pprev;
                int expected_h = prev ? prev->nHeight + 1 : 0;
                if (fix->nHeight != expected_h) {
                    fix->nHeight = expected_h;
                    heights_fixed++;
                }
                block_index_build_skip(fix);

                struct arith_uint256 proof = GetBlockProof(fix);
                struct arith_uint256 expected_work;
                if (prev)
                    arith_uint256_add(&expected_work,
                                      &prev->nChainWork, &proof);
                else
                    expected_work = proof;
                if (arith_uint256_compare(&fix->nChainWork,
                                          &expected_work) != 0) {
                    fix->nChainWork = expected_work;
                    work_fixed++;
                }

                unsigned int ntx = fix->nTx > 0 ? fix->nTx : 1;
                unsigned int expected_tx =
                    prev ? prev->nChainTx + ntx : ntx;
                if (fix->nChainTx != expected_tx) {
                    fix->nChainTx = expected_tx;
                    tx_fixed++;
                }
                stack[ri - 1]->state = 2;
                reachable++;
            }
        } else {
            unresolved += (int)depth;
            for (size_t ri = 0; ri < depth; ri++)
                stack[ri]->state = 3;
        }
    }

    printf("  ancestry recompute: reachable=%d heights=%d chain_work=%d "
           "chain_tx=%d unresolved=%d cycles=%d\n",
           reachable, heights_fixed, work_fixed, tx_fixed,
           unresolved, cycles);

    free(stack);
    free(entries);
    return heights_fixed + work_fixed + tx_fixed;
}

static uint32_t scan_read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long scan_find_next_magic(const uint8_t *data, long start, long size)
{
    const uint8_t m0 = (uint8_t)(ZCL_BLOCK_MAGIC & 0xFF);
    const uint8_t m1 = (uint8_t)((ZCL_BLOCK_MAGIC >> 8) & 0xFF);
    const uint8_t m2 = (uint8_t)((ZCL_BLOCK_MAGIC >> 16) & 0xFF);
    const uint8_t m3 = (uint8_t)((ZCL_BLOCK_MAGIC >> 24) & 0xFF);

    for (long pos = start; pos + 8 + 140 <= size; pos++) {
        if (data[pos] == m0 && data[pos + 1] == m1 &&
            data[pos + 2] == m2 && data[pos + 3] == m3)
            return pos;
    }
    return -1;
}

static bool scan_file_append_meta(struct boot_scan_file_result *r,
                                  const struct boot_scan_block_meta *meta)
{
    if (r->count == r->cap) {
        size_t new_cap = r->cap ? r->cap * 2 : 4096;
        struct boot_scan_block_meta *tmp = zcl_realloc(
            r->blocks, new_cap * sizeof(*r->blocks),
            "boot.index.scan_file_blocks");
        if (!tmp)
            return false;
        r->blocks = tmp;
        r->cap = new_cap;
    }
    r->blocks[r->count++] = *meta;
    return true;
}

static void scan_parse_one_file(struct boot_scan_file_result *r)
{
    r->ok = false;
    int fd = open(r->path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "scan: cannot open %s: %s\n", r->path, strerror(errno));
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        r->ok = true;
        return;
    }
    r->file_size = (long)st.st_size;

    uint8_t *data = mmap(NULL, (size_t)st.st_size, PROT_READ,
                         MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "scan: mmap failed for %s: %s\n",
                r->path, strerror(errno));
        return;
    }

    bool complete = true;
    int consec_errors = 0;
    long pos = 0;
    while (pos + 8 + 140 <= r->file_size) {
        uint32_t magic = scan_read_u32_le(data + pos);
        uint32_t blk_size = scan_read_u32_le(data + pos + 4);

        if (magic != ZCL_BLOCK_MAGIC) {
            long next = scan_find_next_magic(data, pos + 1, r->file_size);
            if (next < 0)
                break;
            pos = next;
            r->skipped++;
            continue;
        }

        if (blk_size < 140 || blk_size > 2000000 ||
            pos + 8 + (long)blk_size > r->file_size) {
            pos += 8;
            continue;
        }

        size_t read_sz = (blk_size < BLOCK_HEADER_READ_SIZE)
                             ? blk_size
                             : BLOCK_HEADER_READ_SIZE;
        struct block_header bhdr;
        block_header_init(&bhdr);
        struct byte_stream bs;
        stream_init_from_data(&bs, data + pos + 8, read_sz);
        if (!block_header_deserialize(&bhdr, &bs)) {
            consec_errors++;
            r->corrupt++;
            if (consec_errors > 20) {
                fprintf(stderr, "scan: %d consecutive corrupt blocks in "
                        "blk%05d.dat at pos %ld — aborting file\n",
                        consec_errors, r->file_idx, pos);
                break;
            }
            pos += 8 + (long)blk_size;
            continue;
        }
        consec_errors = 0;

        uint64_t num_tx = 0;
        if (!stream_read_compact_size(&bs, &num_tx) || num_tx == 0)
            num_tx = 1;
        if (num_tx > 100000) {
            fprintf(stderr, "scan: suspicious num_tx=%llu at file %d pos %ld, "
                    "clamping to 1\n", (unsigned long long)num_tx,
                    r->file_idx, pos);
            num_tx = 1;
        }

        struct boot_scan_block_meta meta;
        memset(&meta, 0, sizeof(meta));
        block_header_get_hash(&bhdr, &meta.hash);
        meta.hashPrevBlock = bhdr.hashPrevBlock;
        meta.hashMerkleRoot = bhdr.hashMerkleRoot;
        meta.hashFinalSaplingRoot = bhdr.hashFinalSaplingRoot;
        meta.nNonce = bhdr.nNonce;
        meta.nVersion = bhdr.nVersion;
        meta.nTime = bhdr.nTime;
        meta.nBits = bhdr.nBits;
        meta.nTx = (unsigned int)num_tx;
        meta.nDataPos = (unsigned int)(pos + 8);
        if (!scan_file_append_meta(r, &meta)) {
            fprintf(stderr, "scan: out of memory while parsing %s at pos %ld\n",
                    r->path, pos);
            complete = false;
            break;
        }

        pos += 8 + (long)blk_size;
    }

    munmap(data, (size_t)st.st_size);
    r->ok = complete;
}

struct boot_scan_parallel_ctx {
    struct boot_scan_file_result *files;
    int nfiles;
    _Atomic int next;
};

static void *scan_parse_worker(void *arg)
{
    struct boot_scan_parallel_ctx *ctx = arg;
    for (;;) {
        int i = atomic_fetch_add(&ctx->next, 1);
        if (i >= ctx->nfiles)
            break;
        scan_parse_one_file(&ctx->files[i]);
    }
    return NULL;
}

static int scan_worker_count(int nfiles)
{
    const char *override = getenv("ZCL_BLOCK_SCAN_WORKERS");
    if (override && override[0]) {
        char *end = NULL;
        long requested = strtol(override, &end, 10);
        if (end && *end == '\0' && requested > 0) {
            if (requested > nfiles) requested = nfiles;
            if (requested > 64) requested = 64;
            if (requested < 1) requested = 1;
            return (int)requested;
        }
        fprintf(stderr, "scan: ignoring invalid ZCL_BLOCK_SCAN_WORKERS=%s\n",
                override);
    }

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int n = cpus > 0 ? (int)cpus : 1;
    if (n > nfiles) n = nfiles;
    if (n > 16) n = 16;
    if (n < 1) n = 1;
    return n;
}

static int scan_parse_files_parallel(struct boot_scan_file_result *files,
                                     int nfiles)
{
    if (nfiles <= 0)
        return 0;

    int workers = scan_worker_count(nfiles);
    printf("  parallel block-file parse: %d files, %d workers\n",
           nfiles, workers);
    if (workers == 1) {
        for (int i = 0; i < nfiles; i++)
            scan_parse_one_file(&files[i]);
        return workers;
    }

    pthread_t *threads = zcl_calloc((size_t)workers, sizeof(*threads),
                                    "boot.index.scan_threads");
    if (!threads) {
        for (int i = 0; i < nfiles; i++)
            scan_parse_one_file(&files[i]);
        return 1;
    }

    struct boot_scan_parallel_ctx ctx = {
        .files = files,
        .nfiles = nfiles,
        .next = 0,
    };
    int started = 0;
    for (int i = 0; i < workers; i++) {
        if (thread_registry_spawn_ex("zcl_blk_scan", scan_parse_worker,
                                     &ctx, &threads[started]) == 0)
            started++;
    }
    if (started == 0) {
        free(threads);
        for (int i = 0; i < nfiles; i++)
            scan_parse_one_file(&files[i]);
        return 1;
    }
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    free(threads);
    return started;
}

static struct boot_scan_apply_counts scan_apply_one_file(
    struct main_state *ms,
    const struct boot_scan_file_result *r,
    const struct chain_params *params)
{
    struct boot_scan_apply_counts counts = {0};
    for (size_t i = 0; i < r->count; i++) {
        const struct boot_scan_block_meta *meta = &r->blocks[i];
        struct block_index *bi = block_map_find(&ms->map_block_index,
                                                &meta->hash);

        if (!bi && params) {
            bi = create_block_index_fast(ms, meta);
            if (bi)
                counts.created++;
        }

        if (!bi)
            continue;

        if (bi->nVersion == 0 || bi->nTime == 0 || bi->nBits == 0) {
            bi->nVersion = meta->nVersion;
            bi->hashMerkleRoot = meta->hashMerkleRoot;
            bi->hashFinalSaplingRoot = meta->hashFinalSaplingRoot;
            bi->nTime = meta->nTime;
            bi->nBits = meta->nBits;
            bi->nNonce = meta->nNonce;
            counts.header_fixed++;
        }

        if (!bi->pprev && bi->nHeight == 0 && params) {
            struct block_index *pprev = block_map_find(
                &ms->map_block_index, &meta->hashPrevBlock);
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
            bi->nFile = r->file_idx;
            bi->nDataPos = meta->nDataPos;
            if (bi->nTx == 0)
                bi->nTx = meta->nTx;
            counts.marked++;
        } else {
            if (bi->nFile != r->file_idx ||
                bi->nDataPos != meta->nDataPos) {
                bi->nFile = r->file_idx;
                bi->nDataPos = meta->nDataPos;
            }
            if (bi->nTx == 0)
                bi->nTx = meta->nTx;
        }
    }
    return counts;
}

static void scan_free_file_results(struct boot_scan_file_result *files,
                                   int nfiles)
{
    for (int i = 0; i < nfiles; i++)
        free(files[i].blocks);
}

/* ── Post-scan pprev resolution from disk ────────────────────── */
/* After all block files are scanned and every block is in the map,
 * resolve orphan pprev links by reading hashPrevBlock from disk.
 * Then propagate heights from genesis outward.
 *
 * Why this is needed: create_block_index_fast links pprev at insert
 * time, but if the parent hasn't been inserted yet (out-of-order in
 * block files, or across file boundaries), pprev stays NULL. The
 * retry passes only fix one level deep per pass. This function
 * resolves ALL orphans in one shot since every block is now in the map. */
static int resolve_orphan_pprev_from_disk(struct main_state *ms,
                                           const char *datadir,
                                           const struct chain_params *params)
{
    if (!ms || !datadir) return 0;

    const struct uint256 *genesis = &params->consensus.hashGenesisBlock;
    int resolved = 0, read_errors = 0;

    /* Phase 1: read hashPrevBlock from disk for orphans, link pprev */
    /* Group reads by file to avoid open/close churn */
    for (int file_idx = 0; file_idx < 256; file_idx++) {
        char path[576];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                 datadir, file_idx);
        FILE *f = NULL;

        disk_block_io_lock();
        size_t iter = 0;
        struct block_index *bi;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi || bi->pprev) continue;
            if (bi->nFile != file_idx) continue;
            if (bi->nDataPos == 0) continue;
            /* Skip genesis */
            if (bi->phashBlock && uint256_eq(bi->phashBlock, genesis))
                continue;

            if (!f) {
                f = fopen(path, "rb");
                if (!f) break;
            }

            /* hashPrevBlock is at offset 4 in serialized header
             * (after int32_t nVersion). nDataPos points to start
             * of block data (past the 8-byte frame header). */
            if (fseek(f, (long)bi->nDataPos + 4, SEEK_SET) != 0) {
                read_errors++;
                continue;
            }
            struct uint256 prev_hash;
            if (fread(prev_hash.data, 1, 32, f) != 32) { // disk-io-lock: held
                read_errors++;
                continue;
            }

            struct block_index *pprev = block_map_find(
                &ms->map_block_index, &prev_hash);
            if (pprev) {
                bi->pprev = pprev;
                resolved++;
            }
        }
        if (f) fclose(f);
        disk_block_io_unlock();
    }

    if (read_errors > 0)
        fprintf(stderr, "resolve_orphan_pprev: %d disk read errors\n",
                read_errors);

    /* Phase 2: propagate heights from pprev chains.
     *
     * Old approach used 40 fixed passes in hash order — only 40 levels
     * deep from any correct ancestor.  After an LDB UTXO import the flat
     * file covers ~500K entries but the block-file scan adds ~2.5M more
     * whose pprev chains extend far past the flat-file entries.  40
     * passes can't reach them.
     *
     * New approach: for each block whose height != pprev->height+1, walk
     * UP the pprev chain collecting ancestors that also need fixing, then
     * propagate back DOWN.  Each block is visited at most twice (once up,
     * once down) so total work is O(n).  After a block is fixed the
     * height check short-circuits, so shared chain prefixes aren't
     * re-walked. */
    int total_height_fixed = 0;
    {
        /* Preallocate a stack for the deepest chain we might encounter.
         * 3M entries × 8 bytes = 24 MB — fine on any machine running a
         * full node (9+ GB RSS typical). */
        size_t stack_cap = 4096;
        struct block_index **stack = zcl_malloc(stack_cap * sizeof(*stack), "boot.index.orphan_stack");
        if (!stack) {
            fprintf(stderr, "resolve_orphan_pprev: stack alloc failed\n");
            goto skip_height;
        }

        size_t iter = 0;
        struct block_index *bi;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi || !bi->pprev) continue;
            if (bi->nHeight == bi->pprev->nHeight + 1) continue;

            /* Walk UP pprev chain to first correct ancestor.
             * Monotonicity guard prevents the realloc loop from
             * running forever on a corrupt pprev cycle. */
            int depth = 0;
            struct block_index *cur = bi;
            while (cur->pprev &&
                   cur->pprev->nHeight < cur->nHeight &&
                   cur->nHeight != cur->pprev->nHeight + 1) {
                if ((size_t)depth >= stack_cap) {
                    stack_cap *= 2;
                    struct block_index **tmp = zcl_realloc(
                        stack, stack_cap * sizeof(*stack), "boot.index.orphan_stack");
                    if (!tmp) break;
                    stack = tmp;
                }
                stack[depth++] = cur;
                cur = cur->pprev;
            }

            /* cur is now correct (or genesis with pprev==NULL).
             * Fix cur itself first if needed, then propagate down. */
            if (cur->pprev && cur->nHeight != cur->pprev->nHeight + 1) {
                cur->nHeight = cur->pprev->nHeight + 1;
                block_index_build_skip(cur);
                struct arith_uint256 proof = GetBlockProof(cur);
                arith_uint256_add(&cur->nChainWork,
                                  &cur->pprev->nChainWork, &proof);
                total_height_fixed++;
            }

            /* Propagate DOWN the stack (deepest ancestor first) */
            for (int i = depth - 1; i >= 0; i--) {
                struct block_index *fix = stack[i];
                fix->nHeight = fix->pprev->nHeight + 1;
                block_index_build_skip(fix);
                struct arith_uint256 proof = GetBlockProof(fix);
                arith_uint256_add(&fix->nChainWork,
                                  &fix->pprev->nChainWork, &proof);
                total_height_fixed++;
            }
        }

        free(stack);
    }
skip_height:

    if (total_height_fixed > 0)
        printf("  heights resolved for %d blocks\n", total_height_fixed);

    return resolved;
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
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    struct boot_scan_file_result files[257];
    int nfiles = 0;
    memset(files, 0, sizeof(files));

    /* Pass 1: parse all block files in parallel.
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

        struct boot_scan_file_result *r = &files[nfiles++];
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->file_idx = file_idx;
    }

    /* Also scan blk_sync.dat when it exists.
     * File-service bootstrap does not always create it, so missing
     * sync spool should not look like a scan failure. */
    snprintf(path, sizeof(path), "%s/blocks/blk_sync.dat", datadir);
    struct stat sync_st;
    if (stat(path, &sync_st) == 0 && sync_st.st_size > 0) {
        struct boot_scan_file_result *r = &files[nfiles++];
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->file_idx = 255;
    }

    int64_t parse_t0 = (int64_t)platform_time_wall_time_t();
    int scan_workers = scan_parse_files_parallel(files, nfiles);
    int64_t parse_elapsed = (int64_t)platform_time_wall_time_t() - parse_t0;

    int64_t apply_t0 = (int64_t)platform_time_wall_time_t();
    for (int i = 0; i < nfiles; i++) {
        struct boot_scan_file_result *r = &files[i];
        if (!r->ok) {
            fprintf(stderr, "scan: parse failed for %s; skipping partial "
                    "metadata\n", r->path);
            continue;
        }
        struct boot_scan_apply_counts c =
            scan_apply_one_file(ms, r, params);
        marked += c.marked + c.header_fixed;
        created += c.created;
        if (c.marked > 0 || c.created > 0 || c.header_fixed > 0) {
            printf("  %s: %d marked, %d created, %d headers fixed, "
                   "%d skipped (%ld MB)\n",
                   strrchr(r->path, '/') ? strrchr(r->path, '/') + 1 : r->path,
                   c.marked, c.created, c.header_fixed, r->skipped,
                   r->file_size / (1024 * 1024));
        }
    }

    /* Pass 2: retry for out-of-order blocks (prevblock now in map).
     * Block files from zclassicd are 99%+ in order, so pass 1 catches
     * nearly everything. Pass 2 picks up stragglers without re-reading
     * disk: the parsed metadata is deterministic and immutable. */
    if (created > 0 && params) {
        for (int retry = 0; retry < 3; retry++) {
            int prev_marked = marked;
            for (int i = 0; i < nfiles; i++) {
                if (!files[i].ok)
                    continue;
                struct boot_scan_apply_counts c =
                    scan_apply_one_file(ms, &files[i], params);
                marked += c.marked + c.header_fixed;
                created += c.created;
            }
            int delta = marked - prev_marked;
            if (delta == 0) break;
            printf("  Retry pass %d: %d additional blocks\n", retry + 1, delta);
        }
    }
    int64_t apply_elapsed = (int64_t)platform_time_wall_time_t() - apply_t0;

    /* Resolve orphan pprev links by reading hashPrevBlock from disk.
     * All blocks are now in the map — pprev lookup will succeed for
     * any block whose parent exists on disk. This fixes the case where
     * create_block_index_fast couldn't link pprev at insertion time
     * because the parent hadn't been scanned yet. */
    if (created > 0 && params) {
        size_t orphan_before = 0;
        { size_t ci = 0; struct block_index *cb;
          while (block_map_next(&ms->map_block_index, &ci, NULL, &cb))
              if (cb && !cb->pprev && cb->nHeight == 0 && cb->nFile >= 0)
                  orphan_before++;
        }
        printf("  orphan check: %zu entries with pprev==NULL, nHeight==0, nFile>=0\n",
               orphan_before);
        if (orphan_before > 0) {
            printf("  %zu orphan blocks — resolving pprev from disk...\n",
                   orphan_before);
            fflush(stdout);
            int resolved = resolve_orphan_pprev_from_disk(ms, datadir, params);
            printf("  pprev resolved for %d blocks from disk\n", resolved);
            fflush(stdout);
        }
    }

    scan_free_file_results(files, nfiles);

    if (marked > 0 && params)
        recompute_index_from_genesis(ms, params);

    /* Propagate nChainTx along the chain. This is REQUIRED for
     * find_most_work_chain to consider these blocks as candidates.
     * Collect all blocks with BLOCK_HAVE_DATA, sort by height,
     * compute nChainTx = pprev->nChainTx + nTx.
     * Multiple passes handle gaps (e.g., retry-created blocks
     * whose pprev was missing in earlier passes). */
    if (marked > 0) {
        size_t total = ms->map_block_index.size;
        struct block_index **sorted = zcl_malloc(total * sizeof(struct block_index *), "boot.index.sorted");
        if (sorted) {
            size_t n = 0, iter = 0;
            struct block_index *bi;
            while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
                /* Include ALL blocks with pprev or data — header-only blocks
                 * (no BLOCK_HAVE_DATA) can still propagate nChainTx through
                 * the chain, bridging gaps where block files are missing. */
                if (bi && (bi->pprev || (bi->nStatus & BLOCK_HAVE_DATA)))
                    sorted[n++] = bi;
            }

            qsort(sorted, n, sizeof(struct block_index *), block_index_cmp_height);

            int total_propagated = 0;
            for (int pass = 0; pass < 50; pass++) {
                int propagated = 0;
                for (size_t i = 0; i < n; i++) {
                    struct block_index *b = sorted[i];
                    if (b->nHeight == 0) {
                        if (b->nChainTx == 0) {
                            b->nChainTx = b->nTx > 0 ? b->nTx : 1;
                            propagated++;
                        }
                        /* Also set chain_work for h=0 blocks (genesis) */
                        if (arith_uint256_is_zero(&b->nChainWork)) {
                            b->nChainWork = GetBlockProof(b);
                            propagated++;
                        }
                    } else if (b->pprev && b->pprev->nChainTx > 0) {
                        unsigned int ntx = b->nTx > 0 ? b->nTx : 1;
                        unsigned int expected = b->pprev->nChainTx + ntx;
                        if (b->nChainTx != expected) {
                            b->nChainTx = expected;
                            propagated++;
                        }
                    } else if (b->pprev && b->pprev->nChainTx == 0) {
                        /* pprev hasn't been reached yet — force-propagate */
                        unsigned int ntx = b->pprev->nTx > 0 ? b->pprev->nTx : 1;
                        b->pprev->nChainTx = b->pprev->nHeight > 0 ?
                            (unsigned)(b->pprev->nHeight) : ntx;
                        unsigned int btx = b->nTx > 0 ? b->nTx : 1;
                        b->nChainTx = b->pprev->nChainTx + btx;
                        /* Also force chain_work if pprev has none */
                        if (arith_uint256_is_zero(&b->pprev->nChainWork)) {
                            b->pprev->nChainWork = GetBlockProof(b->pprev);
                            if (b->pprev->pprev &&
                                !arith_uint256_is_zero(&b->pprev->pprev->nChainWork))
                                arith_uint256_add(&b->pprev->nChainWork,
                                    &b->pprev->pprev->nChainWork,
                                    &b->pprev->nChainWork);
                        }
                        propagated += 2;
                    }
                    /* Also propagate nChainWork alongside nChainTx */
                    if (b->pprev && !arith_uint256_is_zero(&b->pprev->nChainWork) &&
                        arith_uint256_is_zero(&b->nChainWork)) {
                        struct arith_uint256 proof = GetBlockProof(b);
                        arith_uint256_add(&b->nChainWork,
                                          &b->pprev->nChainWork, &proof);
                        propagated++;
                    }
                }
                total_propagated += propagated;
                if (propagated == 0) break;
                if (pass == 49)
                    fprintf(stderr, "WARNING: nChainTx did not converge in "
                            "50 passes (%d blocks still pending) — possible "
                            "gap in block chain\n", propagated);
                if (pass < 3 || pass % 10 == 0)
                    printf("  nChainTx pass %d: +%d blocks\n",
                           pass + 1, propagated);
            }

            /* Find first gap in chain — diagnostic for pprev breaks */
            {
                struct block_index *genesis_bi = NULL;
                size_t gi = 0;
                struct block_index *gb;
                while (block_map_next(&ms->map_block_index, &gi, NULL, &gb))
                    if (gb && gb->nHeight == 0 && gb->nChainTx > 0) {
                        genesis_bi = gb; break;
                    }
                if (genesis_bi) {
                    /* Walk forward from genesis via the active chain */
                    int gap_h = -1;
                    struct block_index *walk = genesis_bi;
                    for (int h = 1; h < 1000 && gap_h < 0; h++) {
                        bool found_next = false;
                        size_t fi = 0;
                        struct block_index *fb;
                        while (block_map_next(&ms->map_block_index, &fi, NULL, &fb)) {
                            if (fb && fb->pprev == walk && fb->nHeight == h) {
                                walk = fb;
                                found_next = true;
                                break;
                            }
                        }
                        if (!found_next) {
                            /* Try finding ANY block at height h */
                            fi = 0;
                            struct block_index *alt = NULL;
                            while (block_map_next(&ms->map_block_index, &fi, NULL, &fb)) {
                                if (fb && fb->nHeight == h) { alt = fb; break; }
                            }
                            printf("  Chain gap at h=%d: pprev_child=%s "
                                   "alt_at_h=%s have_data=%d nTx=%u\n",
                                   h,
                                   found_next ? "yes" : "no",
                                   alt ? "yes" : "no",
                                   alt ? !!(alt->nStatus & BLOCK_HAVE_DATA) : 0,
                                   alt ? alt->nTx : 0);
                            gap_h = h;
                        }
                    }
                    if (gap_h < 0)
                        printf("  Chain contiguous from genesis to h=999+\n");
                }
            }

            /* Count blocks with HAVE_DATA but no nChainTx — these are
             * unreachable from genesis (orphans or broken pprev links) */
            int orphans = 0;
            int no_pprev = 0, pprev_no_data = 0, pprev_no_tx = 0;
            for (size_t i = 0; i < n; i++) {
                if (sorted[i]->nChainTx == 0 && sorted[i]->nHeight > 0) {
                    orphans++;
                    if (!sorted[i]->pprev) no_pprev++;
                    else if (!(sorted[i]->pprev->nStatus & BLOCK_HAVE_DATA))
                        pprev_no_data++;
                    else if (sorted[i]->pprev->nTx == 0)
                        pprev_no_tx++;
                }
            }
            /* Note: chain_work is NOT re-propagated here to avoid
             * overwriting correct values from P2P-synced blocks. */

            free(sorted);
            if (total_propagated > 0)
                printf("  nChainTx propagated for %d blocks",
                       total_propagated);
            if (orphans > 0)
                printf(" (%d orphan: %d no_pprev, %d pprev_no_data, %d pprev_no_tx)",
                       orphans, no_pprev, pprev_no_data, pprev_no_tx);
            if (total_propagated > 0 || orphans > 0)
                printf("\n");
        }
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;

    /* Summary stats: how many index entries have BLOCK_HAVE_DATA vs total */
    size_t total_entries = 0, have_data_entries = 0;
    {
        size_t si = 0;
        struct block_index *sb;
        while (block_map_next(&ms->map_block_index, &si, NULL, &sb)) {
            if (!sb) continue;
            total_entries++;
            if (sb->nStatus & BLOCK_HAVE_DATA)
                have_data_entries++;
        }
    }

    printf("Block file scan: %d marked, %d created in %llds  "
           "[parse=%llds apply=%llds workers=%d index: %zu entries, "
           "%zu have data]\n",
           marked, created, (long long)elapsed,
           (long long)parse_elapsed, (long long)apply_elapsed,
           scan_workers,
           total_entries, have_data_entries);

    return marked;
}

/* ── Reusable nChainTx + nChainWork propagation ────────────────
 * Called after scan_block_files_mark_data or any operation that
 * sets BLOCK_HAVE_DATA on blocks. Propagates nChainTx (cumulative
 * tx count) and nChainWork so find_most_work_chain can consider
 * these blocks as chain tip candidates.
 * Returns the number of blocks whose nChainTx was updated. */
int propagate_nchaintx(struct main_state *ms)
{
    if (!ms) return 0;

    size_t total = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(total * sizeof(struct block_index *), "boot.index.propagate_sorted");
    if (!sorted) return 0;

    size_t n = 0;
    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
        if (bi && (bi->pprev || (bi->nStatus & BLOCK_HAVE_DATA)))
            sorted[n++] = bi;
    }

    qsort(sorted, n, sizeof(struct block_index *), block_index_cmp_height);

    int total_propagated = 0;
    for (int pass = 0; pass < 50; pass++) {
        int propagated = 0;
        for (size_t i = 0; i < n; i++) {
            struct block_index *b = sorted[i];
            if (b->nHeight == 0) {
                if (b->nChainTx == 0) {
                    b->nChainTx = b->nTx > 0 ? b->nTx : 1;
                    propagated++;
                }
                if (arith_uint256_is_zero(&b->nChainWork)) {
                    b->nChainWork = GetBlockProof(b);
                    propagated++;
                }
            } else if (b->pprev && b->pprev->nChainTx > 0) {
                unsigned int ntx = b->nTx > 0 ? b->nTx : 1;
                unsigned int expected = b->pprev->nChainTx + ntx;
                if (b->nChainTx != expected) {
                    b->nChainTx = expected;
                    propagated++;
                }
            } else if (b->pprev && b->pprev->nChainTx == 0) {
                unsigned int ntx = b->pprev->nTx > 0 ? b->pprev->nTx : 1;
                b->pprev->nChainTx = b->pprev->nHeight > 0 ?
                    (unsigned)(b->pprev->nHeight) : ntx;
                unsigned int btx = b->nTx > 0 ? b->nTx : 1;
                b->nChainTx = b->pprev->nChainTx + btx;
                if (arith_uint256_is_zero(&b->pprev->nChainWork)) {
                    b->pprev->nChainWork = GetBlockProof(b->pprev);
                    if (b->pprev->pprev &&
                        !arith_uint256_is_zero(&b->pprev->pprev->nChainWork))
                        arith_uint256_add(&b->pprev->nChainWork,
                            &b->pprev->pprev->nChainWork,
                            &b->pprev->nChainWork);
                }
                propagated += 2;
            }
            /* Propagate nChainWork alongside nChainTx */
            if (b->pprev && !arith_uint256_is_zero(&b->pprev->nChainWork) &&
                arith_uint256_is_zero(&b->nChainWork)) {
                struct arith_uint256 proof = GetBlockProof(b);
                arith_uint256_add(&b->nChainWork,
                                  &b->pprev->nChainWork, &proof);
                propagated++;
            }
        }
        total_propagated += propagated;
        if (propagated == 0) break;
    }

    free(sorted);
    return total_propagated;
}
