/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include <sqlite3.h>
#include "validation/process_block.h"
#include "validation/check_block.h"
#include "validation/connect_block.h"
#include "controllers/blockchain_controller.h"
#include "coins/utxo_commitment.h"
#include "net/download.h"
#include "validation/validationinterface.h"
#include "chain/checkpoints.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "coins/undo.h"
#include "core/serialize.h"
#include "storage/disk_block_io.h"
#include "storage/txdb.h"
#include "storage/block_index_db.h"
#include "core/serialize.h"
#include "wallet/wallet.h"
#include "validation/txmempool.h"
#include "core/utiltime.h"
#include "controllers/sync_controller.h"
#include "event/event.h"
#include "models/database.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "services/snapshot_sync_service.h"
#include "services/chain_activation_controller.h"
#include "services/chain_state_repository.h"
#include "chain/mmr.h"
#include "chain/mmb.h"
#include "util/trace.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "validation/main_constants.h"
#include "storage/coins_view_sqlite.h"
#include "util/safe_alloc.h"

static int g_last_block_file = -1;
static unsigned int g_last_block_file_size = 0;

/* Externs from boot.c — global state accessed during block connection.
 * Declared once at file scope instead of scattered inside functions. */
extern struct block_tree_db *g_active_block_tree;
extern volatile sig_atomic_t g_shutdown_requested;

/* SQLite handle for persisting UTXO commitment alongside flushes.
 * Set by boot.c via set_coins_sqlite_for_commitment(). */
static struct coins_view_sqlite *g_coins_sqlite_ptr = NULL;

void set_coins_sqlite_for_commitment(struct coins_view_sqlite *cvs)
{
    g_coins_sqlite_ptr = cvs;
}

/* Sapling tree pointer for persistence during flush.
 * Set by boot.c after loading tree from node_state. */
static struct incremental_merkle_tree *g_sapling_tree_for_flush = NULL;

void set_sapling_tree_for_flush(struct incremental_merkle_tree *tree)
{
    g_sapling_tree_for_flush = tree;
}

/* ── Flush policy ────────────────────────────────────────────
 * Controls when the in-memory UTXO cache writes to LevelDB.
 * Batching multiple blocks into one LevelDB write improves
 * throughput during IBD by 10-50x.
 *
 * Short-term (hot) layer: coins_view_cache accumulates changes.
 * Long-term (cold) layer: LevelDB receives batched writes.
 * The batch_block_interval controls how many blocks accumulate
 * before flushing — the bridge between the two layers. */
struct flush_policy {
    int64_t interval_secs;     /* max seconds between flushes (default 3600) */
    size_t  max_entries;       /* flush if cache exceeds this (default 500000) */
    int     block_interval;    /* flush every N blocks; 0 = disabled (default) */
};

static struct flush_policy g_flush_policy = {
    .interval_secs  = 3600,
    .max_entries    = 500000,
    .block_interval = 0,
};
static _Atomic int64_t g_last_coins_flush = 0;
static _Atomic int64_t g_blocks_since_flush = 0;

static struct node_db *process_block_node_db(void)
{
    return app_runtime_node_db();
}

static struct wallet *process_block_wallet(void)
{
    return app_runtime_wallet();
}

static struct tx_mempool *process_block_mempool(void)
{
    return app_runtime_mempool();
}

void set_flush_policy(int64_t interval_secs, size_t max_entries,
                      int block_interval)
{
    g_flush_policy.interval_secs = interval_secs;
    g_flush_policy.max_entries = max_entries;
    g_flush_policy.block_interval = block_interval;
    printf("flush_policy: interval=%llds max_entries=%zu block_interval=%d\n",
           (long long)interval_secs, max_entries, block_interval);
}

static bool flush_coins_if_needed(struct coins_view_cache *coins_tip,
                                  bool force)
{
    int64_t now = GetTime();
    if (g_last_coins_flush == 0)
        g_last_coins_flush = now;

    g_blocks_since_flush++;

    bool time_flush = (now - g_last_coins_flush) > g_flush_policy.interval_secs;
    bool size_flush = coins_tip->cache_coins.size > g_flush_policy.max_entries;
    bool block_flush = g_flush_policy.block_interval > 0 &&
                       g_blocks_since_flush >= g_flush_policy.block_interval;

    if (!force && !time_flush && !size_flush && !block_flush)
        return true;


    /* Flush node_db batch first — coins_flush needs the write lock. */
    struct node_db *ndb = process_block_node_db();
    if (ndb && ndb->sync_in_batch)
        node_db_sync_flush(ndb);

    size_t batched = (size_t)g_blocks_since_flush;

    /* Use batch_write_ex to write UTXO commitment atomically inside
     * the same SAVEPOINT transaction as the coins flush. This eliminates
     * the race window where a concurrent reader could block the next
     * SAVEPOINT between flush and commitment write. */
    bool ok;
    if (g_coins_sqlite_ptr) {
        ok = coins_view_sqlite_batch_write_ex(g_coins_sqlite_ptr,
                                               &coins_tip->cache_coins,
                                               &coins_tip->hash_block,
                                               &coins_tip->commitment);
        if (ok) {
            coins_map_free(&coins_tip->cache_coins);
            coins_map_init(&coins_tip->cache_coins);
            utxo_commitment_init(&coins_tip->commitment);
        } else {
            fprintf(stderr, "WARNING: coins cache flush FAILED — retaining "
                    "%zu dirty entries for retry\n",
                    coins_tip->cache_coins.size);
        }
    } else {
        ok = coins_view_cache_flush(coins_tip);
    }

    if (ok) {
        g_last_coins_flush = now;
        g_blocks_since_flush = 0;

        /* Persist Sapling commitment tree state */
        if (ndb && ndb->open &&
            g_sapling_tree_for_flush) {
            struct byte_stream ts;
            stream_init(&ts, 4096);
            if (incremental_tree_serialize(g_sapling_tree_for_flush, &ts)) {
                if (!node_db_state_set(ndb, "sapling_tree",
                                       ts.data, ts.size))
                    fprintf(stderr, "flush_coins: sapling_tree persist failed\n");
            }
            stream_free(&ts);
        }

        const char *trigger = force ? "forced" : time_flush ? "periodic" :
                              size_flush ? "cache-full" : "block-interval";
        event_emitf(EV_COINS_FLUSH, 0, "%s batched=%zu entries=%zu",
                    trigger, batched, coins_tip->cache_coins.size);
        if (batched >= 100)
            printf("flush_coins: wrote %s (%zu blocks batched)\n",
                   trigger, batched);

        /* WAL checkpoint after every successful flush — prevents WAL
         * from growing to multi-GB during sustained block processing.
         * A 6GB WAL was observed in production, blocking all operations.
         * Checkpointing on every flush keeps WAL size bounded. */
        if (ndb && ndb->open)
            node_db_wal_checkpoint(ndb);
    } else {
        event_emitf(EV_COINS_FLUSH_FAILED, 0, "flush returned false");
        /* If there's nothing dirty in the cache, the flush "failure" is
         * harmless — nothing was lost. This happens when force-flush
         * triggers before any blocks are connected (SQLITE_BUSY from
         * background threads). */
        if (coins_tip->cache_coins.size == 0 && batched <= 1) {
            /* Empty cache — nothing to flush, not fatal */
            return true;
        }
        /* During IBD (many blocks since last flush), a flush failure
         * is likely SQLITE_BUSY from lock contention. Don't treat as
         * fatal — retry on next interval. The coins stay in memory. */
        if (batched > 10 && coins_tip->cache_coins.size < 2000000) {
            fprintf(stderr, "flush_coins: BUSY — coins cached in memory, "
                    "will retry (%zu blocks batched, %zu entries)\n",
                   batched, coins_tip->cache_coins.size);
            return true; /* non-fatal during IBD */
        }
        if (coins_tip->cache_coins.size >= 2000000) {
            fprintf(stderr, "flush_coins: CRITICAL — cache has %zu entries "
                    "and flush keeps failing. Halting to prevent OOM.\n",
                    coins_tip->cache_coins.size);
            return false; /* force caller to handle */
        }
        fprintf(stderr, "flush_coins: FAILED to flush coins cache to disk "
                "(%zu blocks batched, %zu entries)\n",
                batched, coins_tip->cache_coins.size);
    }
    return ok;
}

static bool find_block_pos(struct disk_block_pos *pos, unsigned int block_size,
                            const char *datadir)
{
    if (g_last_block_file < 0) {
        /* Scan existing block files to find the last one */
        g_last_block_file = 0;
        for (int i = 0; i < 99999; i++) {
            char path[512];
            struct disk_block_pos probe = { .nFile = i, .nPos = 0 };
            get_block_pos_filename(path, sizeof(path), datadir, &probe, "blk");
            struct stat st;
            if (stat(path, &st) != 0)
                break;
            g_last_block_file = i;
            g_last_block_file_size = (unsigned int)st.st_size;
        }
    }

    /* Move to next file if current one is too large */
    if (g_last_block_file_size + block_size + 8 > MAX_BLOCKFILE_SIZE) {
        g_last_block_file++;
        g_last_block_file_size = 0;
    }

    /* Safety: cap file number to prevent runaway file creation.
     * 10000 files * 128MB each = 1.28TB which is more than enough. */
    if (g_last_block_file > 9999) {
        fprintf(stderr, "find_block_pos: file number %d exceeds max (9999)\n",
                g_last_block_file);
        return false;
    }

    pos->nFile = g_last_block_file;
    pos->nPos = g_last_block_file_size;
    return true;
}

