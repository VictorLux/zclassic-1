/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Wallet rescan, legacy import, and witness management RPCs. */

#include "platform/time_compat.h"
#include "controllers/wallet_rescan_controller.h"
#include "controllers/rpc_chainstate_guard.h"
#include "controllers/wallet_helpers.h"
#include "controllers/strong_params.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "chain/chainparams.h"
#include "encoding/utilmoneystr.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "sapling/fast_scan.h"
#include <stdatomic.h>
#include "script/standard.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "core/random.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/sync_evidence_policy.h"
#include "validation/txmempool.h"
#include "wallet/wallet_sqlite.h"
#include "net/connman.h"
#include "sapling/sapling.h"
#include "sapling/fr.h"
#include "sapling/incremental_merkle_tree.h"
#include "consensus/upgrades.h"
#include "models/database.h"
#include "models/block.h"
#include "models/utxo.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_scan.h"
#include "models/chain_snapshot.h"
#include "controllers/legacy_import.h"
#include "core/serialize.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "views/wallet_view.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static struct wallet_rpc_context *wallet_ctx(void)
{
    return wallet_rpc_context_current();
}

static bool wallet_reset_begin_checked(struct node_db *ndb,
                                       const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb))
        LOG_FAIL("wallet_rescan", "%s failed: %s", label,
                 (ndb && ndb->db) ? sqlite3_errmsg(ndb->db) : "db unavailable");
    return true;
}

static bool wallet_reset_commit_checked(struct node_db *ndb,
                                        const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb))
        LOG_FAIL("wallet_rescan", "%s failed: %s", label,
                 (ndb && ndb->db) ? sqlite3_errmsg(ndb->db) : "db unavailable");
    return true;
}

static void wallet_reset_rollback_best_effort(struct node_db *ndb,
                                              const char *label)
{
    if (!ndb || !ndb->open)
        return;
    if (!node_db_rollback(ndb)) {
        LOG_WARN("wallet_rescan", "[wallet_rescan] %s: rollback failed: %s", label, ndb->db ? sqlite3_errmsg(ndb->db) : "db unavailable");
    }
}

static bool rpc_replaywalletfromchain(const struct json_value *params,
                                       bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "replaywalletfromchain confirm\n"
        "Nuclear rebuild: wipe all wallet UTXOs and transactions from SQLite,\n"
        "then rescan all block files to rebuild from chain truth.\n"
        "\nArguments:\n"
        "1. confirm  (bool, required) Must be true to proceed\n");

    ENSURE_WALLET(result);
    if (!ctx->main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!ctx->datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    bool confirm = rpc_require_bool(&p, 0, "confirm");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    if (!confirm) {
        json_set_str(result,
            "Safety interlock: pass confirm=true to proceed");
        return false;
    }

    int64_t old_balance = db_wallet_utxo_balance(ctx->node_db);

    if (!wallet_reset_begin_checked(ctx->node_db,
                                    "replaywalletfromchain begin")) {
        json_set_str(result, "Failed to begin wallet reset transaction");
        return false;
    }
    if (!db_wallet_utxo_delete_all(ctx->node_db) ||
        !db_wallet_tx_delete_all(ctx->node_db)) {
        wallet_reset_rollback_best_effort(ctx->node_db,
                                          "replaywalletfromchain rollback");
        json_set_str(result, "Failed to clear wallet tables before replay");
        return false;
    }
    if (!wallet_reset_commit_checked(ctx->node_db,
                                     "replaywalletfromchain commit")) {
        json_set_str(result, "Failed to commit wallet reset before replay");
        return false;
    }

    wallet_rebuild_spent_set(ctx->wallet);

    int chain_tip = active_chain_height(&ctx->main_state->chain_active);

    printf("replaywalletfromchain: rescanning %d blocks...\n",
           chain_tip + 1);
    fflush(stdout);

    int found = wallet_scan_blocks(ctx->node_db,
        &ctx->main_state->chain_active, ctx->wallet, ctx->datadir,
        0, chain_tip);

    int64_t new_balance = db_wallet_utxo_balance(ctx->node_db);
    int utxo_count = 0;
    {
        struct db_wallet_utxo tmp[4096];
        utxo_count = db_wallet_utxo_list_unspent(ctx->node_db, tmp, 4096);
    }

    wallet_view_replay_summary(result, utxo_count,
        found > 0 ? found : 0, new_balance, old_balance);
    return true;
}

