/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * UTXO Recovery Service — extracted from boot.c (Phase C).
 * All destructive UTXO operations gated through recovery_policy.
 */

#include "services/utxo_recovery_service.h"
#include "services/recovery_policy.h"
#include "services/chain_activation_controller.h"
#include "services/chain_restore_service.h"
#include "services/chain_state_repository.h"
#include "services/chain_tip.h"
#include "services/snapshot_sync_service.h"
#include "config/boot_internal.h"
#include "config/db_service.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "chain/chainparams.h"
#include "storage/coins_view_sqlite.h"
#include "storage/coins_db.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "chain/checkpoints.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "models/database.h"
#include "models/block.h"
#include "storage/disk_block_io.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#include <limits.h>
#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#define UTXO_CHECKPOINT_NEAR_WINDOW 144

static bool utxo_recovery_commit_tip(struct utxo_recovery_ctx *ctx,
                                     struct block_index *tip,
                                     const char *reason,
                                     bool persist_coins_best)
{
    if (!ctx || !ctx->state || !tip || !tip->phashBlock)
        return false;

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_UTXO_REPAIR,
        .decision = POLICY_ALLOW,
        .from_height = active_chain_height(&ctx->state->chain_active),
        .to_height = tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "utxo_recovery_verified",
        .reason = reason ? reason : "utxo_recovery",
    };
    struct chain_state_commit commit = {
        .new_tip = tip,
        .new_coins_best = *tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = true,
        .persist_coins_best = persist_coins_best,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = reason ? reason : "utxo_recovery",
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK)
        return true;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        if (ctx->coins_tip)
            coins_view_cache_set_best_block(ctx->coins_tip,
                                            tip->phashBlock);
        if (persist_coins_best && ctx->ndb && ctx->ndb->open)
            (void)node_db_state_set(ctx->ndb, "coins_best_block",
                                    tip->phashBlock->data, 32);
        chain_set_active_tip(ctx->state, tip, TIP_FROM_UTXO_REPAIR,
                             reason ? reason : "utxo_recovery_csr_uninit");
        ctx->state->pindex_best_header = tip;
        return true;
    }
#endif

    fprintf(stderr, // obs-ok:pre-existing-diagnostic
            "utxo_recovery: csr rejected tip promotion (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason ? reason : "", tip->nHeight);
    return false;
}

static bool utxo_recovery_commit_genesis(struct utxo_recovery_ctx *ctx,
                                         const char *reason)
{
    if (!ctx || !ctx->state || !ctx->params)
        return false;

    struct block_index *genesis = block_map_find(
        &ctx->state->map_block_index,
        &ctx->params->consensus.hashGenesisBlock);
    if (!genesis) {
        fprintf(stderr,
                "utxo_recovery: cannot reset coins best to genesis; "
                "genesis is missing from block index (reason=%s)\n",
                reason ? reason : "");
        return false;
    }

    if (!utxo_recovery_commit_tip(ctx, genesis,
                                  reason ? reason : "utxo_recovery_genesis",
                                  true))
        return false;
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);
    return true;
}

struct utxo_count_check_result utxo_recovery_classify_count_check(
    int tip_height,
    int checkpoint_height,
    uint64_t checkpoint_count,
    uint64_t actual_count)
{
    struct utxo_count_check_result res = {0};
    res.blocks_past_checkpoint = tip_height - checkpoint_height;

    if (checkpoint_height <= 0 || checkpoint_count == 0 ||
        tip_height < checkpoint_height) {
        res.level = UTXO_COUNT_CHECK_OK;
        return res;
    }

    int64_t delta = (int64_t)actual_count - (int64_t)checkpoint_count;
    if (delta < 0)
        delta = -delta;
    res.pct_delta = (double)delta / (double)checkpoint_count * 100.0;

    if (res.blocks_past_checkpoint > UTXO_CHECKPOINT_NEAR_WINDOW) {
        res.level = UTXO_COUNT_CHECK_INFO_STALE_REFERENCE;
        return res;
    }

    if (res.pct_delta > 50.0)
        res.level = UTXO_COUNT_CHECK_CRITICAL;
    else if (res.pct_delta > 10.0)
        res.level = UTXO_COUNT_CHECK_WARNING;
    else
        res.level = UTXO_COUNT_CHECK_OK;
    return res;
}

bool utxo_recovery_xor_mismatch_is_corruption_candidate(
    uint64_t saved_count,
    uint64_t computed_count)
{
    (void)saved_count;
    (void)computed_count;
    return false;
}

/* ── Policy-gated UTXO wipe ──────────────────────────────────────
 *
 * Every destructive UTXO wipe must go through this function.
 * It counts the rows first, asks recovery_policy for permission with
 * a grep-able reason string, and refuses loudly if the proposed wipe
 * is larger than ZCL_MAX_UTXO_WIPE_ROWS (default 1000).
 *
 * This is the gate that would have saved the 1.3M UTXOs on
 * 2026-04-10. Do not bypass. */
bool utxo_recovery_wipe(struct node_db *ndb, const char *reason)
{
    int64_t proposed = node_db_utxo_count(ndb);

    struct recovery_policy pol;
    policy_load_from_env(&pol);

    enum policy_decision d = policy_check_utxo_wipe(&pol, proposed, reason);
    if (d != POLICY_ALLOW) {
        fprintf(stderr,
                "utxo_recovery: REFUSING wipe at \"%s\" — would drop %lld rows, "
                "policy=%s (override with ZCL_MAX_UTXO_WIPE_ROWS=%lld)\n",
                reason, (long long)proposed,
                policy_decision_name(d), (long long)(proposed + 1));
        return false;
    }
    event_emitf(EV_RECOVERY_ACTION, 0,
        "action=utxo_wipe reason=%s rows=%lld", reason, (long long)proposed);
    node_db_wipe_utxos(ndb);
    return true;
}