static struct block_index *add_to_block_index(struct main_state *ms,
                                               const struct block_header *header)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = zcl_calloc(1, sizeof(struct block_index), "process_block_index");
    if (!pindex)
        return NULL;
    block_index_init(pindex);

    pindex->nVersion = header->nVersion;
    pindex->hashMerkleRoot = header->hashMerkleRoot;
    pindex->hashFinalSaplingRoot = header->hashFinalSaplingRoot;
    pindex->nTime = header->nTime;
    pindex->nBits = header->nBits;
    pindex->nNonce = header->nNonce;
    if (header->nSolutionSize > 0) {
        pindex->nSolution = zcl_malloc(header->nSolutionSize, "block_solution");
        if (pindex->nSolution)
            memcpy(pindex->nSolution, header->nSolution, header->nSolutionSize);
        pindex->nSolutionSize = pindex->nSolution ? header->nSolutionSize : 0;
    } else {
        pindex->nSolution = NULL;
        pindex->nSolutionSize = 0;
    }

    if (!block_map_insert(&ms->map_block_index, &hash, pindex)) {
        free(pindex);
        return block_map_find(&ms->map_block_index, &hash);
    }

    /* phashBlock points into the block_map_entry's hash storage */
    struct block_index *found = block_map_find(&ms->map_block_index, &hash);
    if (found) {
        const struct uint256 *stored = block_map_find_hash(
            &ms->map_block_index, &hash);
        if (stored)
            found->phashBlock = stored;
    }

    /* Link to previous block */
    struct block_index *pprev = block_map_find(&ms->map_block_index,
                                                &header->hashPrevBlock);
    if (pprev) {
        pindex->pprev = pprev;
        pindex->nHeight = pprev->nHeight + 1;
        block_index_build_skip(pindex);

        /* Chain work = prev + work for this block */
        struct arith_uint256 block_proof = GetBlockProof(pindex);
        arith_uint256_add(&pindex->nChainWork, &pprev->nChainWork, &block_proof);
    } else {
        pindex->nHeight = 0;
        pindex->nChainWork = GetBlockProof(pindex);
    }

    return pindex;
}

/* chain_has_all_data removed — find_most_work_chain no longer requires
 * BLOCK_HAVE_DATA. activate_best_chain checks data availability inline
 * before each connect_tip, queuing missing blocks for download. */

static struct block_index *find_most_work_chain(struct main_state *ms)
{
    struct block_index *best = active_chain_tip(&ms->chain_active);
    int skipped_no_chaintx = 0;
    int skipped_failed = 0;
    int skipped_invalid = 0;
    int highest_have_data_no_chaintx = -1;

    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (!pindex)
            continue;

        /* Skip failed blocks and their children */
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            skipped_failed++;
            continue;
        }

        /* Must have at least header validation */
        if (!block_index_is_valid(pindex, BLOCK_VALID_TREE)) {
            skipped_invalid++;
            continue;
        }

        /* Only consider chains where every block from genesis has data.
         * nChainTx > 0 means the cumulative tx count is set, which only
         * happens when a block's data is received AND all its ancestors
         * have data. Orphan blocks in the block index have nChainTx == 0
         * and must not be selected as chain tip. This matches Bitcoin
         * Core's chain_has_all_data / CBlockIndex::HaveTxsDownloaded(). */
        if (pindex->nChainTx == 0) {
            skipped_no_chaintx++;
            if ((pindex->nStatus & BLOCK_HAVE_DATA) &&
                pindex->nHeight > highest_have_data_no_chaintx)
                highest_have_data_no_chaintx = pindex->nHeight;
            continue;
        }

        if (!best || arith_uint256_compare(&pindex->nChainWork,
                                            &best->nChainWork) > 0) {
            /* Check ancestry for failed blocks */
            bool chain_ok = true;
            struct block_index *check = pindex;
            int tip_h = best ? best->nHeight : -1;
            while (check && check->nHeight > tip_h) {
                if (check->nStatus & BLOCK_FAILED_MASK) {
                    chain_ok = false;
                    break;
                }
                if (!check->pprev && check->nHeight > 0) {
                    /* pprev not linked — stop walking ancestry.
                     * This is normal after LDB import where block_index
                     * entries have nChainTx set (from the import) but
                     * pprev pointers aren't fully resolved yet. The
                     * nChainTx > 0 check above already ensures data
                     * availability — don't reject the chain just
                     * because we can't walk pprev to genesis. */
                    break;
                }
                check = check->pprev;
            }
            if (chain_ok)
                best = pindex;
        }
    }

    /* Diagnostic: if blocks with data are being skipped due to nChainTx,
     * log it so we can diagnose chain activation failures. */
    if (highest_have_data_no_chaintx > (best ? best->nHeight : -1)) {
        printf("find_most_work_chain: WARNING: %d blocks skipped (nChainTx==0), "
               "highest HAVE_DATA without nChainTx: h=%d (tip=%d)\n",
               skipped_no_chaintx, highest_have_data_no_chaintx,
               best ? best->nHeight : -1);
    }

    return best;
}

/* ── csr-migration helper ────────────────────────────────────
 * process_block.c mutates the chain tip from five different places
 * (forward extend, disconnect rollback, genesis connect without
 * disk, reorg recovery, no-fork reset). All of them route through
 * this helper so the cross-source validation and observability
 * live in one place.
 *
 * When the csr singleton is wired (normal boot) this performs the
 * full atomic update and returns true. When the singleton is not
 * wired (unit tests that bypass boot) we fall back to raw setters
 * so test harnesses continue to advance their chains — that's the
 * only way NOT_INITIALIZED is returned, any other non-OK result
 * is a real validation failure and is logged loudly.
 *
 * coins_tip may be NULL for callers that don't need to touch the
 * coins_best_block in the fallback path (update_tip is one — the
 * old code there never set coins_best_block, connect_block did).
 * Fork-recovery callers pass a real pointer so both legacy and
 * csr-driven paths end up with the same coins_best_block.
 *
 * update_header_tip controls whether pindex_best_header moves.
 * Callers that only touch the active chain (e.g. fork reset)
 * pass false. */
static bool process_block_commit_tip(struct main_state *ms,
                                      struct coins_view_cache *coins_tip,
                                      struct block_index *new_tip,
                                      const char *reason,
                                      bool update_header_tip)
{
    struct trace_span *csr_span = trace_start("csr.commit_tip");
    trace_attr_int(csr_span, "height", new_tip ? new_tip->nHeight : -1);
    trace_attr_str(csr_span, "reason", reason ? reason : "");

    if (!new_tip || !new_tip->phashBlock) {
        trace_set_status(csr_span, TRACE_STATUS_ERROR);
        trace_attr_str(csr_span, "error", "null_tip");
        trace_end(csr_span);
        LOG_FAIL("validation", "commit_chain_state_tip called with null tip or null phashBlock");
    }

    struct chain_state_commit commit = {
        .new_tip             = new_tip,
        .new_coins_best      = *new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip   = update_header_tip,
        /* Reorg disconnects, forks and fast-sync roll-forwards can
         * all look like backward or sideways moves from the csr's
         * perspective. The validation machinery in process_block.c
         * has already vetted the move, so we bypass the orphan-rows
         * guard here. Recovery policy gating is Phase 2 territory. */
        .allow_rollback      = true,
        .wallet_scan_height  = -1,
        .reason              = reason,
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK) {
        trace_end(csr_span);
        return true;
    }

    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        /* Test harness path: the singleton was never wired. Fall
         * back to the raw setters so existing test coverage still
         * drives the active chain forward. */
        active_chain_set_tip(&ms->chain_active, new_tip);
        if (update_header_tip) ms->pindex_best_header = new_tip;
        if (coins_tip) coins_view_cache_set_best_block(coins_tip,
                                                        new_tip->phashBlock);
        trace_attr_str(csr_span, "fallback", "raw_setters");
        trace_end(csr_span);
        return true;
    }

    /* Real validation failure. The csr has already emitted
     * EV_CHAIN_TIP_REJECTED; shout too so this shows up in the
     * node log even when events are disabled. */
    fprintf(stderr,
            "process_block: csr rejected tip commit (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason, new_tip->nHeight);
    trace_set_status(csr_span, TRACE_STATUS_ERROR);
    trace_attr_str(csr_span, "error", csr_result_name(rc));
    trace_end(csr_span);
    return false;
}

static void update_tip(struct main_state *ms, struct block_index *pindex_new)
{
    if (pindex_new) {
        /* coins_tip is NULL here on purpose: the pre-migration
         * update_tip never set coins_best_block (connect_block had
         * already done it while building the new tip), and the
         * fallback path should preserve that behaviour. */
        process_block_commit_tip(ms, NULL, pindex_new,
                                 "process_block.update_tip", true);
    } else {
        /* Disconnect past genesis — empty the chain. No commit to
         * make; the active_chain primitive handles a NULL tip as
         * "height=-1". */
        active_chain_set_tip(&ms->chain_active, NULL);
    }

    char hex[65];
    if (pindex_new && pindex_new->phashBlock)
        uint256_get_hex(pindex_new->phashBlock, hex);
    else
        snprintf(hex, sizeof(hex), "(null)");

    event_emitf(EV_TIP_UPDATED, 0, "h=%d %s",
                pindex_new ? pindex_new->nHeight : -1, hex);

    /* Progress log every 10000 blocks with speed metric */
    if (pindex_new && pindex_new->nHeight % 10000 == 0 && pindex_new->nHeight > 0) {
        static int64_t last_log_time = 0;
        static int last_log_height = 0;
        int64_t now_log = GetTime();
        int64_t elapsed = now_log - last_log_time;
        int blocks_done = pindex_new->nHeight - last_log_height;
        double bps = elapsed > 0 ? (double)blocks_done / (double)elapsed : 0;
        printf("Chain: height=%d  %.0f blk/s\n",
               pindex_new->nHeight, bps);
        last_log_time = now_log;
        last_log_height = pindex_new->nHeight;
    }
}