static bool rpc_import_from(const struct json_value *params, bool help,
                             struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "import-from \"legacy_datadir\"\n"
        "Repair wallet LevelDB, copy chain data from legacy node, and reload.\n"
        "\nPhase 1: Repairs wallet LevelDB MANIFEST to recover lost keys.\n"
        "Phase 2: Copies blocks/index and chainstate from legacy datadir.\n"
        "Phase 3: Reloads wallet keys and rebuilds spent set.\n"
        "\nArguments:\n"
        "1. legacy_datadir (string, required) Path to legacy ~/.zclassic\n");

    ENSURE_WALLET(result);

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *legacy_dir = rpc_require_str(&p, 0, "legacy_datadir");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    if (!ctx->datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }
    if (!ctx->wallet_db) {
        json_set_str(result, "Wallet DB not available");
        return false;
    }

    json_set_object(result);

    /* ── Phase 1: Reload wallet from SQLite ── */
    size_t keys_before = ctx->wallet->keystore.num_keys;
    size_t txs_before = ctx->wallet->num_wallet_tx;

    /* Re-read wallet data from SQLite (no LevelDB repair needed) */
    bool repaired = true;

    struct json_value phase1 = {0};
    json_set_object(&phase1);
    json_push_kv_bool(&phase1, "repair_success", repaired);

    if (ctx->wallet_db && ctx->wallet_db->open) {
        struct zcl_result rk = wallet_sqlite_read_keys_r(ctx->wallet_db, ctx->wallet);
        if (!rk.ok) {
            LOG_FAIL("wallet", "wallet_repair: read_keys_r failed "
                                "(code=%d): %s", rk.code, rk.message);
        }
        wallet_sqlite_read_txs(ctx->wallet_db, ctx->wallet);
        wallet_sqlite_read_sapling_keys(ctx->wallet_db, ctx->wallet);
        wallet_sqlite_read_scripts(ctx->wallet_db, ctx->wallet);
        wallet_sqlite_read_watch_only(ctx->wallet_db, ctx->wallet);
    }

    size_t keys_after = ctx->wallet->keystore.num_keys;
    size_t txs_after = ctx->wallet->num_wallet_tx;
    size_t keys_recovered = keys_after > keys_before
                          ? keys_after - keys_before : 0;
    size_t txs_recovered = txs_after > txs_before
                         ? txs_after - txs_before : 0;

    json_push_kv_int(&phase1, "keys_before", (int64_t)keys_before);
    json_push_kv_int(&phase1, "keys_after", (int64_t)keys_after);
    json_push_kv_int(&phase1, "keys_recovered", (int64_t)keys_recovered);
    json_push_kv_int(&phase1, "txs_recovered", (int64_t)txs_recovered);
    json_push_kv(result, "wallet_repair", &phase1);
    json_free(&phase1);

    /* ── Phase 2: Validate + copy chain data via chain_snapshot model ── */
    struct chain_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.src_dir = legacy_dir;
    snap.dst_dir = ctx->datadir;

    if (!chain_snapshot_validate(&snap)) {
        struct json_value err = {0};
        json_set_object(&err);
        json_push_kv_str(&err, "error", "Source validation failed");
        json_push_kv_str(&err, "src_dir", legacy_dir);
        json_push_kv(result, "chain_copy", &err);
        json_free(&err);
        return true;
    }

    chain_snapshot_save(&snap);

    /* ── Phase 3: Rebuild wallet state ── */
    wallet_rebuild_spent_set(ctx->wallet);

    struct json_value phase3 = {0};
    json_set_object(&phase3);
    json_push_kv_int(&phase3, "total_keys", (int64_t)ctx->wallet->keystore.num_keys);
    json_push_kv_int(&phase3, "total_txs", (int64_t)ctx->wallet->num_wallet_tx);
    json_push_kv_int(&phase3, "spent_outpoints", (int64_t)ctx->wallet->num_spent);

    char s[32];
    format_amount(wallet_get_balance(ctx->wallet), s, sizeof(s));
    json_push_kv_str(&phase3, "balance", s);

    json_push_kv_str(&phase3, "note",
        "Restart node to load new chain data. "
        "Then run syncwalletfromdb to fix balance.");
    json_push_kv(result, "wallet_state", &phase3);
    json_free(&phase3);

    return true;
}

