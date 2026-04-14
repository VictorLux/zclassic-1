/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Block index management: chainstate rebuild/reindex, address backfill,
 * block file scanning.
 *
 * Block index flat file save/load and SQLite cache functions have been
 * extracted to app/services/src/block_index_loader.c (Phase A). */

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

/* Block index flat file save/load, SQLite cache, and LevelDB loading
 * have been extracted to app/services/src/block_index_loader.c.
 * See services/block_index_loader.h for the public API. */


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

    /* Use conservative mmap — large mmap caused SIGSEGV on some systems
     * when the address space layout conflicts with heap allocations
     * during the large GROUP BY sort buffer. 64MB is plenty. */
    sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 60000);
    /* Reduce temp store pressure — force temp tables to disk */
    sqlite3_exec(db, "PRAGMA temp_store=FILE", NULL, NULL, NULL);
    /* Use a modest cache to avoid memory bloat during aggregation */
    sqlite3_exec(db, "PRAGMA cache_size=-32768", NULL, NULL, NULL); /* 32MB */

    printf("Address backfill: aggregating UTXOs...\n");
    fflush(stdout);
    int64_t t0 = (int64_t)time(NULL);

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

    int64_t elapsed = (int64_t)time(NULL) - t0;

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
    /* Don't store solution in block_index — saves 1.3KB per entry
     * (4GB total for 3M entries). Read from disk when needed. */
    pindex->nSolution = NULL;
    pindex->nSolutionSize = 0;

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
    int marked = 0, created = 0, consec_errors = 0, skipped = 0;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "scan: cannot open %s: %s\n", filepath, strerror(errno));
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Magic bytes for fast scan (little-endian ZCL_BLOCK_MAGIC) */
    const uint8_t magic_bytes[4] = {
        ZCL_BLOCK_MAGIC & 0xFF,
        (ZCL_BLOCK_MAGIC >> 8) & 0xFF,
        (ZCL_BLOCK_MAGIC >> 16) & 0xFF,
        (ZCL_BLOCK_MAGIC >> 24) & 0xFF
    };

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
            /* Fast-scan forward for next magic instead of byte-by-byte.
             * Read a 4KB chunk and search for the magic pattern. */
            uint8_t scan_buf[4096];
            long scan_pos = pos + 1;
            bool found = false;
            while (scan_pos + 8 + 140 <= file_size) {
                fseek(f, scan_pos, SEEK_SET);
                size_t got = fread(scan_buf, 1, sizeof(scan_buf), f);
                if (got < 4) break;
                for (size_t si = 0; si + 3 < got; si++) {
                    if (scan_buf[si] == magic_bytes[0] &&
                        scan_buf[si+1] == magic_bytes[1] &&
                        scan_buf[si+2] == magic_bytes[2] &&
                        scan_buf[si+3] == magic_bytes[3]) {
                        pos = scan_pos + (long)si;
                        fseek(f, pos, SEEK_SET);
                        found = true;
                        break;
                    }
                }
                if (found) break;
                scan_pos += (long)(got - 3); /* overlap by 3 for boundary */
            }
            if (!found) break;
            skipped++;
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
            } else {
                /* Cross-check: block already has data. Verify that
                 * the stored file position matches where we found it.
                 * A mismatch means the block was moved or the index
                 * is stale — update to the file we just scanned. */
                if (bi->nFile != file_idx ||
                    bi->nDataPos != (unsigned int)(pos + 8)) {
                    bi->nFile = file_idx;
                    bi->nDataPos = (unsigned int)(pos + 8);
                }
                if (bi->nTx == 0)
                    bi->nTx = (unsigned int)num_tx;
            }
        }

        pos += 8 + (long)blk_size;
        fseek(f, pos, SEEK_SET);
    }

    fclose(f);
    if (created_out) *created_out += created;
    if (marked > 0 || created > 0)
        printf("  blk%05d.dat: %d marked, %d created, %d skipped (%ld MB)\n",
               file_idx, marked, created, skipped,
               file_size / (1024 * 1024));
    return marked;
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
            if (fread(prev_hash.data, 1, 32, f) != 32) {
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
        struct block_index **stack = malloc(stack_cap * sizeof(*stack));
        if (!stack) {
            fprintf(stderr, "resolve_orphan_pprev: stack alloc failed\n");
            goto skip_height;
        }

        size_t iter = 0;
        struct block_index *bi;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi || !bi->pprev) continue;
            if (bi->nHeight == bi->pprev->nHeight + 1) continue;

            /* Walk UP pprev chain to first correct ancestor */
            int depth = 0;
            struct block_index *cur = bi;
            while (cur->pprev &&
                   cur->nHeight != cur->pprev->nHeight + 1) {
                if ((size_t)depth >= stack_cap) {
                    stack_cap *= 2;
                    struct block_index **tmp = realloc(
                        stack, stack_cap * sizeof(*stack));
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

    int64_t elapsed = (int64_t)time(NULL) - t0;

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
           "[index: %zu entries, %zu have data]\n",
           marked, created, (long long)elapsed,
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
    struct block_index **sorted = malloc(total * sizeof(struct block_index *)); // raw-alloc-ok
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