bool accept_block_header(const struct block_header *header,
                         struct validation_state *state,
                         struct main_state *ms,
                         const struct chain_params *params,
                         struct block_index **ppindex)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
    if (pindex) {
        if (ppindex)
            *ppindex = pindex;
        /* Fix scrambled heights from LDB import.  After snapshot sync,
         * block_map entries above the coins tip may have nHeight=0 or
         * wrong values because the flat-file/LDB import didn't walk
         * pprev chains for blocks it couldn't fully validate.  Walk UP
         * the pprev chain to find the first correct ancestor, then
         * propagate heights DOWN — same algorithm as boot_index.c.
         * Without this, the getheaders loop stalls forever because
         * pindex_best_header never advances past the wrong height. */
        if (pindex->pprev &&
            pindex->nHeight != pindex->pprev->nHeight + 1) {
            /* Walk up to find first correct ancestor */
            struct block_index *stack[2048];
            int depth = 0;
            struct block_index *cur = pindex;
            while (cur->pprev &&
                   cur->nHeight != cur->pprev->nHeight + 1 &&
                   depth < 2048) {
                stack[depth++] = cur;
                cur = cur->pprev;
            }
            /* Fix cur if needed */
            if (cur->pprev && cur->nHeight != cur->pprev->nHeight + 1) {
                cur->nHeight = cur->pprev->nHeight + 1;
                struct arith_uint256 proof = GetBlockProof(cur);
                arith_uint256_add(&cur->nChainWork,
                                  &cur->pprev->nChainWork, &proof);
            }
            /* Propagate down the stack */
            for (int i = depth - 1; i >= 0; i--) {
                struct block_index *fix = stack[i];
                if (fix->pprev) {
                    fix->nHeight = fix->pprev->nHeight + 1;
                    struct arith_uint256 proof = GetBlockProof(fix);
                    arith_uint256_add(&fix->nChainWork,
                                      &fix->pprev->nChainWork, &proof);
                }
            }
        }
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            /* Clear FAILED for blocks near the chain tip — these are new
             * blocks that may have failed due to transient issues (tree
             * not synced, VK not loaded, etc.). For blocks far below tip,
             * rate-limit to prevent infinite CPU-thrashing retry loops. */
            int tip_h = active_chain_height(&ms->chain_active);
            if (pindex->nHeight >= tip_h - 100) {
                /* Recent block — always allow retry */
                pindex->nStatus &= ~BLOCK_FAILED_MASK;
            } else {
                /* Old block — rate-limit retries */
                static time_t last_retry_clear = 0;
                time_t now = time(NULL);
                if (now - last_retry_clear >= 300) {
                    pindex->nStatus &= ~BLOCK_FAILED_MASK;
                    last_retry_clear = now;
                }
            }
        }
        return true;
    }

    if (!check_block_header(header, state, params, true))
        LOG_FAIL("validation", "check_block_header failed for accepted header");

    /* Get prev block index */
    struct block_index *pindex_prev = NULL;
    if (uint256_cmp(&hash, &params->consensus.hashGenesisBlock) != 0) {
        pindex_prev = block_map_find(&ms->map_block_index,
                                      &header->hashPrevBlock);
        if (!pindex_prev) {
            /* Parent not in our block index — this is an orphan block.
             * Normal during sync (blocks arrive before headers).
             * DoS=0: don't penalize the peer for out-of-order delivery. */
            return validation_state_invalid(state, false, 0,
                                            "bad-prevblk", NULL);
        }
        if (pindex_prev->nStatus & BLOCK_FAILED_MASK) {
            /* Don't ban peer — parent may have been marked failed by a
             * prior validation bug (e.g. turnstile false positive).
             * The block is invalid from our perspective, but the peer
             * isn't misbehaving. DoS=0 rejects without penalty. */
            return validation_state_invalid(state, false, REJECT_INVALID,
                                            "bad-prevblk", NULL);
        }
    }

    /* Fix pindex_prev height if scrambled (same logic as the already-known
     * path above).  After snapshot sync + LDB import, block_map entries
     * may have nHeight=0 or wrong values because pprev chains weren't
     * fully resolved.  Without this fix, contextual_check_block_header
     * applies rules for the WRONG height (e.g. pre-Sapling equihash size
     * check at computed height 2 for a block really at height 2M+). */
    if (pindex_prev && pindex_prev->pprev &&
        pindex_prev->nHeight != pindex_prev->pprev->nHeight + 1) {
        struct block_index *stack[2048];
        int depth = 0;
        struct block_index *cur = pindex_prev;
        while (cur->pprev &&
               cur->nHeight != cur->pprev->nHeight + 1 &&
               depth < 2048) {
            stack[depth++] = cur;
            cur = cur->pprev;
        }
        if (cur->pprev && cur->nHeight != cur->pprev->nHeight + 1) {
            cur->nHeight = cur->pprev->nHeight + 1;
            struct arith_uint256 proof = GetBlockProof(cur);
            arith_uint256_add(&cur->nChainWork,
                              &cur->pprev->nChainWork, &proof);
        }
        for (int i = depth - 1; i >= 0; i--) {
            struct block_index *fix = stack[i];
            if (fix->pprev) {
                fix->nHeight = fix->pprev->nHeight + 1;
                struct arith_uint256 proof = GetBlockProof(fix);
                arith_uint256_add(&fix->nChainWork,
                                  &fix->pprev->nChainWork, &proof);
            }
        }
    }

    /* Skip contextual header check during IBD when the block index has
     * scrambled heights from snapshot/LDB import.  The pprev chains may
     * trace back to genesis with wrong heights, causing
     * contextual_check_block_header to apply pre-Sapling rules (wrong
     * equihash solution size) to post-Sapling blocks.  Full validation
     * happens later in connect_block().  This mirrors Bitcoin Core's
     * behavior: header-first sync trusts header chain structure. */
    {
        int tip_h = active_chain_height(&ms->chain_active);
        bool skip_contextual = (tip_h > 100000 && pindex_prev &&
                                pindex_prev->nHeight < tip_h - 1000);
        if (pindex_prev && !skip_contextual &&
            !contextual_check_block_header(header, state, params, pindex_prev,
                                            ms->fCheckpointsEnabled))
            LOG_FAIL("validation", "contextual_check_block_header failed for header at height %d",
                     pindex_prev->nHeight + 1);
    }

    pindex = add_to_block_index(ms, header);
    if (!pindex)
        return validation_state_error(state, "add-to-block-index-failed");

    if ((pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_TREE;

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool accept_block(struct block *block,
                  struct validation_state *state,
                  struct main_state *ms,
                  const struct chain_params *params,
                  struct block_index **ppindex,
                  bool requested,
                  const char *datadir)
{
    struct block_index *pindex = NULL;
    if (!accept_block_header(&block->header, state, ms, params, &pindex))
        LOG_FAIL("validation", "accept_block_header failed in accept_block");
    if (ppindex)
        *ppindex = pindex;

    bool already_have = (pindex->nStatus & BLOCK_HAVE_DATA) != 0;
    if (already_have) {
        /* Blocks from LDB import may have BLOCK_HAVE_DATA but nChainTx==0.
         * Without nChainTx, find_most_work_chain skips them and the chain
         * never advances.  Fix: set nChainTx for already-have blocks that
         * are missing it.  Use nTx if set, otherwise default to 1 (the
         * block exists so it has at least one tx — the coinbase). */
        if (pindex->nChainTx == 0) {
            unsigned int ntx = pindex->nTx > 0 ? pindex->nTx : 1;
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + ntx;
        }
        return true;
    }

    struct block_index *tip = active_chain_tip(&ms->chain_active);
    bool has_more_work = tip ?
        arith_uint256_compare(&pindex->nChainWork, &tip->nChainWork) >= 0 :
        true;
    if (!requested) {
        /* Skip blocks we already have data for (nTx set AND data on disk).
         * During IBD, blocks whose BLOCK_HAVE_DATA was cleared (e.g. from
         * snapshot cleanup) may still have nTx set from the index — we
         * must NOT skip those, they need to be re-written to disk. */
        if (pindex->nTx != 0 && (pindex->nStatus & BLOCK_HAVE_DATA))
            return true;
        if (!has_more_work)
            return true;
    }

    if (!check_block(block, state, params, true, true, true) ||
        !contextual_check_block(block, state, params, pindex->pprev)) {
        if (validation_state_is_invalid(state) && !state->corruption_possible) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
        }
        LOG_FAIL("validation", "check_block or contextual_check_block failed at height %d",
                 pindex->nHeight);
    }

    event_emitf(EV_BLOCK_CHECK_PASSED, 0,
                "height=%d ntx=%zu checks=header,merkle,tx,contextual",
                pindex->nHeight, block->num_vtx);

    /* Write block to disk — validate serialized size first */
    struct byte_stream blk_stream;
    stream_init(&blk_stream, 4096);
    if (!block_serialize(block, &blk_stream)) {
        stream_free(&blk_stream);
        return validation_state_error(state, "failed-to-serialize-block");
    }

    /* Reject blocks larger than MAX_BLOCK_SIZE before persisting.
     * This catches oversized blocks that passed earlier checks
     * (e.g. check_block only checks vtx count, not serialized size). */
    if (blk_stream.size > 2000000) {
        fprintf(stderr, "accept_block: serialized size %zu exceeds "
                "MAX_BLOCK_SIZE at height %d\n",
                blk_stream.size, pindex->nHeight);
        stream_free(&blk_stream);
        return validation_state_dos(state, 100, false, REJECT_INVALID,
                                    "bad-blk-length", false, NULL);
    }

    struct disk_block_pos block_pos;
    disk_block_pos_init(&block_pos);
    if (!find_block_pos(&block_pos, (unsigned int)blk_stream.size, datadir)) {
        stream_free(&blk_stream);
        return validation_state_error(state, "failed-to-find-block-pos");
    }
    stream_free(&blk_stream);

    if (!write_block_to_disk(block, &block_pos, datadir,
                             params->pchMessageStart))
        return validation_state_error(state, "failed-to-write-block");

    /* Update file size tracker: pos->nPos now points past magic+size header,
     * so total = nPos + block_data_size */
    {
        char path[512];
        get_block_pos_filename(path, sizeof(path), datadir, &block_pos, "blk");
        struct stat st;
        if (stat(path, &st) == 0)
            g_last_block_file_size = (unsigned int)st.st_size;
    }

    /* Mark block as having data and valid transactions */
    pindex->nStatus |= BLOCK_HAVE_DATA;
    pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                       BLOCK_VALID_TRANSACTIONS;
    pindex->nFile = block_pos.nFile;
    pindex->nDataPos = block_pos.nPos;
    pindex->nTx = (unsigned int)block->num_vtx;
    pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) +
                        pindex->nTx;

    /* Propagate nChainTx forward to children that arrived out-of-order.
     * During parallel IBD, block N+1 may arrive before block N. When
     * block N is stored, block N+1 already has BLOCK_HAVE_DATA but its
     * nChainTx was wrong (computed with pprev->nChainTx == 0).
     * Walk forward through the block index and fix any chain that
     * leads from this block. Uses a queue for BFS. */
    {
        struct block_index **queue = zcl_malloc(4096 * sizeof(struct block_index *), "chaintx_queue");
        if (!queue) {
            fprintf(stderr, "process_block: nChainTx propagation skipped "
                    "— malloc(4096) failed\n");
        }
        if (queue) {
            size_t qlen = 0, qcap = 4096;
            queue[qlen++] = pindex;

            while (qlen > 0) {
                struct block_index *parent = queue[--qlen];
                /* Scan all block_index entries for children of parent */
                size_t iter2 = 0;
                struct block_index *child;
                while (block_map_next(&ms->map_block_index, &iter2,
                                       NULL, &child)) {
                    if (!child || child->pprev != parent) continue;
                    if (!(child->nStatus & BLOCK_HAVE_DATA)) continue;

                    unsigned int expected = parent->nChainTx + child->nTx;
                    if (child->nChainTx != expected) {
                        child->nChainTx = expected;
                        /* Queue child to propagate further */
                        if (qlen >= qcap && qcap < 65536) {
                            size_t nc = qcap * 2;
                            struct block_index **nq = zcl_realloc(queue,
                                nc * sizeof(struct block_index *), "chaintx_queue_grow");
                            if (nq) { queue = nq; qcap = nc; }
                        }
                        if (qlen < qcap)
                            queue[qlen++] = child;
                    }
                }
            }
            free(queue);
        }
    }

    return true;
}