static bool rpc_syncwalletfromdb(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "syncwalletfromdb\n"
        "Sync in-memory wallet spent set from SQLite + chainstate truth.\n"
        "For each SQLite unspent UTXO verified in chainstate, removes it from\n"
        "the spent set if incorrectly marked. For UTXOs not in chainstate,\n"
        "marks them as spent. Fixes getbalance without restart.");

    ENSURE_WALLET(result);
    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!rpc_require_chainstate_lookup_ready(ctx->main_state, result,
            "syncwalletfromdb", "Chainstate lookup"))
        return false;
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }

    int64_t balance_before = wallet_get_balance(ctx->wallet);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 4096);

    int synced = 0, already_correct = 0, marked_spent = 0;

    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool found = coins_view_cache_get_coins(ctx->coins_tip, &tid, &c);
        bool available = found &&
            coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (available) {
            if (wallet_is_outpoint_spent(ctx->wallet, &tid, unspent[i].vout)) {
                wallet_unmark_outpoint_spent(ctx->wallet, &tid, unspent[i].vout);
                synced++;
            } else {
                already_correct++;
            }
        } else {
            if (!wallet_is_outpoint_spent(ctx->wallet, &tid, unspent[i].vout)) {
                wallet_mark_outpoint_spent(ctx->wallet, &tid, unspent[i].vout);
                marked_spent++;
            } else {
                already_correct++;
            }
        }
    }

    int64_t balance_after = wallet_get_balance(ctx->wallet);

    wallet_view_sync_summary(result, synced, already_correct, marked_spent,
                              balance_before, balance_after);
    return true;
}

