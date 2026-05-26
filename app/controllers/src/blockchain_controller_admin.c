/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Administrative chainstate RPCs: reindexchainstate (replay UTXO set from
 * blocks) and importchainstate (bulk-import UTXO set from an external
 * LevelDB chainstate). Heavy, operator-invoked operations. See
 * blockchain_controller_internal.h for shared declarations. */

#include "platform/time_compat.h"
#include "controllers/blockchain_controller.h"
#include "blockchain_controller_internal.h"
#include "controllers/sync_controller.h"
#include "controllers/strong_params.h"
#include "config/runtime.h"
#include "chain/chain.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "json/json.h"
#include "models/database.h"
#include "primitives/block.h"
#include "services/chain_state_repository.h"
#include "storage/coins_db.h"
#include "storage/disk_block_io.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/update_coins.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── reindexchainstate ──────────────────────────────────────── */

static bool reindex_set_coins_best(struct blockchain_context *ctx,
                                   const struct uint256 *block_hash,
                                   const char *reason)
{
    if (!ctx || !block_hash)
        return false;
    struct chain_state_rollback_authorization auth = {
        .source = CSR_ROLLBACK_SOURCE_REINDEX,
        .decision = POLICY_ALLOW,
        .from_height = ctx->main_state
            ? active_chain_height(&ctx->main_state->chain_active) : -1,
        .to_height = ctx->main_state
            ? active_chain_height(&ctx->main_state->chain_active) : -1,
        .max_depth = 0,
        .evidence_class = "operator_reindex_replay",
        .reason = reason ? reason : "reindexchainstate.replay",
    };
    struct chain_state_coins_best_repair repair = {
        .new_coins_best = *block_hash,
        .repair_auth = &auth,
        .reason = reason ? reason : "reindexchainstate.replay",
    };
    enum csr_result rc = csr_repair_set_coins_best(csr_instance(), &repair);
    if (rc == CSR_OK)
        return true;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED && ctx->coins_tip) {
        coins_view_cache_set_best_block(ctx->coins_tip, block_hash);
        return true;
    }