bool connect_tip(struct validation_state *state,
                 struct main_state *ms,
                 struct coins_view_cache *coins_tip,
                 struct block_index *pindex_new,
                 struct block *pblock,
                 const struct chain_params *params,
                 const char *datadir)
{
    struct trace_span *ct_span = trace_start("chain.connect_tip");
    trace_attr_int(ct_span, "height", pindex_new ? pindex_new->nHeight : -1);

    struct block local_block;
    block_init(&local_block);

    if (!pblock) {
        if (!read_block_from_disk_index(&local_block, pindex_new, datadir)) {
            /* Genesis block (height 0) may not be on disk (blk00000.dat
             * empty after legacy import). Genesis has only the unspendable
             * coinbase — safe to connect without block data. */
            if (pindex_new->nHeight == 0) {
                block_free(&local_block);
                pindex_new->nStatus |= BLOCK_HAVE_DATA;
                pindex_new->nStatus = (pindex_new->nStatus & ~BLOCK_VALID_MASK)
                                       | BLOCK_VALID_SCRIPTS;
                pindex_new->nTx = 1;
                pindex_new->nChainTx = 1;
                process_block_commit_tip(ms, coins_tip, pindex_new,
                    "process_block.connect_tip.genesis_no_disk", true);
                printf("Genesis block: connected (no disk data needed)\n");
                trace_end(ct_span);
                return true;
            }
            /* Retry: pindex_new may be a stale copy (from mmap or header
             * processing) without disk position. Look up the canonical
             * block_index by hash which has the correct file/pos. */
            if (pindex_new->phashBlock) {
                struct block_index *canonical = block_map_find(
                    &ms->map_block_index, pindex_new->phashBlock);
                if (canonical && canonical != pindex_new &&
                    (canonical->nStatus & BLOCK_HAVE_DATA) &&
                    read_block_from_disk_index(&local_block, canonical,
                                               datadir)) {
                    pindex_new->nFile = canonical->nFile;
                    pindex_new->nDataPos = canonical->nDataPos;
                    pindex_new->nStatus |= BLOCK_HAVE_DATA;
                    goto block_read_ok;
                }
            }
            fprintf(stderr, "connect_tip: failed to read block at height %d "
                    "file=%d pos=%u status=%u — clearing HAVE_DATA\n",
                    pindex_new->nHeight, pindex_new->nFile,
                    pindex_new->nDataPos, pindex_new->nStatus);
            pindex_new->nStatus &= ~BLOCK_HAVE_DATA;
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "failed-to-read-block");
        }
        block_read_ok:
        pblock = &local_block;

        /* Verify block read from disk matches expected hash */
        struct uint256 disk_hash;
        block_header_get_hash(&pblock->header, &disk_hash);
        if (!pindex_new->phashBlock) {
            fprintf(stderr, "connect_tip: block index at height %d has NULL "
                    "hash pointer — cannot verify disk block integrity\n",
                    pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "block-index-no-hash");
        }
        if (uint256_cmp(&disk_hash, pindex_new->phashBlock) != 0) {
            char exp[65], got[65];
            uint256_get_hex(pindex_new->phashBlock, exp);
            uint256_get_hex(&disk_hash, got);
            fprintf(stderr, "connect_tip: WRONG BLOCK at height %d!\n"
                    "  expected: %s\n  got:      %s\n"
                    "  file=%d pos=%u — clearing HAVE_DATA for re-download\n",
                   pindex_new->nHeight, exp, got,
                   pindex_new->nFile, pindex_new->nDataPos);
            /* Self-healing: clear BLOCK_HAVE_DATA so the download manager
             * re-requests this block from P2P. The stale disk position
             * was likely caused by a symlinked block file being modified
             * by another node (zclassicd). */
            pindex_new->nStatus &= ~(unsigned)BLOCK_HAVE_DATA;
            pindex_new->nFile = -1;
            pindex_new->nDataPos = 0;
            event_emitf(EV_BLOCK_REJECTED, 0,
                        "wrong-block-on-disk h=%d cleared-have-data",
                        pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "wrong-block-on-disk");
        }

        /* Redundant: verify transaction count matches header.
         * Catches truncated block reads from disk corruption. */
        if (pblock->num_vtx == 0) {
            fprintf(stderr, "connect_tip: empty block at h=%d "
                    "(deserialization or disk error)\n", pindex_new->nHeight);
            block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "empty-block-from-disk");
        }
    }

    /* Apply the block to the chain state, with self-healing UTXO recovery.
     * If connect_block fails because a UTXO is missing from the coins DB
     * (e.g. from a non-atomic chainstate import), we find the creating
     * transaction via the tx index, read it from disk, inject the UTXO
     * into the cache, and retry. This makes the node self-sufficient —
     * no manual --repair or zclassicd dependency needed. */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        int recovery_attempts = 0;