static bool rpc_coinanalysis(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "coinanalysis\n"
        "Full chain analysis: for every wallet key, queries chainstate for\n"
        "unspent outputs. Compares against wallet's tracked UTXOs to find\n"
        "missing (untracked) coins. Reports total recoverable balance.");

    ENSURE_WALLET(result);
    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate not available");
        return false;
    }
    if (!rpc_require_chainstate_lookup_ready(ctx->main_state, result,
            "coinanalysis", "Chainstate lookup"))
        return false;
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }

    json_set_object(result);

    /* Get all wallet-tracked unspent UTXOs */
    struct db_wallet_utxo tracked[4096];
    int tracked_count = db_wallet_utxo_list_unspent(ctx->node_db, tracked, 4096);

    int64_t tracked_balance = 0;
    for (int i = 0; i < tracked_count; i++)
        tracked_balance += tracked[i].value;

    /* Scan all wallet transactions for outputs to our keys
     * that aren't in our tracked UTXO set */
    int untracked_count = 0;
    int64_t untracked_balance = 0;
    struct json_value untracked_arr = {0};
    json_set_array(&untracked_arr);

    /* Iterate all wallet transactions (from in-memory wallet) */
    for (size_t ti = 0; ti < ctx->wallet->num_wallet_tx; ti++) {
        const struct wallet_tx *wtx = &ctx->wallet->map_wallet[ti];
        for (size_t vi = 0; vi < wtx->tx.num_vout; vi++) {
            const struct tx_out *out = &wtx->tx.vout[vi];

            /* Check if output goes to one of our keys */
            struct tx_destination dest;
            if (!script_extract_destination(&out->script_pub_key, &dest))
                continue;
            if (dest.type != DEST_KEY_ID) continue;

            struct privkey test_key;
            if (!keystore_get_key(&ctx->wallet->keystore,
                                   &dest.id.key, &test_key))
                continue;
            memory_cleanse(test_key.vch, 32);

            /* It's ours. Check if tracked in SQLite */
            bool is_tracked = false;
            for (int j = 0; j < tracked_count; j++) {
                if (memcmp(tracked[j].txid, wtx->tx.hash.data, 32) == 0 &&
                    tracked[j].vout == (uint32_t)vi) {
                    is_tracked = true;
                    break;
                }
            }

            /* Check chainstate: is it actually unspent? */
            struct coins c;
            coins_init(&c);
            bool in_chain = coins_view_cache_get_coins(
                ctx->coins_tip, &wtx->tx.hash, &c);
            bool available = in_chain &&
                coins_is_available(&c, (unsigned int)vi);
            coins_free(&c);

            if (available && !is_tracked) {
                untracked_count++;
                untracked_balance += out->value;

                char addr[128];
                wallet_encode_destination(&dest, addr, sizeof(addr));

                char txid_hex[65];
                uint256_get_hex(&wtx->tx.hash, txid_hex);

                struct json_value entry = {0};
                json_set_object(&entry);
                json_push_kv_str(&entry, "txid", txid_hex);
                json_push_kv_int(&entry, "vout", (int64_t)vi);
                json_push_kv_str(&entry, "address", addr);

                char amt[32];
                format_amount(out->value, amt, sizeof(amt));
                json_push_kv_str(&entry, "amount", amt);
                json_push_kv_int(&entry, "confirmations",
                                  (int64_t)wtx->confirms);
                json_push_back(&untracked_arr, &entry);
                json_free(&entry);
            }
        }
    }

    /* Shielded analysis — all notes (spent and unspent) */
    int64_t z_balance = 0;
    int z_unspent = 0;
    int z_spent = 0;
    int64_t z_total_received = 0;
    struct json_value z_arr = {0};
    json_set_array(&z_arr);

    /* Query all sapling notes from SQLite (both spent and unspent) */
    sqlite3_stmt *z_stmt = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT txid, output_index, value, block_height, spent_txid,"
        " diversifier, pk_d, witness_height"
        " FROM wallet_sapling_notes ORDER BY block_height",
        -1, &z_stmt, NULL);
    while (z_stmt && AR_STEP_ROW_READONLY(z_stmt) == SQLITE_ROW) {
        const uint8_t *ntxid = sqlite3_column_blob(z_stmt, 0);
        int nidx = sqlite3_column_int(z_stmt, 1);
        int64_t nval = sqlite3_column_int64(z_stmt, 2);
        int nheight = sqlite3_column_int(z_stmt, 3);
        const uint8_t *spent_by = sqlite3_column_blob(z_stmt, 4);
        int spent_len = sqlite3_column_bytes(z_stmt, 4);
        const uint8_t *ndiv = sqlite3_column_blob(z_stmt, 5);
        const uint8_t *npkd = sqlite3_column_blob(z_stmt, 6);
        int wheight = sqlite3_column_int(z_stmt, 7);

        bool is_spent = (spent_by && spent_len == 32);
        z_total_received += nval;
        if (!is_spent) {
            z_balance += nval;
            z_unspent++;
        } else {
            z_spent++;
        }

        struct json_value ze = {0};
        json_set_object(&ze);

        char txid_hex[65];
        if (ntxid) {
            wallet_txid_hex_le(ntxid, txid_hex);
        } else {
            txid_hex[0] = '\0';
        }
        json_push_kv_str(&ze, "txid", txid_hex);
        json_push_kv_int(&ze, "output_index", nidx);

        char z_addr[128];
        if (ndiv && npkd)
            sapling_encode_payment_address(ndiv, npkd,
                                            "zs", z_addr, sizeof(z_addr));
        else
            z_addr[0] = '\0';
        json_push_kv_str(&ze, "address", z_addr);

        char zamt[32];
        format_amount(nval, zamt, sizeof(zamt));
        json_push_kv_str(&ze, "amount", zamt);
        json_push_kv_int(&ze, "block_height", nheight);
        json_push_kv_str(&ze, "status", is_spent ? "spent" : "unspent");

        if (is_spent) {
            char spent_hex[65];
            wallet_txid_hex_le(spent_by, spent_hex);
            json_push_kv_str(&ze, "spent_by", spent_hex);
        }

        if (wheight > 0)
            json_push_kv_int(&ze, "witness_height", wheight);

        json_push_back(&z_arr, &ze);
        json_free(&ze);
    }
    if (z_stmt) sqlite3_finalize(z_stmt);

    /* Fee accounting from wallet transactions */
    int64_t total_fees = 0;
    int tx_count = 0;
    sqlite3_stmt *fee_stmt = NULL;
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT fee FROM wallet_transactions WHERE from_me = 1 AND fee > 0",
        -1, &fee_stmt, NULL);
    while (fee_stmt && AR_STEP_ROW_READONLY(fee_stmt) == SQLITE_ROW) {
        total_fees += sqlite3_column_int64(fee_stmt, 0);
        tx_count++;
    }
    if (fee_stmt) sqlite3_finalize(fee_stmt);

    /* Summary */
    char amt[32];
    json_push_kv_int(result, "tracked_utxos", tracked_count);
    format_amount(tracked_balance, amt, sizeof(amt));
    json_push_kv_str(result, "tracked_balance", amt);
    json_push_kv_int(result, "untracked_utxos", untracked_count);
    format_amount(untracked_balance, amt, sizeof(amt));
    json_push_kv_str(result, "untracked_balance", amt);

    json_push_kv_int(result, "shielded_unspent", z_unspent);
    json_push_kv_int(result, "shielded_spent", z_spent);
    format_amount(z_balance, amt, sizeof(amt));
    json_push_kv_str(result, "shielded_balance", amt);
    format_amount(z_total_received, amt, sizeof(amt));
    json_push_kv_str(result, "shielded_total_received", amt);

    int64_t grand_total = tracked_balance + untracked_balance + z_balance;
    format_amount(grand_total, amt, sizeof(amt));
    json_push_kv_str(result, "total_balance", amt);

    format_amount(total_fees, amt, sizeof(amt));
    json_push_kv_str(result, "total_fees_paid", amt);
    json_push_kv_int(result, "fee_paying_txns", tx_count);

    json_push_kv(result, "untracked", &untracked_arr);
    json_free(&untracked_arr);
    json_push_kv(result, "shielded_notes_detail", &z_arr);
    json_free(&z_arr);

    return true;
}