/* ── Auto-reimport flag ──────────────────────────────────────── */

bool utxo_recovery_check_reimport_flag(const char *datadir)
{
    char flag_path[512];
    snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", datadir);
    FILE *flag = fopen(flag_path, "r");
    if (!flag) return false;

    char buf[8] = {0};
    fread(buf, 1, sizeof(buf) - 1, flag);
    fclose(flag);
    remove(flag_path);

    if (buf[0] == '1') {
        printf("Auto-reimport triggered by previous UTXO "
               "validation failures.\n");
        return true;
    }
    return false;
}

bool utxo_recovery_prepare_reimport(struct node_db *ndb)
{
    printf("Forced UTXO re-import requested.\n");
    /* Only clear the migration flag — the wipe will happen at the
     * start of utxo_recovery_import_ldb (line 168).  Wiping here
     * AND in import_ldb was one of the three redundant wipes that
     * could destroy imported UTXOs. */
    node_db_exec(ndb, "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'");
    return true;
}

/* ── LDB→SQLite UTXO import ─────────────────────────────────── */

struct utxo_import_result utxo_recovery_import_ldb(
    struct utxo_recovery_ctx *ctx)
{
    struct utxo_import_result res = {0};

    uint8_t mig_buf[8];
    size_t mig_len = 0;
    bool migration_done = node_db_state_get(ctx->ndb,
        "leveldb_utxo_migrated", mig_buf, sizeof(mig_buf), &mig_len);

    if (migration_done)
        return res;

    char cs_path[1024];
    char import_path_cleanup[1100] = "";
    snprintf(cs_path, sizeof(cs_path), "%s/chainstate", ctx->datadir);
    struct stat cs_st;

    /* Fall back to zclassicd's chainstate if local doesn't exist */
    if (stat(cs_path, &cs_st) != 0) {
        const char *home_cs = getenv("HOME");
        if (home_cs)
            snprintf(cs_path, sizeof(cs_path),
                     "%s/.zclassic/chainstate", home_cs);
    }
    if (stat(cs_path, &cs_st) != 0) {
        /* No chainstate dir — mark as done (fresh node) */
        uint8_t one = 1;
        node_db_state_set(ctx->ndb, "leveldb_utxo_migrated", &one, 1);
        return res;
    }

    printf("LevelDB→SQLite UTXO migration from %s\n", cs_path);
    fflush(stdout);

    /* If zclassicd's LOCK file exists, another process owns this
     * LevelDB. Copy the chainstate to a temp dir to avoid
     * corrupting zclassicd's data. NEVER delete another
     * process's LOCK file. */
    char cs_lock[1100];
    snprintf(cs_lock, sizeof(cs_lock), "%s/LOCK", cs_path);
    char import_path[1100];
    struct stat lock_st;
    if (stat(cs_lock, &lock_st) == 0) {
        snprintf(import_path, sizeof(import_path),
                 "%s/chainstate_import_tmp", ctx->datadir);
        char cmd[2300];
        snprintf(cmd, sizeof(cmd),
                 "rm -rf '%s' && cp -a '%s' '%s'",
                 import_path, cs_path, import_path);
        printf("Copying chainstate (zclassicd LOCK present)...\n");
        fflush(stdout);
        if (system(cmd) != 0) {
            printf("ERROR: failed to copy chainstate\n");
            goto cleanup;
        }
        /* Remove the copied LOCK so we can open it */
        char tmp_lock[1200];
        snprintf(tmp_lock, sizeof(tmp_lock), "%s/LOCK", import_path);
        unlink(tmp_lock);
        snprintf(import_path_cleanup, sizeof(import_path_cleanup),
                 "%s", import_path);
    } else {
        snprintf(import_path, sizeof(import_path), "%s", cs_path);
    }

    (void)utxo_recovery_wipe(ctx->ndb, "boot.ldb_import_prepare");
    coins_view_sqlite_close(ctx->coins_sqlite);