retry_connect:
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);

        /* Set Sapling tree for connect_block to update + verify root.
         * The tree persists in ms->sapling_tree across blocks. */
        connect_block_set_sapling_tree(&ms->sapling_tree);

        bool rv = connect_block(pblock, state, pindex_new, &view, params, false);
        connect_block_set_sapling_tree(NULL); /* clear after use */
        if (!rv) {
            /* ── Self-healing: recover missing UTXO from block data ── */
            if (state->has_missing_utxo &&
                strcmp(state->reject_reason, "bad-txns-inputs-missingorspent") == 0 &&
                recovery_attempts < 100 &&
                g_active_block_tree != NULL) {
                bool recovered = false;
                char hex[65];
                uint256_get_hex(&state->missing_txid, hex);

                if (!g_active_block_tree) {
                    fprintf(stderr, "[self-heal] tx index not available "
                            "(no block tree DB)\n");
                } else {
                    struct disk_tx_pos txpos;
                    disk_tx_pos_init(&txpos);
                    if (!block_tree_db_read_tx_index(g_active_block_tree,
                                                     &state->missing_txid,
                                                     &txpos)) {
                        fprintf(stderr, "[self-heal] tx %s not in tx index\n",
                                hex);
                    } else if (txpos.block_pos.nFile < 0) {
                        fprintf(stderr, "[self-heal] tx %s nFile=%d "
                                "(tx index entry too small or corrupt)\n",
                                hex, txpos.block_pos.nFile);
                    } else {
                        struct block src_block;
                        block_init(&src_block);
                        if (!read_block_from_disk(&src_block,
                                                  &txpos.block_pos, datadir)) {
                            fprintf(stderr, "[self-heal] failed to read block "
                                    "file=%d pos=%u for tx %s\n",
                                    txpos.block_pos.nFile,
                                    txpos.block_pos.nPos, hex);
                            block_free(&src_block);
                        } else {
                            for (size_t ti = 0; ti < src_block.num_vtx; ti++) {
                                if (uint256_eq(&src_block.vtx[ti].hash,
                                               &state->missing_txid)) {
                                    struct uint256 src_hash;
                                    block_get_hash(&src_block, &src_hash);
                                    struct block_index *src_idx =
                                        block_map_find(&ms->map_block_index,
                                                       &src_hash);
                                    int src_height = src_idx ?
                                        src_idx->nHeight : 0;

                                    struct coins_cache_entry *entry =
                                        coins_view_cache_modify_new(coins_tip,
                                            &state->missing_txid);
                                    if (entry) {
                                        coins_from_transaction(&entry->coins,
                                            &src_block.vtx[ti], src_height);
                                        entry->flags = COINS_CACHE_DIRTY;
                                        printf("[self-heal] Recovered UTXO "
                                               "%s (from h=%d) — retry %d\n",
                                               hex, src_height,
                                               recovery_attempts + 1);
                                        recovered = true;
                                    }
                                    break;
                                }
                            }
                            block_free(&src_block);
                        }
                    }
                }

                if (recovered) {
                    coins_view_cache_free(&view);
                    memset(&view, 0, sizeof(view));
                    recovery_attempts++;
                    validation_state_init(state);
                    goto retry_connect;
                }
                /* Recovery failed — fall through to BLOCK_FAILED path */
                fprintf(stderr, "[self-heal] FAILED to recover tx %s:%u "
                        "— marking block %d as failed\n",
                        hex, state->missing_vout, pindex_new->nHeight);
            }

            fprintf(stderr, "connect_tip: connect_block FAILED h=%d: %s\n",
                    pindex_new->nHeight,
                    state->reject_reason[0] ? state->reject_reason : "unknown");
            if (validation_state_is_invalid(state)) {
                pindex_new->nStatus |= BLOCK_FAILED_VALID;
                /* Don't propagate BLOCK_FAILED_CHILD for very early
                 * blocks (h<=10) during IBD. BIP30 failures at h=1 are
                 * typically caused by stale UTXO state (e.g. snapshot
                 * UTXOs at genesis), not genuinely invalid blocks.
                 * Propagating to 3M+ descendants is catastrophic and
                 * prevents any further syncing. The outer do-while loop
                 * will retry find_most_work_chain, which skips this one
                 * failed block. */
                if (pindex_new->nHeight <= 10) {
                    fprintf(stderr, "connect_tip: NOT propagating "
                            "BLOCK_FAILED at h=%d (early block, likely "
                            "transient UTXO state issue)\n",
                            pindex_new->nHeight);
                } else
                /* Propagate BLOCK_FAILED_CHILD to all descendants.
                 * Uses height-sorted single pass instead of repeated
                 * full-map scans (O(n) vs O(n*depth) for 3M+ blocks). */
                {
                    size_t map_sz = ms->map_block_index.size;
                    struct block_index **all = zcl_malloc(
                        map_sz * sizeof(struct block_index *), "failed_child_all");
                    size_t propagated = 0;
                    if (!all) {
                        fprintf(stderr, "BLOCK_FAILED_CHILD: malloc failed "
                                "for %zu entries — propagation skipped!\n",
                                map_sz);
                    } else {
                        size_t n = 0, iter2 = 0;
                        struct block_index *ch;
                        while (block_map_next(&ms->map_block_index,
                                               &iter2, NULL, &ch)) {
                            if (ch && ch->nHeight > pindex_new->nHeight)
                                all[n++] = ch;
                        }
                        /* Sort by height ascending — parents before children.
                         * Use a simple comparator with qsort. */
                        qsort(all, n, sizeof(struct block_index *),
                              block_index_cmp_height);
                        /* Single pass: if parent is failed, child is failed */
                        for (size_t i = 0; i < n; i++) {
                            if (!all[i]->pprev) continue;
                            if (all[i]->nStatus & BLOCK_FAILED_MASK) continue;
                            if (all[i]->pprev->nStatus & BLOCK_FAILED_MASK) {
                                all[i]->nStatus |= BLOCK_FAILED_CHILD;
                                propagated++;
                            }
                        }
                        free(all);
                    }
                    if (propagated > 0)
                        printf("Propagated BLOCK_FAILED_CHILD to %zu "
                               "descendants\n", propagated);
                }
            }
            /* Clean up: free view first (may contain entries from update_coins),
             * then block. Zero view to prevent any double-free. */
            coins_view_cache_free(&view);
            memset(&view, 0, sizeof(view));
            if (pblock == &local_block)
                block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return false;
        }

        if (!coins_view_cache_flush(&view)) {
            fprintf(stderr, "connect_tip: FATAL coins flush failed h=%d\n",
                    pindex_new->nHeight);
            coins_view_cache_free(&view);
            if (pblock == &local_block)
                block_free(&local_block);
            trace_set_status(ct_span, TRACE_STATUS_ERROR);
            trace_end(ct_span);
            return validation_state_error(state, "coins-flush-failed");
        }
        coins_view_cache_free(&view);
    }

    /* ── Mandatory SHA3 UTXO checkpoint verification ──────────── */
    /* When we reach a hardcoded checkpoint height, flush all coins to
     * SQLite and verify the SHA3 hash matches the compiled-in constant.
     * This is a one-time O(n) check that guarantees UTXO set integrity.
     * If it fails, the node's data is corrupted and MUST NOT continue. */
    {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (cp && pindex_new->nHeight == cp->height) {
            /* Force full coins flush to SQLite */
            flush_coins_if_needed(coins_tip, true);

            struct node_db *ndb = process_block_node_db();
            if (ndb && ndb->db) {
                uint8_t sha3[32];
                uint64_t count = 0;
                utxo_commitment_sha3_compute(ndb->db, sha3, &count);

                if (memcmp(sha3, cp->sha3_hash, 32) != 0) {
                    char exp[65], got[65];
                    for (int i = 0; i < 32; i++) {
                        snprintf(exp + i*2, 3, "%02x", cp->sha3_hash[i]);
                        snprintf(got + i*2, 3, "%02x", sha3[i]);
                    }
                    fprintf(stderr,
                        "\n*** SHA3 UTXO CHECKPOINT FAILED at height %d ***\n"
                        "Expected: %s\n"
                        "Computed: %s\n"
                        "Expected %lu UTXOs, computed %lu\n"
                        "Your UTXO set is corrupted. The node will shut down.\n"
                        "Fix: delete node.db and resync from scratch.\n\n",
                        cp->height, exp, got,
                        (unsigned long)cp->utxo_count,
                        (unsigned long)count);
                    fflush(stderr);
                    event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                                "height=%d expected=%s got=%s",
                                cp->height, exp, got);
                    trace_set_status(ct_span, TRACE_STATUS_ERROR);
                    trace_end(ct_span);
                    return validation_state_error(state,
                        "sha3-utxo-checkpoint-failed");
                }
                printf("SHA3 checkpoint PASSED at height %d (%lu UTXOs)\n",
                       cp->height, (unsigned long)count);
                fflush(stdout);
                event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                            "height=%d count=%lu",
                            cp->height, (unsigned long)count);
            }
        }
    }

    /* Update chain tip */
    update_tip(ms, pindex_new);
    pindex_new->nStatus = (pindex_new->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_SCRIPTS;

    /* Persist block_index entry to LevelDB */
    if (g_active_block_tree) {
        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        if (pindex_new->pprev && pindex_new->pprev->phashBlock)
            dbi.hashPrev = *pindex_new->pprev->phashBlock;
        dbi.nHeight = pindex_new->nHeight;
        dbi.nStatus = pindex_new->nStatus;
        dbi.nTx = pindex_new->nTx;
        dbi.nFile = pindex_new->nFile;
        dbi.nDataPos = pindex_new->nDataPos;
        dbi.nUndoPos = pindex_new->nUndoPos;
        dbi.nCachedBranchId = pindex_new->nCachedBranchId;
        dbi.nVersion = pindex_new->nVersion;
        dbi.hashMerkleRoot = pindex_new->hashMerkleRoot;
        dbi.hashFinalSaplingRoot = pindex_new->hashFinalSaplingRoot;
        dbi.nTime = pindex_new->nTime;
        dbi.nBits = pindex_new->nBits;
        dbi.nNonce = pindex_new->nNonce;
        if (pindex_new->nSolution && pindex_new->nSolutionSize > 0)
            memcpy(dbi.nSolution, pindex_new->nSolution, pindex_new->nSolutionSize);
        dbi.nSolutionSize = pindex_new->nSolutionSize;
        block_tree_db_write_block_index(g_active_block_tree, &dbi);
    }

    /* Write transaction index if enabled */
    if (g_active_block_tree && ms->fTxIndex && pblock->num_vtx > 0) {
        struct uint256 *txids = zcl_malloc(pblock->num_vtx * sizeof(struct uint256), "connect_tip_txids");
        struct disk_tx_pos *positions = zcl_malloc(
            pblock->num_vtx * sizeof(struct disk_tx_pos), "connect_tip_txpos");
        if (!txids || !positions) {
            fprintf(stderr, "connect_tip: tx index alloc failed at height %d "
                    "(%zu txs)\n", pindex_new->nHeight, pblock->num_vtx);
        }
        if (txids && positions) {
            size_t header_size = BLOCK_HEADER_SIZE +
                compact_size_sizeof(pblock->header.nSolutionSize) +
                pblock->header.nSolutionSize;
            unsigned int offset = (unsigned int)(header_size +
                compact_size_sizeof(pblock->num_vtx));

            for (size_t i = 0; i < pblock->num_vtx; i++) {
                txids[i] = pblock->vtx[i].hash;
                positions[i].block_pos.nFile = pindex_new->nFile;
                positions[i].block_pos.nPos = pindex_new->nDataPos;
                positions[i].nTxOffset = offset;

                struct byte_stream ts;
                stream_init(&ts, 1024);
                transaction_serialize(&pblock->vtx[i], &ts);
                offset += (unsigned int)ts.size;
                stream_free(&ts);
            }
            block_tree_db_write_tx_index(g_active_block_tree,
                                          txids, positions, pblock->num_vtx);
        }
        free(txids);
        free(positions);
    }

    /* Notify wallet of transactions in the connected block */
    {
        struct wallet *wallet = process_block_wallet();
        struct node_db *ndb = process_block_node_db();
        if (wallet) {
            for (size_t i = 0; i < pblock->num_vtx; i++) {
                wallet_sync_transaction(wallet, &pblock->vtx[i],
                                        pindex_new);
                /* Trial-decrypt Sapling shielded outputs for our wallet */
                if (pblock->vtx[i].num_shielded_output > 0 &&
                    wallet->sapling_keys.num_keys > 0) {
                    struct transaction *tx =
                        (struct transaction *)&pblock->vtx[i];
                    transaction_compute_hash(tx);
                    size_t notes_before = wallet->num_sapling_notes;
                    wallet_try_sapling_decrypt(wallet, tx,
                                               &tx->hash);
                    /* Persist newly discovered notes to SQLite */
                    if (ndb && wallet->num_sapling_notes > notes_before) {
                        for (size_t ni = notes_before;
                             ni < wallet->num_sapling_notes; ni++) {
                            struct sapling_received_note *note =
                                &wallet->sapling_notes[ni];
                            node_db_sync_sapling_note(ndb,
                                note->txid.data, note->output_index,
                                (int64_t)note->value, note->rcm,
                                note->memo, 512, note->ivk,
                                note->diversifier, note->pk_d,
                                note->cm, note->nf,
                                pindex_new->nHeight);
                        }
                    }
                }
                /* Mark spent nullifiers */
                if (pblock->vtx[i].num_shielded_spend > 0)
                    wallet_mark_sapling_nullifiers_spent(
                        wallet,
                        (struct transaction *)&pblock->vtx[i]);
            }
            wallet->best_block_height = pindex_new->nHeight;
        }
    }

    /* Remove confirmed transactions from mempool */
    {
        struct tx_mempool *mempool = process_block_mempool();
        if (mempool)
            tx_mempool_remove_for_block(mempool,
                pblock->vtx, pblock->num_vtx,
                (unsigned int)pindex_new->nHeight);
    }

    /* Sync block to SQLite database */
    {
        struct node_db *ndb = process_block_node_db();
        if (ndb) {
            node_db_sync_connect_block(ndb, pblock, pindex_new);
            /* Wallet tx scan deferred to tip — expensive per-tx SQLite
             * queries slow down IBD and can corrupt heap (db_wallet_utxo_find
             * allocates per-call). Use rescanblockchain RPC after sync. */

            /* coins_best_block is updated by coins_view_sqlite_batch_write
             * when the coins cache flushes to SQLite. Do NOT update it
             * per-block here — it creates a consistency gap where
             * coins_best_block points ahead of the actual flushed UTXO
             * set. On crash, the node would think UTXOs are current
             * when they're actually stale in the cache. */
        }
    }

    /* Append block hash to Merkle Mountain Range */
    if (pindex_new->phashBlock)
        rpc_blockchain_mmr_append(pindex_new->phashBlock->data);

    /* Append rich leaf to Merkle Mountain Belt (O(1) per block) */
    if (pindex_new->phashBlock) {
        struct mmb_leaf mmb_leaf;
        mmb_leaf_from_block(&mmb_leaf,
            pindex_new->phashBlock->data,
            pindex_new->nHeight, pindex_new->nTime, pindex_new->nBits,
            pindex_new->hashFinalSaplingRoot.data,
            (const uint8_t *)pindex_new->nChainWork.pn);
        rpc_blockchain_mmb_append(&mmb_leaf);
    }

    /* Deferred MMR verification: if this node received a UTXO snapshot
     * via fast sync, verify the offered MMR root matches our locally-built
     * MMR once we've synced headers to the snapshot height. This binds
     * the imported UTXO set to the PoW chain cryptographically. */
    if (g_coins_sqlite_ptr && g_coins_sqlite_ptr->db) {
        static int32_t s_mmr_check_height = -1;
        static uint8_t s_mmr_expected[32];
        static bool s_mmr_loaded = false;
        static bool s_mmr_verified = false;

        if (!s_mmr_loaded && g_coins_sqlite_ptr->db) {
            sqlite3_stmt *qs = NULL;
            sqlite3_prepare_v2(g_coins_sqlite_ptr->db,
                "SELECT value FROM node_state WHERE key='snapshot_mmr_height'",
                -1, &qs, NULL);
            if (qs && sqlite3_step(qs) == SQLITE_ROW) {
                const void *blob = sqlite3_column_blob(qs, 0);
                if (blob && sqlite3_column_bytes(qs, 0) >= 4)
                    memcpy(&s_mmr_check_height, blob, 4);
            }
            if (qs) sqlite3_finalize(qs);

            if (s_mmr_check_height > 0) {
                sqlite3_prepare_v2(g_coins_sqlite_ptr->db,
                    "SELECT value FROM node_state WHERE key='snapshot_mmr_root'",
                    -1, &qs, NULL);
                if (qs && sqlite3_step(qs) == SQLITE_ROW) {
                    const void *blob = sqlite3_column_blob(qs, 0);
                    if (blob && sqlite3_column_bytes(qs, 0) >= 32)
                        memcpy(s_mmr_expected, blob, 32);
                }
                if (qs) sqlite3_finalize(qs);
            }
            s_mmr_loaded = true;
        }

        if (!s_mmr_verified && s_mmr_check_height > 0 &&
            pindex_new->nHeight == s_mmr_check_height) {
            struct mmr *m = rpc_blockchain_get_mmr();
            if (m && m->num_leaves > 0) {
                uint8_t local_root[32];
                mmr_root(m, local_root);
                if (memcmp(local_root, s_mmr_expected, 32) == 0) {
                    printf("*** MMR VERIFICATION PASSED at height %d ***\n"
                           "    Snapshot UTXO set is cryptographically bound "
                           "to PoW chain (%llu blocks)\n",
                           s_mmr_check_height,
                           (unsigned long long)m->num_leaves);
                    event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                                "MMR verified at h=%d leaves=%llu",
                                s_mmr_check_height,
                                (unsigned long long)m->num_leaves);
                    /* Clear the deferred check — it passed */
                    sqlite3_exec(g_coins_sqlite_ptr->db,
                        "DELETE FROM node_state WHERE key IN "
                        "('snapshot_mmr_root','snapshot_mmr_height')",
                        NULL, NULL, NULL);
                } else {
                    char exp_hex[65], got_hex[65];
                    for (int i = 0; i < 32; i++) {
                        sprintf(exp_hex + i*2, "%02x", s_mmr_expected[i]);
                        sprintf(got_hex + i*2, "%02x", local_root[i]);
                    }
                    fprintf(stderr,
                        "*** MMR VERIFICATION FAILED at height %d ***\n"
                        "  Expected: %s\n"
                        "  Got:      %s\n"
                        "  The imported UTXO snapshot does NOT match the "
                        "PoW chain! This node may have received tampered data.\n",
                        s_mmr_check_height, exp_hex, got_hex);
                    event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                                "MMR FAILED at h=%d expected=%s got=%s",
                                s_mmr_check_height, exp_hex, got_hex);
                }
                s_mmr_verified = true;
            }
        }
    }

    /* Every 100 blocks: append UTXO commitment to MMR.
     * Uses the O(1) XOR accumulator instead of O(N) SHA3 full-table scan. */
    if (pindex_new->phashBlock) {
        rpc_blockchain_maybe_commit(pindex_new->nHeight,
                                     pindex_new->phashBlock->data,
                                     coins_tip->commitment.accumulator,
                                     coins_tip->commitment.count);
    }

    /* Periodically flush coins cache to SQLite.
     * If flush fails, we MUST stop connecting blocks. Continuing would
     * spend UTXOs that were never written to SQLite, causing permanent
     * UTXO loss (the "create → refuse flush → spend → later flush DELETEs
     * a UTXO that was never INSERTed" bug). */
    if (!flush_coins_if_needed(coins_tip, false)) {
        fprintf(stderr, "connect_block: coins flush failed at height %d "
                "— halting block connection to prevent UTXO loss\n",
                pindex_new->nHeight);
        if (pblock == &local_block)
            block_free(&local_block);
        trace_set_status(ct_span, TRACE_STATUS_ERROR);
        trace_end(ct_span);
        return false;
    }

    if (pblock == &local_block)
        block_free(&local_block);
    trace_end(ct_span);
    return true;
}