#endif

    LOG_FAIL("blockchain", "reindexchainstate: csr rejected coins-best repair: %s",
            csr_result_name(rc));
}
bool rpc_reindexchainstate(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    (void)params;
    RPC_HELP(help, result,
        "reindexchainstate\n"
        "\nWipes the chainstate (UTXO database) and rebuilds it by replaying\n"
        "all blocks from genesis to chain tip. This fixes any corrupt coins\n"
        "entries from prior serialization bugs.\n"
        "\nWARNING: This operation takes a long time (minutes to hours).\n"
        "The node will not process new blocks or transactions during reindex.\n");

    if (!ctx->coins_db || !ctx->coins_tip || !ctx->main_state || !ctx->datadir) {
        json_set_str(result, "Node not fully initialized");
        LOG_FAIL("blockchain", "reindexchainstate: node not fully initialized (coins_db=%p coins_tip=%p main_state=%p datadir=%p)",
                 (void *)ctx->coins_db, (void *)ctx->coins_tip, (void *)ctx->main_state, (void *)ctx->datadir);
    }

    int tip_height = active_chain_height(&ctx->main_state->chain_active);
    if (tip_height < 0) {
        json_set_str(result, "No active chain");
        LOG_FAIL("blockchain", "reindexchainstate: no active chain (tip_height=%d)", tip_height);
    }

    printf("reindexchainstate: rebuilding UTXO set for %d blocks...\n",
           tip_height + 1);
    fflush(stdout);

    /* Step 1: Flush and free the in-memory cache */
    coins_view_cache_flush(ctx->coins_tip);
    coins_view_cache_free(ctx->coins_tip);

    /* Step 2: Close and reopen coins DB with wipe=true */
    coins_view_db_close(ctx->coins_db);

    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", ctx->datadir);
    if (!coins_view_db_open(ctx->coins_db, coins_path,
                            450 << 20, false, true)) {
        json_set_str(result, "Failed to reopen coins database");
        LOG_FAIL("blockchain", "reindexchainstate: failed to reopen coins database at %s", coins_path);
    }

    /* Step 3: Reinitialize coins cache */
    coins_view_cache_init(ctx->coins_tip, &ctx->coins_db->view);

    /* Step 3.5: Reset sapling tree state — must replay from empty.
     * Use the global node_db (set later in file via rpc_blockchain_set_node_db). */
    {
        struct node_db *ndb = app_runtime_node_db();
        if (ndb && ndb->open) {
            node_db_state_set(ndb, "sapling_tree", NULL, 0);
            node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);
        }
    }

    int64_t t_start = (int64_t)platform_time_wall_time_t();
    int errors = 0;

    /* Step 4: Replay all blocks */
    for (int h = 0; h <= tip_height; h++) {
        struct block_index *pindex = active_chain_at(
            &ctx->main_state->chain_active, h);
        if (!pindex) {
            printf("reindexchainstate: missing block_index at height %d\n", h);
            errors++;
            continue;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, ctx->datadir)) {
            printf("reindexchainstate: failed to read block at height %d\n", h);
            errors++;
            continue;
        }

        /* Genesis block: just set best block */
        if (h == 0) {
            struct uint256 block_hash;
            block_header_get_hash(&blk.header, &block_hash);
            if (!reindex_set_coins_best(ctx, &block_hash,
                                        "reindexchainstate.genesis")) {
                block_free(&blk);
                errors++;
                continue;
            }
            block_free(&blk);
            if (h % 10000 == 0) {
                printf("  height %d/%d\n", h, tip_height);
                fflush(stdout);
            }
            continue;
        }

        /* Apply each transaction to the UTXO set */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            update_coins(&blk.vtx[i], ctx->coins_tip, pindex->nHeight);
        }

        /* Set best block hash for this operator-invoked replay step.
         * active_chain does not move here; CSR gates the repair as a
         * typed coins-best-only mutation. */
        struct uint256 block_hash;
        block_header_get_hash(&blk.header, &block_hash);
        if (!reindex_set_coins_best(ctx, &block_hash,
                                    "reindexchainstate.replay")) {
            block_free(&blk);
            errors++;
            continue;
        }

        block_free(&blk);

        /* Periodic flush every 10000 blocks */
        if (h % 10000 == 0) {
            coins_view_cache_flush(ctx->coins_tip);
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
            int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
            printf("  height %d/%d (%.0f blk/s, ETA %dm%ds)\n",
                   h, tip_height, rate, eta / 60, eta % 60);
            fflush(stdout);
        }
    }

    /* Step 5: Final flush */
    coins_view_cache_flush(ctx->coins_tip);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("reindexchainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    fflush(stdout);

    /* Report results */
    json_set_object(result);
    json_push_kv_int(result, "height", tip_height);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    json_push_kv_int(result, "errors", errors);
    json_push_kv_str(result, "status", errors == 0 ? "success" : "completed with errors");

    return true;
}

/* ── importchainstate ──────────────────────────────────────── */