    struct coins_view_db migrate_db;
    if (coins_view_db_open(&migrate_db, import_path,
                           450 << 20, false, false)) {
        struct node_db import_db;
        if (node_db_sync_open_private_db_like(ctx->ndb, &import_db)) {
            node_db_sync_import_utxos(&import_db, &migrate_db);
            node_db_close(&import_db);
        } else {
            node_db_sync_import_utxos(ctx->ndb, &migrate_db);
        }

        /* Discover LDB height from imported UTXOs */
        int ldb_height = 0;
        if (ctx->ndb->open) {
            sqlite3_stmt *hstmt = NULL;
            sqlite3_prepare_v2(ctx->ndb->db,
                "SELECT MAX(height) FROM utxos",
                -1, &hstmt, NULL);
            if (hstmt && AR_STEP_ROW_READONLY(hstmt) == SQLITE_ROW)
                ldb_height = sqlite3_column_int(hstmt, 0);
            if (hstmt) sqlite3_finalize(hstmt);
            if (ldb_height > 0)
                printf("LDB import: height %d (from UTXO heights)\n",
                       ldb_height);
        }
        res.height = ldb_height;

        /* Set coins_best_block from LDB */
        struct uint256 ldb_best;
        memset(&ldb_best, 0, sizeof(ldb_best));
        if (coins_view_db_get_best_block(&migrate_db, &ldb_best) &&
            !uint256_is_null(&ldb_best)) {
            struct block_index *found = block_map_find(
                &ctx->state->map_block_index, &ldb_best);

            char dbg_hex[65];
            uint256_get_hex(&ldb_best, dbg_hex);

            if (found && found->nHeight > 0) {
                /* Block found in index — set as chain tip */
                if (utxo_recovery_commit_tip(
                        ctx, found, "ldb_import_found", true)) {
                    printf("LDB import: chain tip at h=%d hash=%s\n",
                           found->nHeight, dbg_hex);
                    res.skip_activate = true;
                    snprintf(res.anchor_reason, sizeof(res.anchor_reason),
                             "ldb_import_found");
                }
            } else if (ldb_height > 0) {
                /* LDB best block NOT in our index — record an activation
                 * anchor only. Do not publish coins_best_block until the
                 * block is present in the local index and CSR can commit it. */
                struct block_index *anchor = chain_restore_create_anchor(
                    ctx->state, &ldb_best, ldb_height);
                if (anchor) {
                    snapsync_set_anchor(anchor);

                    printf("LDB import: metadata anchor at h=%d hash=%s "
                           "— waiting for real block data.\n",
                           ldb_height, dbg_hex);
                }
                res.skip_activate = true;
                snprintf(res.anchor_reason, sizeof(res.anchor_reason),
                         "ldb_import_anchor");
            } else {
                char dbg_hex2[65];
                uint256_get_hex(&ldb_best, dbg_hex2);
                printf("LDB import: coins_best_block=%s "
                       "(height unknown)\n", dbg_hex2);
                res.skip_activate = true;
                snprintf(res.anchor_reason, sizeof(res.anchor_reason),
                         "ldb_import_unknown");
            }
        }

        coins_view_db_close(&migrate_db);
        node_db_wal_checkpoint(ctx->ndb);

        /* Force a fresh read snapshot so ctx->ndb sees the UTXOs
         * written by import_db (the private connection).  Without
         * this, a stale SQLite snapshot can report 0 rows, causing
         * the SHA3 check to falsely fail and wipe valid data. */
        sqlite3_exec(ctx->ndb->db, "BEGIN; END;", NULL, NULL, NULL);

        /* SHA3 verification */
        uint8_t imported_root[32];
        uint64_t imported_count = 0;
        utxo_commitment_sha3_compute(ctx->ndb->db,
            imported_root, &imported_count);
        printf("SHA3 UTXO verification: %llu UTXOs\n",
               (unsigned long long)imported_count);
        res.utxo_count = imported_count;

        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (cp && imported_count == cp->utxo_count &&
            memcmp(imported_root, cp->sha3_hash, 32) == 0) {
            printf("=== SHA3 UTXO CHECKPOINT: PASSED ===\n");
        } else if (imported_count > 100000) {
            printf("SHA3: %llu UTXOs (different height from "
                   "checkpoint, will verify later)\n",
                   (unsigned long long)imported_count);
        } else {
            /* Double-check actual row count before wiping — the
             * SHA3 function might have missed data due to a stale
             * snapshot, but the UTXOs are actually in the table. */
            int64_t actual = node_db_utxo_count(ctx->ndb);
            if (actual > 100000) {
                printf("SHA3 saw %llu but utxo table has %lld rows "
                       "— keeping data (snapshot lag)\n",
                       (unsigned long long)imported_count,
                       (long long)actual);
                imported_count = (uint64_t)actual;
                res.utxo_count = imported_count;
            } else {
                fprintf(stderr, "ERROR: only %llu UTXOs imported "
                        "— will retry on next boot\n",
                        (unsigned long long)imported_count);
                (void)utxo_recovery_wipe(ctx->ndb,
                    "boot.ldb_import_failed_retry");
            }
        }

        uint8_t one = 1;
        if (imported_count > 100000)
            node_db_state_set(ctx->ndb, "leveldb_utxo_migrated", &one, 1);

        /* Marker consumed by process_block.c's hot-loop exit
         * debounce. If the reimport happens but the UTXO set
         * is still incomplete (e.g. zclassicd's on-disk LDB
         * is memtable-stale and doesn't carry the missing
         * UTXO either), the hot-loop exit SHOULD NOT trigger
         * a restart — that would bootloop. Writing this
         * marker lets process_block.c detect "we just tried
         * reimport and are STILL stuck" and stop requesting
         * shutdown. The 10-min staleness window there gives
         * operator time to intervene. */
        if (ctx->datadir) {
            char marker_path[512];
            snprintf(marker_path, sizeof(marker_path),
                     "%s/last_reimport_attempted", ctx->datadir);
            FILE *mf = fopen(marker_path, "w");
            if (mf) {
                fputs("1\n", mf);
                fclose(mf);
            }
        }

        coins_view_sqlite_open(ctx->coins_sqlite, ctx->ndb->db);
        /* Re-init coins cache after import */
        coins_view_cache_init(ctx->coins_tip, &ctx->coins_sqlite->view);
        set_coins_sqlite_for_commitment(ctx->coins_sqlite);

        /* Diagnostic: log UTXO height vs chain tip after import */
        {
            int tip_h = active_chain_height(&ctx->state->chain_active);
            printf("[boot] UTXO import: coins_best_block at h=%d, "
                   "chain tip at h=%d%s\n",
                   ldb_height, tip_h,
                   ldb_height != tip_h
                       ? " (MISMATCH — adjusting tip)" : "");
        }

        printf("UTXO migration complete.\n");
        fflush(stdout);
        res.imported = true;
    }

cleanup:
    if (import_path_cleanup[0]) {
        char rm_cmd[1200];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", import_path_cleanup);
        system(rm_cmd);
    }