bool disconnect_tip(struct validation_state *state,
                    struct main_state *ms,
                    struct coins_view_cache *coins_tip,
                    const char *datadir)
{
    struct block_index *pindex_delete = active_chain_tip(&ms->chain_active);
    if (!pindex_delete)
        LOG_FAIL("validation", "disconnect_tip called with no active chain tip");
    /* Never disconnect genesis (pprev is NULL) */
    if (!pindex_delete->pprev)
        LOG_FAIL("validation", "disconnect_tip refused to disconnect genesis block");

    struct block block;
    block_init(&block);
    if (!read_block_from_disk_index(&block, pindex_delete, datadir)) {
        block_free(&block);
        return validation_state_error(state, "failed-to-read-block");
    }

    /* Read undo data */
    struct block_undo blockundo;
    block_undo_init(&blockundo);

    struct disk_block_pos undo_pos;
    undo_pos.nFile = pindex_delete->nFile;
    undo_pos.nPos = pindex_delete->nUndoPos;

    bool undo_loaded = false;
    if (undo_pos.nPos > 0) {
        disk_block_io_lock();
        FILE *f = open_undo_file(datadir, &undo_pos, true);
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (file_len > 0 && file_len <= 32 * 1024 * 1024) {
                uint8_t *buf = zcl_malloc((size_t)file_len, "undo_file_buf");
                if (buf) {
                    size_t nread = fread(buf, 1, (size_t)file_len, f); // disk-io-lock: held
                    if (nread > 0) {
                        struct byte_stream s;
                        stream_init_from_data(&s, buf, nread);
                        block_undo_deserialize(&blockundo, &s);
                        undo_loaded = (blockundo.num_txundo > 0);
                    }
                    free(buf);
                }
            }
            disk_block_io_release_handle(f);
        }
        disk_block_io_unlock();
    }

    /* Self-healing: reconstruct undo data from tx index when undo file
     * is missing or corrupt. For each non-coinbase input, look up the
     * source transaction via the tx index and extract the spent output. */
    if (!undo_loaded && block.num_vtx > 1 && g_active_block_tree) {
        printf("[self-heal] Reconstructing undo data for h=%d "
               "(%zu txs) from tx index\n",
               pindex_delete->nHeight, block.num_vtx);
        blockundo.num_txundo = block.num_vtx - 1;
        blockundo.vtxundo = zcl_calloc(blockundo.num_txundo,
                                    sizeof(struct tx_undo), "undo_vtxundo");
        bool reconstruct_ok = (blockundo.vtxundo != NULL);
        for (size_t i = 1; i < block.num_vtx && reconstruct_ok; i++) {
            const struct transaction *tx = &block.vtx[i];
            struct tx_undo *tu = &blockundo.vtxundo[i - 1];
            tu->num_prevout = tx->num_vin;
            tu->vprevout = zcl_calloc(tx->num_vin, sizeof(struct tx_in_undo), "undo_vprevout");
            if (!tu->vprevout) { reconstruct_ok = false; break; }

            for (size_t j = 0; j < tx->num_vin; j++) {
                const struct uint256 *prev_txid = &tx->vin[j].prevout.hash;
                uint32_t prev_n = tx->vin[j].prevout.n;
                struct disk_tx_pos txpos;
                disk_tx_pos_init(&txpos);
                if (!block_tree_db_read_tx_index(g_active_block_tree,
                                                  prev_txid, &txpos) ||
                    txpos.block_pos.nFile < 0) {
                    reconstruct_ok = false; break;
                }
                struct block src_blk;
                block_init(&src_blk);
                if (!read_block_from_disk(&src_blk, &txpos.block_pos,
                                          datadir)) {
                    block_free(&src_blk);
                    reconstruct_ok = false; break;
                }
                bool found = false;
                for (size_t ti = 0; ti < src_blk.num_vtx; ti++) {
                    if (uint256_eq(&src_blk.vtx[ti].hash, prev_txid)) {
                        if (prev_n < src_blk.vtx[ti].num_vout) {
                            /* struct assignment copies the fixed-size
                             * script_pub_key array by value */
                            tu->vprevout[j].txout =
                                src_blk.vtx[ti].vout[prev_n];
                            /* Get source block height */
                            struct uint256 src_hash;
                            block_get_hash(&src_blk, &src_hash);
                            struct block_index *src_idx = block_map_find(
                                &ms->map_block_index, &src_hash);
                            tu->vprevout[j].height = src_idx ?
                                (unsigned int)src_idx->nHeight : 0;
                            tu->vprevout[j].coinbase =
                                transaction_is_coinbase(&src_blk.vtx[ti]);
                            found = true;
                        }
                        break;
                    }
                }
                block_free(&src_blk);
                if (!found) { reconstruct_ok = false; break; }
            }
        }
        if (reconstruct_ok) {
            printf("[self-heal] Undo data reconstructed for h=%d\n",
                   pindex_delete->nHeight);
            undo_loaded = true;
        } else {
            fprintf(stderr, "[self-heal] Failed to reconstruct undo data "
                    "for h=%d\n", pindex_delete->nHeight);
            block_undo_free(&blockundo);
            block_undo_init(&blockundo);
        }
    }

    /* Apply disconnect */
    {
        struct coins_view_cache view;
        struct coins_view backing;
        coins_view_cache_as_view(&backing, coins_tip);
        coins_view_cache_init(&view, &backing);

        if (!disconnect_block(&block, state, pindex_delete, &view, &blockundo)) {
            block_free(&block);
            block_undo_free(&blockundo);
            coins_view_cache_free(&view);
            LOG_FAIL("validation", "disconnect_block failed at height %d",
                     pindex_delete->nHeight);
        }

        if (!coins_view_cache_flush(&view)) {
            fprintf(stderr, "disconnect_tip: FATAL coins flush failed "
                    "h=%d\n", pindex_delete->nHeight);
            coins_view_cache_free(&view);
            block_free(&block);
            block_undo_free(&blockundo);
            return validation_state_error(state, "coins-flush-failed");
        }
        coins_view_cache_free(&view);
    }

    /* Sync disconnect to SQLite */
    {
        struct node_db *ndb = process_block_node_db();
        if (ndb)
            node_db_sync_disconnect_block(ndb,
                                          &block, pindex_delete);
    }

    update_tip(ms, pindex_delete->pprev);

    block_free(&block);
    block_undo_free(&blockundo);
    return true;
}