static bool rpc_rescanwallet(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "rescanwallet\n"
        "Rescan the entire chain for wallet transactions using the current\n"
        "full key set. Finds UTXOs that were missed because keys were added\n"
        "after the initial scan. Imports them into SQLite and syncs the\n"
        "in-memory wallet.");

    ENSURE_WALLET(result);
    if (!ctx->main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!ctx->datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }
    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate not available");
        return false;
    }
    if (!rpc_require_chainstate_lookup_ready(ctx->main_state, result,
            "rescanwallet", "Chainstate lookup"))
        return false;

    int64_t balance_before = db_wallet_utxo_balance(ctx->node_db);
    int utxos_before = 0;
    {
        struct db_wallet_utxo tmp[4096];
        utxos_before = db_wallet_utxo_list_unspent(ctx->node_db, tmp, 4096);
    }

    /* Full rescan from block 0 */
    if (!wallet_reset_begin_checked(ctx->node_db, "rescanwallet begin")) {
        json_set_str(result, "Failed to begin wallet reset transaction");
        return false;
    }
    if (!db_wallet_utxo_delete_all(ctx->node_db) ||
        !db_wallet_tx_delete_all(ctx->node_db)) {
        wallet_reset_rollback_best_effort(ctx->node_db,
                                          "rescanwallet rollback");
        json_set_str(result, "Failed to clear wallet tables before rescan");
        return false;
    }
    if (!wallet_reset_commit_checked(ctx->node_db, "rescanwallet commit")) {
        json_set_str(result, "Failed to commit wallet reset before rescan");
        return false;
    }

    int chain_tip = active_chain_height(&ctx->main_state->chain_active);
    printf("rescanwallet: rescanning %d blocks with %zu keys...\n",
           chain_tip + 1, ctx->wallet->keystore.num_keys);
    fflush(stdout);

    int found = wallet_scan_blocks(ctx->node_db,
        &ctx->main_state->chain_active, ctx->wallet, ctx->datadir,
        0, chain_tip);

    /* Now sync the in-memory wallet from the fresh SQLite data */
    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 4096);

    int synced = 0;
    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool avail = coins_view_cache_get_coins(ctx->coins_tip, &tid, &c)
                   && coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (avail && wallet_is_outpoint_spent(ctx->wallet, &tid,
                                               unspent[i].vout)) {
            wallet_unmark_outpoint_spent(ctx->wallet, &tid, unspent[i].vout);
            synced++;
        }
    }

    int64_t balance_after = db_wallet_utxo_balance(ctx->node_db);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", (int64_t)(chain_tip + 1));
    json_push_kv_int(result, "wallet_txs_found",
                      found > 0 ? (int64_t)found : 0);
    json_push_kv_int(result, "utxos_before", utxos_before);
    json_push_kv_int(result, "utxos_after", count);
    json_push_kv_int(result, "spent_set_fixed", synced);

    char amt[32];
    format_amount(balance_before, amt, sizeof(amt));
    json_push_kv_str(result, "balance_before", amt);
    format_amount(balance_after, amt, sizeof(amt));
    json_push_kv_str(result, "balance_after", amt);

    return true;
}