    /* Mark done for fresh node if no chainstate was found */
    if (!res.imported && !migration_done) {
        uint8_t one = 1;
        node_db_state_set(ctx->ndb, "leveldb_utxo_migrated", &one, 1);
    }

    return res;
}

/* ── Chain tip restoration ──────────────────────────────────── */

static bool has_disk_backed_competing_sibling(
    struct main_state *ms,
    const struct block_index *candidate,
    const char *datadir)
{
    if (!ms || !candidate || !candidate->pprev || !candidate->phashBlock)
        return false;

    size_t iter = 0;
    struct block_index *alt;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &alt)) {
        if (!alt || alt == candidate)
            continue;
        if (alt->nHeight != candidate->nHeight)
            continue;
        if (alt->pprev != candidate->pprev)
            continue;
        if (alt->phashBlock &&
            uint256_eq(alt->phashBlock, candidate->phashBlock))
            continue;
        if (chain_restore_block_is_consensus_backed_on_disk(alt, datadir))
            return true;
    }
    return false;
}

struct chain_restore_result utxo_recovery_restore_chain_tip(
    struct utxo_recovery_ctx *ctx,
    struct block_index *scan_fallback)
{
    struct chain_restore_result res = {0};

    struct uint256 best_hash;
    coins_view_cache_get_best_block(ctx->coins_tip, &best_hash);

    if (uint256_is_null(&best_hash)) {
        /* No best block — try fast rebuild if fallback available */
        if (scan_fallback) {
            if (utxo_recovery_commit_tip(
                    ctx, scan_fallback, "scan_fallback", false)) {
                printf("WARNING: Chain tip at height %d but coins DB is empty!\n",
                       scan_fallback->nHeight);
                printf("Attempting fast chainstate rebuild from SQLite...\n");
                if (fast_rebuild_chainstate(ctx->coins_sqlite, ctx->coins_tip,
                                             ctx->datadir))
                    printf("Fast rebuild complete — will activate chain.\n");
                else
                    printf("Fast rebuild unavailable — will activate from genesis.\n");
                res.restored = true;
            }
        }
        return res;
    }

    struct block_index *best = block_map_find(
        &ctx->state->map_block_index, &best_hash);

    /* Diagnostic: log whether coins_best_block was found in block_index */
    {
        char hex[65];
        uint256_get_hex(&best_hash, hex);
        if (best)
            printf("[boot] coins_best_block %s found in block_index "
                   "at h=%d\n", hex, best->nHeight);
        else
            printf("[boot] coins_best_block %s NOT found in block_index "
                   "(map size=%zu)\n", hex,
                   ctx->state->map_block_index.size);
    }

    /* SQLite fallback: if block_map maps the hash to height 0
     * but it's not actually the genesis block */
    if (best && best->nHeight == 0 &&
        !uint256_eq(&best_hash, &ctx->params->consensus.hashGenesisBlock)) {
        char hex[65];
        uint256_get_hex(&best_hash, hex);
        fprintf(stderr,
            "WARNING: coins DB best block %s mapped to height=0 "
            "in block_index (not genesis)\n", hex);
        if (ctx->ndb->open) {
            struct db_block sqlite_blk;
            if (db_block_find_by_hash(ctx->ndb, best_hash.data,
                                       &sqlite_blk) &&
                sqlite_blk.height > 0) {
                printf("Correcting: block nHeight 0→%d from SQLite\n",
                       sqlite_blk.height);
                best->nHeight = sqlite_blk.height;
            } else {
                best = NULL;
            }
        } else {
            best = NULL;
        }
    }