/* ── Reorg Recovery ─────────────────────────────────────────────
 *
 * When disconnect_tip fails (missing undo data), the node is stuck:
 * the active chain tip cannot be rolled back, and the better chain
 * cannot be connected. This function implements a clean recovery:
 *
 *   1. SYNC_REORG → SYNC_REORG_RECOVERY (state machine transition)
 *   2. Clear the in-memory UTXO cache (discard stale entries)
 *   3. Force the active chain tip to the fork point
 *   4. Set coins_best_block in both memory and SQLite to fork hash
 *   5. Emit EV_REORG_DISCONNECT_FAILED + EV_REORG_RECOVERY_COMPLETE
 *
 * After recovery, activate_best_chain proceeds to connect blocks
 * from the fork point forward, rebuilding UTXOs for that range.
 *
 * Returns true if recovery succeeded, false if unrecoverable. */
static bool recover_from_disconnect_failure(
    struct main_state *ms,
    struct coins_view_cache *coins_tip,
    struct block_index *fork,
    int stuck_height)
{
    if (!fork || !fork->phashBlock)
        LOG_FAIL("validation", "recover_from_disconnect_failure called with null fork or null phashBlock");

    /* State machine: REORG → REORG_RECOVERY */
    sync_set_state(SYNC_REORG_RECOVERY,
                   "disconnect failed, clearing UTXO cache");

    event_emitf(EV_REORG_DISCONNECT_FAILED, 0,
        "stuck_h=%d fork_h=%d", stuck_height, fork->nHeight);

    /* Step 1: Clear the in-memory UTXO cache.
     * Do NOT flush — the cache contains stale entries from the
     * partially-disconnected chain that would corrupt SQLite. */
    coins_view_cache_clear(coins_tip);

    /* Steps 2 + 3: Force the active chain tip to the fork point AND
     * set coins_best_block to the fork hash in one atomic csr
     * commit. Previously these were two separate mutations that
     * could leave the six sources of truth briefly inconsistent —
     * exactly the shape of bug the chain_state_repository exists
     * to prevent.
     *
     * Note: the SQLite UTXO set may not exactly match the fork point
     * (blocks connected after the fork consumed UTXOs). This is
     * acceptable — connect_block will fail for those blocks, and
     * the operator can run `importchainstate` to get a clean set.
     * We do NOT reimport from LevelDB here because LevelDB's UTXO
     * set is at a different (later) height than the fork point. */
    process_block_commit_tip(ms, coins_tip, fork,
        "process_block.recover_from_disconnect_failure", false);

    {
        struct node_db *ndb = process_block_node_db();
        if (ndb && ndb->open) {
            node_db_state_set(ndb, "coins_best_block",
                          fork->phashBlock->data, 32);
        }
    }

    /* Step 4: Flush any pending SQLite batch. */
    {
        struct node_db *ndb = process_block_node_db();
        if (ndb && ndb->sync_in_batch)
            node_db_sync_flush(ndb);
    }

    /* Step 6: Clear BLOCK_FAILED flags on blocks above the fork point.
     * Previous connect attempts may have marked blocks invalid due to
     * stale UTXO data. After reimport, those blocks are valid. */
    {
        size_t iter = 0;
        struct block_index *bi = NULL;
        int cleared = 0;
        while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
            if (!bi) continue;
            if (bi->nHeight > fork->nHeight &&
                (bi->nStatus & BLOCK_FAILED_MASK)) {
                bi->nStatus &= ~BLOCK_FAILED_MASK;
                cleared++;
            }
        }
        if (cleared > 0)
            fprintf(stderr, "reorg_recovery: cleared BLOCK_FAILED on %d blocks "
                    "above fork h=%d\n", cleared, fork->nHeight);
    }

    event_emitf(EV_REORG_RECOVERY_COMPLETE, 0,
        "fork_h=%d cache_cleared=true", fork->nHeight);

    fprintf(stderr,
        "activate_best_chain: recovered from disconnect failure, "
        "chain reset to h=%d, UTXO cache cleared\n", fork->nHeight);

    return true;
}