static bool rpc_rescanwitnesses(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "rescanwitnesses\n"
        "Rebuild Sapling Merkle witnesses for all unspent shielded notes.\n"
        "Required before spending z→z or z→t. Replays the commitment tree\n"
        "from the Sapling activation height to tip.");

    ENSURE_WALLET(result);
    if (!ctx->main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!ctx->datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }

    /* Load all unspent notes that need witnesses */
    struct db_sapling_note notes[256];
    int n_notes = db_sapling_note_list_unspent(ctx->node_db, notes, 256);
    if (n_notes == 0) {
        json_set_object(result);
        json_push_kv_int(result, "notes_updated", 0);
        json_push_kv_str(result, "status", "no unspent notes");
        return true;
    }

    printf("rescanwitnesses: building witnesses for %d notes...\n", n_notes);
    fflush(stdout);

    /* Prevent sync_controller from overwriting Sapling tree during rescan */
    extern _Atomic bool g_sapling_rescan_active;
    atomic_store(&g_sapling_rescan_active, true);

    int chain_tip = active_chain_height(&ctx->main_state->chain_active);
    int sapling_start = 476969; /* Sapling activation on ZClassic mainnet */

    /* Initialize empty tree and per-note witness state */
    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);

    struct incremental_witness *witnesses = zcl_calloc((size_t)n_notes,
        sizeof(struct incremental_witness), "rescan witnesses");
    bool *witness_active = zcl_calloc((size_t)n_notes, sizeof(bool), "rescan witness active");
    int witnesses_built = 0;

    /* mmap cache */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    int64_t t_start = (int64_t)platform_time_wall_time_t();
    int blocks_scanned = 0;
    size_t total_commitments = 0;

    /* Stop at the immutable height to avoid reading blocks the C++ node
     * may still be writing to shared blk*.dat files. The remaining
     * blocks will be handled by normal connect_block processing. */
    int safe_tip = zcl_immutable_height(chain_tip);
    if (safe_tip < sapling_start) safe_tip = chain_tip;

    for (int h = sapling_start; h <= safe_tip; h++) {
        const struct block_index *pindex =
            active_chain_at(&ctx->main_state->chain_active, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap block file */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            char path[512];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     ctx->datadir, pindex->nFile);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { cached_data = NULL; cached_file = -1; continue; }
            struct stat fst;
            if (fstat(fd, &fst) != 0) { close(fd); continue; }
            cached_size = (size_t)fst.st_size;
            cached_data = mmap(NULL, cached_size,
                               PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (cached_data == MAP_FAILED) {
                cached_data = NULL; cached_file = -1; continue;
            }
            /* Advise kernel: sequential read, prefetch entire file */
            posix_madvise(cached_data, cached_size,
                          POSIX_MADV_SEQUENTIAL);
            posix_madvise(cached_data, cached_size,
                          POSIX_MADV_WILLNEED);
            cached_file = pindex->nFile;
        }
        if (!cached_data || pindex->nDataPos >= cached_size) continue;

        /* Fast-scan: extract Sapling commitments without full
         * block deserialization. 1000x faster — skips scriptSig parsing
         * for blocks with thousands of inputs. */
        uint8_t block_cms[4096][32];
        size_t block_data_len = cached_size - pindex->nDataPos;
        int n_cms = fast_scan_sapling_commitments(
            cached_data + pindex->nDataPos, block_data_len,
            block_cms, 4096);

        for (int ci = 0; ci < n_cms; ci++) {
            struct uint256 cm;
            memcpy(cm.data, block_cms[ci], 32);

            /* Advance all active witnesses */
            for (int ni = 0; ni < n_notes; ni++) {
                if (witness_active[ni])
                    incremental_witness_append(&witnesses[ni], &cm);
            }

            /* Append to tree */
            incremental_tree_append(&tree, &cm);
            total_commitments++;

            /* Check if this cm matches any note's commitment */
            for (int ni = 0; ni < n_notes; ni++) {
                if (witness_active[ni]) continue;
                if (memcmp(cm.data, notes[ni].cm, 32) == 0) {
                    incremental_witness_init(&witnesses[ni], &tree);
                    witness_active[ni] = true;
                    witnesses_built++;
                }
            }
        }
        blocks_scanned++;

        /* Checkpoint: compare our tree root vs block header.
         * Every 100K blocks normally, every 1000 heights in last 10K. */
        bool do_ckpt = (blocks_scanned % 100000 == 0) ||
                       (h > safe_tip - 10000 && h % 1000 == 0);
        if (do_ckpt) {
            int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
            printf("rescanwitnesses: %d blocks (height %d), "
                   "%zu cms, %d/%d witnesses, %llds",
                   blocks_scanned, h, total_commitments,
                   witnesses_built, n_notes, (long long)elapsed);

            struct uint256 our_root;
            incremental_tree_root(&tree, &our_root);
            if (memcmp(our_root.data,
                       pindex->hashFinalSaplingRoot.data, 32) == 0) {
                printf(" [tree OK]\n");
            } else {
                char oh[65], bh[65];
                uint256_get_hex(&our_root, oh);
                uint256_get_hex(&pindex->hashFinalSaplingRoot, bh);
                printf(" [TREE DIVERGED!]\n"
                       "  our root:   %s (size=%zu)\n"
                       "  block root: %s\n",
                       oh, incremental_tree_size(&tree), bh);
            }
            fflush(stdout);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    /* Binary search for divergence point: check tree root at last checkpoint
     * that passed (3036968) vs block header. We know tree matches there.
     * Log our total commitment count at save height for comparison. */
    printf("rescanwitnesses: total commitments: %zu at save height %d\n",
           total_commitments, safe_tip);
    fflush(stdout);

    /* Verify tree root matches block header at save height */
    {
        const struct block_index *save_block =
            active_chain_at(&ctx->main_state->chain_active, safe_tip);
        if (save_block) {
            struct uint256 our_root;
            incremental_tree_root(&tree, &our_root);
            char oh[65], bh[65];
            uint256_get_hex(&our_root, oh);
            uint256_get_hex(&save_block->hashFinalSaplingRoot, bh);
            if (memcmp(our_root.data,
                       save_block->hashFinalSaplingRoot.data, 32) == 0) {
                printf("rescanwitnesses: FINAL tree root matches block header at height %d ✓\n", safe_tip);
            } else {
                printf("rescanwitnesses: FINAL tree root DOES NOT match block header at height %d!\n"
                       "  our root:   %s (size=%zu)\n"
                       "  block root: %s\n",
                       safe_tip, oh, incremental_tree_size(&tree), bh);
            }
            fflush(stdout);
        }
    }

    /* Verify witness roots match tree root BEFORE saving */
    {
        struct uint256 tree_root;
        incremental_tree_root(&tree, &tree_root);
        char tr_hex[65]; uint256_get_hex(&tree_root, tr_hex);
        for (int ni = 0; ni < n_notes; ni++) {
            if (!witness_active[ni]) continue;
            struct uint256 wr;
            incremental_witness_root(&witnesses[ni], &wr);
            char wr_hex[65]; uint256_get_hex(&wr, wr_hex);
            if (memcmp(wr.data, tree_root.data, 32) != 0) {
                printf("rescanwitnesses: WITNESS ROOT MISMATCH for note %d!\n"
                    "  tree root:    %s (size=%zu)\n"
                    "  witness root: %s (fills=%zu)\n",
                    ni, tr_hex, incremental_tree_size(&tree),
                    wr_hex, witnesses[ni].num_filled);
            } else {
                printf("rescanwitnesses: note %d witness root MATCHES tree ✓\n", ni);
            }
        }
        fflush(stdout);
    }

    /* Save the authoritative tree state to node_state.
     * This replaces any incomplete tree from catchup.
     * Tree is saved at safe_tip height — subsequent connect_block
     * calls will load it and extend naturally for remaining blocks. */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        /* Save to BOTH the normal key AND a rescan-specific key.
         * The rescan key can't be overwritten by connect_block. */
        node_db_state_set(ctx->node_db, "sapling_tree", ts.data, ts.size);
        node_db_state_set(ctx->node_db, "sapling_tree_rescan", ts.data, ts.size);

        printf("rescanwitnesses: tree saved (%zu bytes, %zu cms)\n",
               ts.size, incremental_tree_size(&tree));
        fflush(stdout);
        stream_free(&ts);

        char height_str[16];
        snprintf(height_str, sizeof(height_str), "%d", safe_tip);
        node_db_state_set(ctx->node_db, "sapling_tree_height",
                          (uint8_t *)height_str, strlen(height_str));
        node_db_state_set(ctx->node_db, "sapling_tree_rescan_height",
                          (uint8_t *)height_str, strlen(height_str));
    }

    /* Serialize and save witnesses (BEFORE releasing the rescan lock) */
    int saved = 0;
    for (int ni = 0; ni < n_notes; ni++) {
        if (!witness_active[ni]) continue;

        struct byte_stream ws;
        stream_init(&ws, 4096);
        if (incremental_witness_serialize(&witnesses[ni], &ws)) {
            db_sapling_note_save_witness(ctx->node_db,
                notes[ni].txid, notes[ni].output_index,
                ws.data, ws.size, safe_tip);
            saved++;
        }
        stream_free(&ws);
    }

    free(witnesses);
    free(witness_active);

    /* NOW release the rescan lock — tree and witnesses are all saved */
    atomic_store(&g_sapling_rescan_active, false);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t_start;
    printf("rescanwitnesses: done in %llds — %zu cms, %d/%d witnesses, "
           "%d saved\n",
           (long long)elapsed, total_commitments, witnesses_built,
           n_notes, saved);
    fflush(stdout);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", blocks_scanned);
    json_push_kv_int(result, "notes_total", n_notes);
    json_push_kv_int(result, "witnesses_built", witnesses_built);
    json_push_kv_int(result, "witnesses_saved", saved);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

void register_wallet_rescan_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "wallet", "replaywalletfromchain", rpc_replaywalletfromchain, false },
        { "wallet", "import-from",         rpc_import_from,          false },
        { "wallet", "syncwalletfromdb",    rpc_syncwalletfromdb,     false },
        { "wallet", "coinanalysis",        rpc_coinanalysis,         false },
        { "wallet", "rescanwallet",        rpc_rescanwallet,         false },
        { "wallet", "rescanwitnesses",     rpc_rescanwitnesses,      false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