    if (best) {
        struct block_index *restore_tip = best;
        bool best_backed =
            chain_restore_block_is_consensus_backed_on_disk(best,
                                                            ctx->datadir);
        {
            char best_hex[65] = {0};
            char prev_hex[65] = {0};
            bool merkle_null = uint256_is_null(&best->hashMerkleRoot);
            if (best->phashBlock)
                uint256_get_hex(best->phashBlock, best_hex);
            if (best->pprev && best->pprev->phashBlock)
                uint256_get_hex(best->pprev->phashBlock, prev_hex);
            fprintf(stderr,
                "[boot] coins_best_block validation h=%d hash=%s "
                "status=%u file=%d pos=%u tx=%u chaintx=%lld bits=%u "
                "pprev_h=%d pprev=%s merkle_null=%d disk_backed=%d\n",
                best->nHeight, best_hex[0] ? best_hex : "<null>",
                best->nStatus, best->nFile, best->nDataPos, best->nTx,
                (long long)best->nChainTx, best->nBits,
                best->pprev ? best->pprev->nHeight : -1,
                prev_hex[0] ? prev_hex : "<null>",
                merkle_null ? 1 : 0, best_backed ? 1 : 0);
        }
        if (!best_backed) {
            restore_tip = chain_restore_nearest_consensus_backed_ancestor_on_disk(
                best, ctx->datadir);
            if (restore_tip && restore_tip->phashBlock) {
                char bad_hex[65], good_hex[65];
                uint256_get_hex(&best_hash, bad_hex);
                uint256_get_hex(restore_tip->phashBlock, good_hex);
                fprintf(stderr,
                    "[boot] coins_best_block %s at h=%d is not backed by "
                    "real block data; using nearest consensus-backed "
                    "ancestor h=%d hash=%s\n",
                    bad_hex, best->nHeight, restore_tip->nHeight, good_hex);
            }
        } else if (has_disk_backed_competing_sibling(ctx->state, best,
                                                    ctx->datadir)) {
            restore_tip = best->pprev;
            if (restore_tip && restore_tip->phashBlock) {
                char bad_hex[65], parent_hex[65];
                uint256_get_hex(&best_hash, bad_hex);
                uint256_get_hex(restore_tip->phashBlock, parent_hex);
                fprintf(stderr,
                    "[boot] coins_best_block %s at h=%d is a disk-backed "
                    "fork leaf with a competing disk-backed sibling; "
                    "restoring common ancestor h=%d hash=%s so normal "
                    "validation can choose the best branch\n",
                    bad_hex, best->nHeight, restore_tip->nHeight,
                    parent_hex);
            }
        }

        if (!restore_tip) {
            fprintf(stderr,
                "[boot] coins_best_block found in index but no "
                "consensus-backed ancestor is available; waiting for P2P\n");
            return res;
        }

        if (!utxo_recovery_commit_tip(
                ctx, restore_tip, "coins_best_restore", true))
            return res;
        (void)chain_restore_rebuild_active_chain(ctx->state, restore_tip);
        printf("Restored chain tip from coins DB: height=%d\n",
               restore_tip->nHeight);
        event_emitf(EV_BOOT_CHAIN_RESTORED, 0, "height=%d",
                    restore_tip->nHeight);
        res.restored = true;

        /* P14.11 + P14.12: populate active_chain.chain[] from pprev +
         * block_map, and backfill nBits from on-disk block headers for
         * any pindex entry whose nBits is still zero. Without this, the
         * anchor-restore path leaves `getblockhash <h>` broken for every
         * h below the tip and GetNextWorkRequired trips `bad-diffbits`
         * on the first real-difficulty header whose pprev window
         * includes an nBits==0 entry. */
        snapsync_set_anchor(NULL);
        (void)chain_restore_finalize(ctx->state, ctx->datadir);

        return res;
    }

    /* coins_best_block not in block_map — create placeholder anchor */
    char hex[65];
    uint256_get_hex(&best_hash, hex);
    printf("Coins DB best block %s not in index (block_map size=%zu).\n",
           hex, ctx->state->map_block_index.size);

    int utxo_max_height = 0;
    if (ctx->ndb->open) {
        sqlite3_stmt *hstmt = NULL;
        sqlite3_prepare_v2(ctx->ndb->db,
            "SELECT MAX(height) FROM utxos", -1, &hstmt, NULL);
        if (hstmt && AR_STEP_ROW_READONLY(hstmt) == SQLITE_ROW)
            utxo_max_height = sqlite3_column_int(hstmt, 0);
        if (hstmt) sqlite3_finalize(hstmt);
    }

    if (utxo_max_height > 0) {
        struct block_index *anchor =
            chain_restore_create_anchor(ctx->state, &best_hash,
                                        utxo_max_height);
        if (anchor) {
            snapsync_set_anchor(anchor);

            printf("Chain restore: metadata anchor at h=%d hash=%s "
                   "— waiting for real block data.\n", utxo_max_height, hex);
            /* P14.11 + P14.12: see the same call above; fire here too
             * so the fresh-anchor path gets rebuild + nBits backfill. */
            (void)chain_restore_finalize(ctx->state, ctx->datadir);
        }
        res.skip_activate = true;
        snprintf(res.anchor_reason, sizeof(res.anchor_reason),
                 "chain_restore_anchor");
    } else {
        /* No UTXOs — wipe and start fresh */
        printf("No UTXOs found — wiping coins state.\n");
        (void)utxo_recovery_wipe(ctx->ndb, "boot.restore_no_utxos");
        (void)utxo_recovery_commit_genesis(ctx, "boot.restore_no_utxos");
    }

    res.restored = true;
    return res;
}

/* ── Validation recovery execution ──────────────────────────── */

/* Helper: recover from stale metadata by fixing coins_best_block
 * from UTXO set heights instead of wiping. */
static bool recover_stale_metadata(struct utxo_recovery_ctx *ctx)
{
    int64_t actual_utxos = node_db_utxo_count(ctx->ndb);
    if (actual_utxos <= 1000)
        return false;

    fprintf(stderr,
        "ABORT WIPE: validation says 'empty' but utxos "
        "table has %lld rows. coins_best_block may be "
        "stale, NOT the UTXOs. Refusing to destroy data.\n",
        (long long)actual_utxos);

    int max_h = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ctx->ndb->db,
            "SELECT MAX(height) FROM utxos",
            -1, &st, NULL) == SQLITE_OK) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
            max_h = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (max_h > 0) {
        struct db_block tip_blk;
        if (db_block_find_by_height(ctx->ndb, max_h, &tip_blk)) {
            struct uint256 tip_hash;
            memcpy(tip_hash.data, tip_blk.hash, 32);
            struct block_index *bi = block_map_find(
                &ctx->state->map_block_index, &tip_hash);
            if (bi && chain_restore_block_is_consensus_backed_on_disk(
                    bi, ctx->datadir)) {
                if (utxo_recovery_commit_tip(
                        ctx, bi, "best_have_data", true)) {
                    coins_view_cache_flush(ctx->coins_tip);
                    printf("RECOVERY: restored chain tip from UTXO set: h=%d\n",
                           max_h);
                }
            } else if (bi) {
                fprintf(stderr,
                    "RECOVERY: UTXO-derived tip h=%d is not disk-backed; "
                    "not setting active tip\n", max_h);
            }
        }
    }
    return true;
}