bool rpc_importchainstate(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct blockchain_context *ctx = blockchain_ctx();
    RPC_HELP(help, result,
        "importchainstate \"chainstate_path\"\n"
        "\nRebuild the UTXO index from an external LevelDB chainstate directory.\n"
        "Use this to import the complete UTXO set from a zclassicd node:\n"
        "  importchainstate /home/user/.zclassic/chainstate\n"
        "\nThis replaces all UTXOs in SQLite with those from the given chainstate.\n"
        "The source node should be stopped to avoid LevelDB lock conflicts.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *cs_path = rpc_require_str(&p, 0, "chainstate_path");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("blockchain", "importchainstate: invalid params"); }

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Node database not open");
        LOG_FAIL("blockchain", "importchainstate: node database not open");
    }

    printf("importchainstate: opening %s...\n", cs_path);
    fflush(stdout);

    struct coins_view_db ext_db;
    memset(&ext_db, 0, sizeof(ext_db));
    if (!coins_view_db_open(&ext_db, cs_path, 256, false, false)) {
        json_set_str(result, "Cannot open chainstate LevelDB");
        LOG_FAIL("blockchain", "importchainstate: cannot open LevelDB at %s", cs_path);
    }

    /* Read best block hash from LevelDB before importing.
     * This is the height at which the UTXO set was snapshotted. */
    struct uint256 ldb_best_block;
    memset(&ldb_best_block, 0, sizeof(ldb_best_block));
    coins_view_db_get_best_block(&ext_db, &ldb_best_block);
    if (uint256_is_null(&ldb_best_block)) {
        coins_view_db_close(&ext_db);
        json_set_str(result, "LevelDB chainstate best block is unset");
        LOG_FAIL("blockchain", "importchainstate: best block is unset");
    }
    if (!ctx->main_state) {
        coins_view_db_close(&ext_db);
        json_set_str(result, "Main chain state not available");
        LOG_FAIL("blockchain", "importchainstate: main state not available");
    }
    struct block_index *ldb_tip = block_map_find(
        &ctx->main_state->map_block_index, &ldb_best_block);
    if (!ldb_tip || !ldb_tip->phashBlock) {
        coins_view_db_close(&ext_db);
        json_set_str(result,
                     "LevelDB best block is not verified in the block index");
        LOG_FAIL("blockchain",
                 "importchainstate: best block missing from block index");
    }

    struct node_db import_db;
    struct node_db *import_target = ctx->node_db;
    if (node_db_sync_open_private_db_like(ctx->node_db, &import_db))
        import_target = &import_db;

    int count = node_db_sync_import_utxos(import_target, &ext_db);
    if (import_target == &import_db)
        node_db_close(&import_db);
    coins_view_db_close(&ext_db);

    if (count < 0) {
        json_set_str(result, "Import failed");
        LOG_FAIL("blockchain", "importchainstate: UTXO import failed (count=%d)", count);
    }

    /* Fix height=0 UTXOs from transaction index (LevelDB decoder can
     * fail to read the trailing height varint for some entries). */
    {
        sqlite3_stmt *h0 = NULL;
        sqlite3_prepare_v2(ctx->node_db->db,
            "SELECT COUNT(*) FROM utxos WHERE height = 0 AND value > 0",
            -1, &h0, NULL);
        int64_t h0_count = 0;
        if (h0 && AR_STEP_ROW_READONLY(h0) == SQLITE_ROW)
            h0_count = sqlite3_column_int64(h0, 0);
        sqlite3_finalize(h0);
        if (h0_count > 0) {
            printf("importchainstate: fixing %lld UTXOs with height=0...\n",
                   (long long)h0_count);
            sqlite3_exec(ctx->node_db->db,
                "UPDATE utxos SET height = ("
                "  SELECT t.block_height FROM transactions t"
                "  WHERE t.txid = utxos.txid"
                ") WHERE height = 0 AND EXISTS ("
                "  SELECT 1 FROM transactions t"
                "  WHERE t.txid = utxos.txid AND t.block_height IS NOT NULL"
                ")", NULL, NULL, NULL);
            printf("importchainstate: fixed %d UTXO heights\n",
                   sqlite3_changes(ctx->node_db->db));
        }
    }

    /* Rebuild wallet_utxos and addresses from new UTXO set */
    sqlite3_exec(ctx->node_db->db, "BEGIN", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "DELETE FROM wallet_utxos", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "INSERT INTO wallet_utxos "
        "(txid, vout, value, address_hash, script, height, is_coinbase) "
        "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
        "u.height, u.is_coinbase "
        "FROM utxos u INNER JOIN wallet_keys wk "
        "ON u.address_hash = wk.pubkey_hash",
        NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "DELETE FROM addresses", NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);
    sqlite3_exec(ctx->node_db->db, "COMMIT", NULL, NULL, NULL);

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_UTXO_REPAIR,
        .decision = POLICY_ALLOW,
        .from_height = active_chain_height(&ctx->main_state->chain_active),
        .to_height = ldb_tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "leveldb_chainstate_best_block_indexed",
        .reason = "rpc.importchainstate",
    };
    struct chain_state_commit commit = {
        .new_tip = ldb_tip,
        .new_coins_best = *ldb_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = true,
        .persist_coins_best = true,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = "rpc.importchainstate",
    };
    enum csr_result csr_rc = csr_commit_tip(csr_instance(), &commit);
#ifdef ZCL_TESTING
    if (csr_rc == CSR_REJECTED_NOT_INITIALIZED) {
        coins_view_cache_set_best_block(ctx->coins_tip, ldb_tip->phashBlock);
        csr_rc = CSR_OK;
    }
#endif
    if (csr_rc != CSR_OK) {
        json_set_str(result, "CSR rejected imported chainstate tip");
        LOG_FAIL("blockchain",
                 "importchainstate: csr rejected imported tip (%s)",
                 csr_result_name(csr_rc));
    }

    json_set_object(result);
    json_push_kv_int(result, "utxos_imported", count);

    /* Report balance */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT COALESCE(SUM(value),0) FROM utxos",
        -1, &s, NULL);
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
        json_push_kv_int(result, "total_value_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    s = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos WHERE spent_txid IS NULL",
        -1, &s, NULL);
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
        json_push_kv_int(result, "wallet_balance_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    printf("importchainstate: done — %d UTXOs imported\n", count);
    fflush(stdout);
    return true;
}