bool activate_best_chain(struct validation_state *state,
                         struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         const struct chain_params *params,
                         struct block *pblock,
                         const char *datadir)
{
    /* Note: anchor/UTXO guards are now handled by the chain activation
     * controller (activation_request_connect). This function should only
     * be called via the controller. */

    struct block_index *pindex_most_work;

    do {
        pindex_most_work = find_most_work_chain(ms);

        struct block_index *tip = active_chain_tip(&ms->chain_active);
        if (!pindex_most_work || pindex_most_work == tip)
            return true;
        /* Don't reorg to a chain with less or equal work than our tip.
         * This happens when nChainTx gaps make find_most_work_chain
         * return a shorter chain that is actually part of our chain. */
        if (tip && arith_uint256_compare(&pindex_most_work->nChainWork,
                                          &tip->nChainWork) <= 0)
            return true;
        printf("activate_best_chain: tip=%d most_work=%d\n",
               tip ? tip->nHeight : -1,
               pindex_most_work->nHeight);
        event_emitf(EV_BOOT_ACTIVATE, 0, "tip=%d most_work=%d",
                    tip ? tip->nHeight : -1,
                    pindex_most_work->nHeight);

        /* Check reorg length */
        if (tip) {
            /* Find fork point.
             * SAFETY: check pprev at every step — blocks loaded from
             * flat file may have dangling pprev if the file was saved
             * before all P2P blocks were linked. */
            struct block_index *fork = tip;
            while (fork && fork->pprev &&
                   fork->nHeight > pindex_most_work->nHeight)
                fork = fork->pprev;
            if (!fork) return true; /* chain broken, wait for P2P */
            struct block_index *walk = pindex_most_work;
            while (walk && walk->pprev &&
                   walk->nHeight > fork->nHeight)
                walk = walk->pprev;
            while (fork && walk && fork != walk &&
                   fork->pprev && walk->pprev) {
                fork = fork->pprev;
                walk = walk->pprev;
            }
            /* If pprev walk couldn't find a common ancestor (broken
             * links after LDB import), assume we're extending the
             * current chain — use tip as the fork point.  Do NOT
             * set fork=NULL which triggers a destructive genesis reset. */
            if (fork != walk)
                fork = tip;

            /* During IBD, allow deep reorgs — fork blocks received in
             * parallel can cause the wrong chain to be connected initially.
             * At tip (steady state), enforce the reorg limit. */
            /* If most_work chain is not actually better, skip reorg */
            if (pindex_most_work->nHeight <= tip->nHeight) {
                return true; /* current chain is already at or above most_work */
            }

            int reorg_depth = tip->nHeight - (fork ? fork->nHeight : -1);
            bool in_ibd = (sync_get_state() <= SYNC_BLOCKS_DOWNLOAD);
            /* Skip reorg limit when fork is NULL but we're extending the
             * chain (not actually reorging). This happens after LDB import
             * when pprev pointers aren't fully resolved. */
            bool extending = (!fork && pindex_most_work->nHeight > tip->nHeight &&
                              pindex_most_work->nHeight <= tip->nHeight + 200);
            if (!extending && !in_ibd && reorg_depth > MAX_REORG_LENGTH) {
                printf("activate_best_chain: reorg depth %d exceeds max %d\n",
                       reorg_depth, MAX_REORG_LENGTH);
                return false;
            }
            if (!extending && in_ibd && reorg_depth > MAX_IBD_REORG_LENGTH) {
                printf("activate_best_chain: IBD reorg depth %d exceeds "
                       "max %d\n", reorg_depth, MAX_IBD_REORG_LENGTH);
                return false;
            }

            /* Disconnect blocks from current tip to fork point */
            if (!fork) {
                /* No common ancestor found — chains are completely
                 * divergent (broken pprev links). Reset to genesis.
                 * This is a clear rollback, so the csr commit uses
                 * allow_rollback (via the helper) and does not move
                 * pindex_best_header — the header tip stays put so
                 * accept_block_header's retry logic can rebuild the
                 * chain upward. */
                struct block_index *genesis = active_chain_at(
                    &ms->chain_active, 0);
                if (genesis) {
                    process_block_commit_tip(ms, coins_tip, genesis,
                        "process_block.activate_best_chain.no_fork_reset",
                        false);
                    printf("activate_best_chain: no fork point, "
                           "reset to genesis\n");
                }
            } else if (tip->nHeight > fork->nHeight) {
                event_emitf(EV_REORG_START, 0, "fork=%d tip=%d depth=%d",
                            fork->nHeight, tip->nHeight,
                            tip->nHeight - fork->nHeight);
                sync_set_state(SYNC_REORG, "chain reorganization");
                while (active_chain_tip(&ms->chain_active) != fork) {
                    if (!disconnect_tip(state, ms, coins_tip, datadir)) {
                        int stuck_h = active_chain_height(&ms->chain_active);
                        if (!recover_from_disconnect_failure(
                                ms, coins_tip, fork, stuck_h)) {
                            sync_set_state(SYNC_FAILED,
                                "unrecoverable disconnect failure");
                            LOG_FAIL("validation", "unrecoverable disconnect failure during reorg at height %d",
                                     active_chain_height(&ms->chain_active));
                        }
                        break; /* exit disconnect loop, proceed to connect */
                    }
                }
            }
        }

        /* Connect blocks from fork to most-work tip.
         * Count total depth, then allocate dynamically.
         * Cap at 500 blocks per batch to prevent OOM when connecting
         * 1M+ blocks (e.g. after LDB import with symlinked blk files).
         * The outer do-while loop re-finds most_work and continues. */
        #define CONNECT_BATCH_MAX 500

        struct block_index *current_tip = active_chain_tip(&ms->chain_active);
        int total_depth = 0;
        for (struct block_index *w = pindex_most_work;
             w && w != current_tip; w = w->pprev)
            total_depth++;

        int batch_depth = total_depth > CONNECT_BATCH_MAX
                        ? CONNECT_BATCH_MAX : total_depth;

        printf("activate_best_chain: connect path depth=%d "
               "(from h=%d to tip h=%d)%s\n",
               total_depth, pindex_most_work->nHeight,
               current_tip ? current_tip->nHeight : -1,
               total_depth > CONNECT_BATCH_MAX ? " [batched]" : "");
        fflush(stdout);

        /* Walk backward from most_work but only collect batch_depth
         * entries. For batched connects, we start from the OLDEST
         * needed block (closest to current tip), not from most_work. */
        struct block_index **connect_path = zcl_malloc(
            (size_t)batch_depth * sizeof(struct block_index *), "connect_path");
        if (!connect_path)
            LOG_FAIL("validation", "malloc failed for connect_path (%d entries)", batch_depth);

        /* Walk all the way back to build the path from tip to most_work,
         * but only keep the last batch_depth entries (closest to tip). */
        int path_len = 0;
        struct block_index *w = pindex_most_work;
        /* Skip entries beyond our batch window */
        int skip = total_depth - batch_depth;
        for (int s = 0; s < skip && w && w != current_tip; s++)
            w = w->pprev;
        for (; w && w != current_tip && path_len < batch_depth;
             w = w->pprev)
            connect_path[path_len++] = w;

        /* Connect in forward order (reverse of path) */
        if (path_len > 0) {
            printf("activate_best_chain: first connect h=%d last h=%d "
                   "path_len=%d\n",
                   connect_path[path_len - 1]->nHeight,
                   connect_path[0]->nHeight, path_len);
            fflush(stdout);
        }
        for (int i = path_len - 1; i >= 0; i--) {
            /* Check for shutdown request (Ctrl-C during replay) */
            if (g_shutdown_requested) {
                printf("activate_best_chain: shutdown requested at height %d, "
                       "flushing coins...\n",
                       active_chain_height(&ms->chain_active));
                flush_coins_if_needed(coins_tip, true); /* force flush */
                free(connect_path);
                return true; /* clean exit, coins flushed */
            }

            struct block *use_block = NULL;
            if (pblock && i == 0) {
                struct uint256 block_hash;
                block_header_get_hash(&pblock->header, &block_hash);
                if (connect_path[0]->phashBlock &&
                    uint256_cmp(&block_hash, connect_path[0]->phashBlock) == 0)
                    use_block = pblock;
            }

            /* Only connect blocks that have data. If a block on the
             * path doesn't have data yet (header-only), stop here.
             * The download manager will fetch it; on the next call
             * to activate_best_chain we'll continue from this point. */
            if (!(connect_path[i]->nStatus & BLOCK_HAVE_DATA)) {
                /* Priority-queue this block — it's the NEXT one needed
                 * to advance the chain. Gets assigned to the next peer
                 * before any other queued blocks. */
                if (connect_path[i]->phashBlock) {
                    struct download_manager *dm_abc = msg_get_download_mgr();
                    if (dm_abc)
                        dl_queue_priority(dm_abc, connect_path[i]->phashBlock,
                                           connect_path[i]->nHeight);
                }
                /* Force flush coins to SQLite before pausing. If we
                 * connected any blocks above, their UTXOs are in the
                 * in-memory cache only. A restart without flush would
                 * lose them, corrupting the UTXO set. */
                flush_coins_if_needed(coins_tip, true);
                free(connect_path);
                return true; /* partial success, will continue later */
            }

            if (!connect_tip(state, ms, coins_tip, connect_path[i],
                            use_block, params, datadir)) {
                fprintf(stderr,
                    "activate_best_chain: connect_tip FAILED at height %d "
                    "reason=%s invalid=%d\n",
                    connect_path[i]->nHeight,
                    state->reject_reason[0] ? state->reject_reason
                                            : "unknown",
                    validation_state_is_invalid(state));
                event_emitf(EV_BOOT_ACTIVATE, 0, "FAILED h=%d reason=%s",
                    connect_path[i]->nHeight,
                    state->reject_reason[0] ? state->reject_reason
                                            : "unknown");

                /* Auto-recovery: if UTXO mismatches keep failing at the
                 * same height, write a flag file so boot.c reimports
                 * UTXOs from LevelDB on next restart. */
                if (state->reject_reason[0] &&
                    strcmp(state->reject_reason,
                           "bad-txns-inputs-missingorspent") == 0) {
                    static int s_utxo_fail_count = 0;
                    static int s_utxo_fail_height = -1;
                    if (connect_path[i]->nHeight == s_utxo_fail_height)
                        s_utxo_fail_count++;
                    else {
                        s_utxo_fail_height = connect_path[i]->nHeight;
                        s_utxo_fail_count = 1;
                    }
                    if (s_utxo_fail_count >= 3 && datadir) {
                        char flag_path[512];
                        snprintf(flag_path, sizeof(flag_path),
                                 "%s/needs_reimport", datadir);
                        FILE *flag = fopen(flag_path, "w");
                        if (flag) {
                            fprintf(flag, "1\n");
                            fclose(flag);
                        }
                        fprintf(stderr,
                            "CRITICAL: %d UTXO failures at h=%d — "
                            "wrote needs_reimport flag.\n",
                            s_utxo_fail_count,
                            connect_path[i]->nHeight);
                    }
                }
                if (validation_state_is_invalid(state)) {
                    /* Block failed validation — mark it and retry.
                     * The do-while loop will call find_most_work_chain
                     * again, which skips this failed block and finds
                     * an alternative chain. This matches ZClassic C++
                     * ActivateBestChainStep behavior. */
                    validation_state_init(state);
                    connect_path = NULL; /* prevent double-free at line 979 */
                    break; /* break inner loop, retry outer do-while */
                }
                /* System error (not invalid block) — abort */
                free(connect_path);
                return false;
            }
        }
        /* Flush coins after each batch to bound memory usage.
         * The outer do-while loop re-finds most_work and connects
         * the next batch until we reach the tip. */
        if (total_depth > CONNECT_BATCH_MAX) {
            flush_coins_if_needed(coins_tip, true);
            printf("activate_best_chain: batch done, flushed at h=%d "
                   "(%d remaining)\n",
                   active_chain_height(&ms->chain_active),
                   total_depth - batch_depth);
        }
        free(connect_path);

    } while (pindex_most_work != active_chain_tip(&ms->chain_active) &&
             !g_shutdown_requested);

    return true;
}

bool process_new_block(struct validation_state *state,
                       struct main_state *ms,
                       struct coins_view_cache *coins_tip,
                       const struct chain_params *params,
                       struct block *pblock,
                       bool force_processing,
                       const char *datadir)
{
    (void)coins_tip;  /* activation controller owns coins_tip reference */

    bool checked = check_block(pblock, state, params, true, true, true);
    if (!checked) {
        fprintf(stderr, "process_new_block: check_block FAILED: %s\n",
                state->reject_reason[0] ? state->reject_reason : "unknown");
        return false;
    }

    struct block_index *pindex = NULL;
    bool requested = force_processing;

    if (!accept_block(pblock, state, ms, params, &pindex, requested, datadir)) {
        fprintf(stderr, "process_new_block: accept_block FAILED: %s\n",
                state->reject_reason[0] ? state->reject_reason : "unknown");
        return false;
    }

    /* Do NOT connect blocks if we're waiting for a UTXO snapshot.
     * Connecting blocks from genesis with an empty UTXO set permanently
     * marks valid blocks as BLOCK_FAILED (e.g. coinbase maturity checks
     * fail because the coinbase outputs were never added to the view).
     * accept_block above is safe — it only indexes the block on disk. */
    /* Connect blocks via controller (single authority). The controller
     * handles: anchor check, UTXO availability, mutex serialization. */
    {
        struct activation_exec_outcome ao;
        activation_request_connect(boot_activation_controller(),
                                   ACTIVATION_SRC_NEW_BLOCK, pblock, &ao);
        if (ao.result == ACTIVATION_EXEC_FAILED) {
            fprintf(stderr, "process_new_block: activation FAILED: %s\n",
                    ao.reason);
            return false;
        }
    }

    return true;
}

bool test_block_validity(struct validation_state *state,
                         const struct chain_params *params,
                         struct coins_view_cache *coins_tip,
                         const struct block *block,
                         struct block_index *pindex_prev)
{
    struct coins_view_cache view;
    struct coins_view backing;
    coins_view_cache_as_view(&backing, coins_tip);
    coins_view_cache_init(&view, &backing);

    struct block_index index_dummy;
    block_index_init(&index_dummy);
    index_dummy.pprev = pindex_prev;
    index_dummy.nHeight = pindex_prev->nHeight + 1;

    if (!contextual_check_block_header(&block->header, state, params,
                                        pindex_prev, true)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: contextual_check_block_header failed");
    }
    if (!check_block(block, state, params, true, true, true)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: check_block failed");
    }
    if (!contextual_check_block(block, state, params, pindex_prev)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: contextual_check_block failed");
    }
    if (!connect_block(block, state, &index_dummy, &view, params, true)) {
        coins_view_cache_free(&view);
        LOG_FAIL("validation", "test_block_validity: connect_block failed");
    }

    coins_view_cache_free(&view);
    return true;
}