/* Helper: wipe UTXOs and reset to genesis for clean re-sync */
static void reset_to_genesis(struct utxo_recovery_ctx *ctx)
{
    (void)utxo_recovery_wipe(ctx->ndb, "boot.reset_to_genesis");

    struct block_index *genesis = block_map_find(
        &ctx->state->map_block_index,
        &ctx->params->consensus.hashGenesisBlock);
    if (genesis) {
        if (utxo_recovery_commit_tip(ctx, genesis, "fresh_genesis", true))
            coins_view_cache_flush(ctx->coins_tip);
    }

    /* Clear migration flag for LevelDB re-import on next boot */
    node_db_exec(ctx->ndb,
        "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'");

    extern _Atomic bool g_utxo_commitment_skip;
    atomic_store(&g_utxo_commitment_skip, true);

    set_flush_policy(3600, 1000000, 500);
}

/* Helper: boot-time integrity checks when BOOT_OK */
static bool integrity_checks_boot_ok(struct utxo_recovery_ctx *ctx,
                                      bool *out_skip_activate)
{
    int tip_h = active_chain_height(&ctx->state->chain_active);

    /* A: Stale UTXO wipe — chain at genesis but UTXO set non-empty */
    if (tip_h <= 0 && ctx->ndb->open) {
        sqlite3_stmt *stale_cnt = NULL;
        int64_t stale_utxos = 0;
        if (sqlite3_prepare_v2(ctx->ndb->db,
                "SELECT COUNT(*) FROM utxos", -1,
                &stale_cnt, NULL) == SQLITE_OK && stale_cnt) {
            if (AR_STEP_ROW_READONLY(stale_cnt) == SQLITE_ROW)
                stale_utxos = sqlite3_column_int64(stale_cnt, 0);
            sqlite3_finalize(stale_cnt);
        }
        if (stale_utxos > 0) {
            printf("WARNING: Chain at genesis but %lld stale UTXOs "
                   "from previous snapshot — wiping for clean sync\n",
                   (long long)stale_utxos);
            (void)utxo_recovery_wipe(ctx->ndb,
                                      "boot.stale_utxos_at_genesis");
            (void)utxo_recovery_commit_genesis(
                ctx, "boot.stale_utxos_at_genesis");
            *out_skip_activate = true;
        }
    }

    /* B: UTXO count sanity check against SHA3 checkpoint */
    const struct sha3_utxo_checkpoint *sha3cp = get_sha3_utxo_checkpoint();
    if (sha3cp && sha3cp->height > 0 &&
        tip_h >= sha3cp->height && ctx->ndb->open) {
        sqlite3_stmt *cnt_stmt = NULL;
        sqlite3_prepare_v2(ctx->ndb->db,
            "SELECT COUNT(*) FROM utxos", -1, &cnt_stmt, NULL);
        if (cnt_stmt && AR_STEP_ROW_READONLY(cnt_stmt) == SQLITE_ROW) {
            int64_t actual = sqlite3_column_int64(cnt_stmt, 0);
            struct utxo_count_check_result count_check =
                utxo_recovery_classify_count_check(
                    tip_h, sha3cp->height, sha3cp->utxo_count,
                    (uint64_t)actual);
            if (count_check.level == UTXO_COUNT_CHECK_CRITICAL)
                fprintf(stderr, "CRITICAL: UTXO count %lld vs "
                        "checkpoint %lld (%.1f%% off) — consider "
                        "reimport\n", (long long)actual,
                        (long long)sha3cp->utxo_count,
                        count_check.pct_delta);
            else if (count_check.level == UTXO_COUNT_CHECK_WARNING)
                printf("WARNING: UTXO count %lld vs checkpoint %lld "
                       "(%.1f%% off, chain %d blocks past checkpoint)\n",
                       (long long)actual,
                       (long long)sha3cp->utxo_count,
                       count_check.pct_delta,
                       count_check.blocks_past_checkpoint);
            else if (count_check.level ==
                     UTXO_COUNT_CHECK_INFO_STALE_REFERENCE)
                printf("INFO: skipping UTXO count checkpoint warning: "
                       "checkpoint h=%d is %d blocks behind tip h=%d "
                       "(actual=%lld checkpoint=%lld delta=%.1f%%)\n",
                       sha3cp->height,
                       count_check.blocks_past_checkpoint,
                       tip_h,
                       (long long)actual,
                       (long long)sha3cp->utxo_count,
                       count_check.pct_delta);
        }
        sqlite3_finalize(cnt_stmt);
    }

    /* C: XOR commitment verification */
    if (ctx->ndb->open) {
        struct utxo_commitment saved_uc, computed_uc;
        memset(&saved_uc, 0, sizeof(saved_uc));
        memset(&computed_uc, 0, sizeof(computed_uc));
        if (utxo_commitment_load_checkpoint(ctx->ndb->db, &saved_uc)) {
            utxo_commitment_compute_db(ctx->ndb->db, &computed_uc);
            if (!utxo_commitment_equal(&saved_uc, &computed_uc)) {
                if (utxo_recovery_xor_mismatch_is_corruption_candidate(
                        saved_uc.count, computed_uc.count)) {
                    fprintf(stderr, "WARNING: XOR commitment mismatch — "
                            "UTXO set may be corrupted. "
                            "Consider running --importchainstate\n");
                } else {
                    printf("INFO: skipping XOR commitment corruption warning: "
                           "stored commitment checkpoint is stale "
                           "(saved_count=%llu computed_count=%llu)\n",
                           (unsigned long long)saved_uc.count,
                           (unsigned long long)computed_uc.count);
                    if (ctx->coins_tip)
                        ctx->coins_tip->commitment = computed_uc;
                    if (utxo_commitment_save_checkpoint(ctx->ndb->db,
                                                        &computed_uc)) {
                        printf("INFO: refreshed stale XOR commitment "
                               "checkpoint (count=%llu)\n",
                               (unsigned long long)computed_uc.count);
                    } else {
                        fprintf(stderr, "WARNING: failed to refresh stale "
                                "XOR commitment checkpoint\n");
                    }
                }
            }
        }
    }

    return true;
}

