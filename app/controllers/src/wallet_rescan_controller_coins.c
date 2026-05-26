/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * coinanalysis RPC. Split out of wallet_rescan_controller.c (D5);
 * behavior byte-identical. */

#include "controllers/wallet_rescan_controller_internal.h"

bool rpc_coinanalysis(const struct json_value *params, bool help,
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