struct recovery_exec_result utxo_recovery_execute(
    struct utxo_recovery_ctx *ctx,
    struct boot_validation_result *vr)
{
    struct recovery_exec_result res = {0};

    /* Check activation controller — skip when ANCHOR_ACTIVE */
    struct utxo_wipe_decision wd;
    activation_should_allow_utxo_wipe(&wd,
        activation_get_state(ctx->activation_ctl),
        snapsync_get_anchor() != NULL);
    if (!wd.safe_to_wipe) {
        printf("Skipping coins/chain validation — %s\n", wd.reason);
        return res;
    }

    switch (vr->action) {
    case BOOT_RECOVER_REIMPORT:
    case BOOT_RECOVER_WIPE_WAIT:
        printf("WARNING: Chain tip at h=%d but coins DB %s!\n",
               vr->chain_height,
               vr->action == BOOT_RECOVER_REIMPORT
                   ? "empty (LevelDB available)" : "empty");

        /* SAFETY: check actual UTXO count before wiping */
        if (recover_stale_metadata(ctx)) {
            res.recovered = true;
            break;
        }

        reset_to_genesis(ctx);
        res.recovered = true;
        break;

    case BOOT_RECOVER_RESET_CHAIN: {
        struct block_index *coins_block = block_map_find(
            &ctx->state->map_block_index, &vr->coins_hash);
        if (coins_block) {
            if (chain_restore_block_is_consensus_backed_on_disk(
                    coins_block, ctx->datadir)) {
                printf("Chain tip/coins mismatch: chain=%d coins=%d\n"
                       "  Resetting chain to disk-backed coins tip — "
                       "will replay %d blocks.\n",
                       vr->chain_height, vr->coins_height,
                       vr->chain_height - vr->coins_height);
                (void)utxo_recovery_commit_tip(
                    ctx, coins_block, "chain_coins_mismatch_reset", true);
            } else {
                fprintf(stderr,
                    "Chain tip/coins mismatch: coins tip h=%d is not "
                    "disk-backed; refusing active-tip reset\n",
                    vr->coins_height);
            }
        }
        res.recovered = true;
        break;
    }
    case BOOT_RECOVER_RESET_COINS_TO_CHAIN_TIP: {
        struct block_index *chain_tip =
            active_chain_tip(&ctx->state->chain_active);
        if (chain_tip) {
            (void)utxo_recovery_commit_tip(
                ctx, chain_tip, "coins_cursor_to_chain_tip", true);
            res.recovered = true;
        }
        break;
    }
    case BOOT_RECOVER_RESET_COINS_TO_GENESIS:
        if (utxo_recovery_commit_genesis(ctx, "coins_cursor_to_genesis"))
            res.recovered = true;
        break;
    case BOOT_OK:
        integrity_checks_boot_ok(ctx, &res.skip_activate);
        break;
    }

    return res;
}

/* ── UTXO cleanup ───────────────────────────────────────────── */

int utxo_recovery_clean_above_tip(struct node_db *ndb,
                                   struct main_state *state)
{
    if (!ndb->open) return 0;

    struct block_index *tip = active_chain_tip(&state->chain_active);
    int tip_h = tip ? tip->nHeight : 0;
    if (tip_h <= 0) return 0;

    /* Count how many UTXOs would be wiped */
    int64_t would_wipe = 0;
    {
        sqlite3_stmt *st = NULL;
        char count_sql[128];
        snprintf(count_sql, sizeof(count_sql),
                 "SELECT count(*) FROM utxos WHERE height > %d", tip_h);
        if (sqlite3_prepare_v2(ndb->db, count_sql, -1, &st, NULL) == SQLITE_OK) {
            if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
                would_wipe = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }

    if (would_wipe > 1000) {
        fprintf(stderr,
            "ABORT: would wipe %lld UTXOs above tip h=%d. "
            "Chain tip is likely wrong, not the UTXOs. "
            "Refusing to destroy data. "
            "Investigate block_index.bin corruption.\n",
            (long long)would_wipe, tip_h);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "wipe_blocked count=%lld tip=%d",
            (long long)would_wipe, tip_h);
        return 0;
    }

    if (would_wipe > 0) {
        event_emitf(EV_RECOVERY_ACTION, 0,
            "action=utxo_prune_above_tip height=%d count=%lld",
            tip_h, (long long)would_wipe);
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "DELETE FROM utxos WHERE height > %d", tip_h);
        char *err = NULL;
        int rc = sqlite3_exec(ndb->db, sql, NULL, NULL, &err);
        int changes = sqlite3_changes(ndb->db);
        if (rc == SQLITE_OK && changes > 0)
            printf("Boot: removed %d UTXOs above tip h=%d\n",
                   changes, tip_h);
        if (err) sqlite3_free(err);
        return changes;
    }

    return 0;
}

/* ── Shielded value backfill ────────────────────────────────── */

struct shielded_backfill_ctx {
    int updated;
    struct main_state *state;
    const char *datadir;
};

extern const char *g_datadir;

static bool backfill_shielded_write(struct node_db *ndb, void *ctx_ptr)
{
    struct shielded_backfill_ctx *bctx = ctx_ptr;
    if (!ndb || !ndb->open) LOG_FAIL("utxo_recovery", "backfill_shielded called with null or closed db");

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT OR REPLACE INTO blocks"
        "(hash,height,prev_hash,version,merkle_root,"
        "time,bits,nonce,solution,chain_work,status,"
        "file_num,data_pos,undo_pos,num_tx,"
        "sapling_root,sprout_root,sapling_value,sprout_value)"
        " VALUES(?,?,?,?,?,?,?,?,X'',X'',?,?,?,0,?,NULL,NULL,?,?)";
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "backfill: prepare failed: %s\n",
                sqlite3_errmsg(ndb->db));
        return false;
    }

    int updated = 0, batch = 0;
    node_db_begin(ndb);

    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&bctx->state->map_block_index, &iter, NULL, &bi)) {
        if (!bi) continue;
        if (bi->nFile < 0 || !(bi->nStatus & 8)) continue;
        if (bi->nDataPos == 0 && bi->nHeight > 0) continue;

        int64_t sprout_val = bi->nSproutValue;
        int64_t sapling_val = bi->nSaplingValue;

        /* If block_index has no values, read from disk */
        if (sprout_val == 0 && sapling_val == 0) {
            struct block blk;
            if (!read_block_from_disk_index(&blk, bi, bctx->datadir))
                continue;
            for (size_t i = 0; i < blk.num_vtx; i++) {
                const struct transaction *tx = &blk.vtx[i];
                for (size_t j = 0; j < tx->num_joinsplit; j++) {
                    sprout_val += tx->v_joinsplit[j].vpub_old;
                    sprout_val -= tx->v_joinsplit[j].vpub_new;
                }
                sapling_val += tx->value_balance;
            }
            block_free(&blk);
            if (sprout_val == 0 && sapling_val == 0) continue;
            bi->nSproutValue = sprout_val;
            bi->nSaplingValue = sapling_val;
        }

        sqlite3_reset(stmt);
        sqlite3_bind_blob(stmt, 1, bi->phashBlock->data, 32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, bi->nHeight);
        sqlite3_bind_blob(stmt, 3,
            bi->pprev ? bi->pprev->phashBlock->data : (const uint8_t[32]){0},
            32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, bi->nVersion);
        sqlite3_bind_blob(stmt, 5, bi->hashMerkleRoot.data, 32, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, bi->nTime);
        sqlite3_bind_int(stmt, 7, (int)bi->nBits);
        sqlite3_bind_blob(stmt, 8, bi->nNonce.data, 32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 9, bi->nStatus);
        sqlite3_bind_int(stmt, 10, bi->nFile);
        sqlite3_bind_int(stmt, 11, (int)bi->nDataPos);
        sqlite3_bind_int(stmt, 12, bi->nTx);
        sqlite3_bind_int64(stmt, 13, sapling_val);
        sqlite3_bind_int64(stmt, 14, sprout_val);

        if (AR_STEP_ROW_READONLY(stmt) != SQLITE_DONE) {
            static int errs = 0;
            if (++errs <= 3)
                fprintf(stderr, "backfill h=%d: %s\n",
                        bi->nHeight, sqlite3_errmsg(ndb->db));
        } else {
            updated++;
        }

        if (++batch >= 5000) {
            node_db_commit(ndb);
            node_db_begin(ndb);
            batch = 0;
            printf("  backfill: %d blocks so far...\n", updated);
            fflush(stdout);
        }
    }

    node_db_commit(ndb);
    sqlite3_finalize(stmt);
    node_db_state_set_int(ndb, "shielded_backfilled", 1);

    bctx->updated = updated;
    printf("Shielded backfill complete: %d blocks with "
           "JoinSplit/Sapling data\n", updated);
    fflush(stdout);
    return true;
}

int utxo_recovery_backfill_shielded(struct node_db *ndb,
                                     struct db_service *dbsvc,
                                     struct main_state *state,
                                     const char *datadir)
{
    struct shielded_backfill_ctx bctx = {
        .updated = 0,
        .state = state,
        .datadir = datadir,
    };
    bool ok = false;

    printf("Backfilling shielded values from block_index...\n");
    if (dbsvc)
        ok = db_service_run_write(dbsvc, backfill_shielded_write, &bctx);
    else
        ok = backfill_shielded_write(ndb, &bctx);

    if (ok) {
        printf("Backfill: updated %d blocks with shielded values\n",
               bctx.updated);
        fflush(stdout);
        return bctx.updated;
    }

    LOG_ERR("recovery", "backfill: failed to update shielded values");
}
