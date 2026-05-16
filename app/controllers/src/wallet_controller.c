/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_controller.h"
#include "rpc/client.h"
#include "controllers/rpc_chainstate_guard.h"
#include "controllers/wallet_helpers.h"
#include "controllers/wallet_shielded_controller.h"
#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/wallet_rescan_controller.h"
#include "controllers/strong_params.h"
#include "wallet/wallet.h"
#include "chain/chainparams.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "wallet/wallet_sqlite.h"
#include "net/connman.h"
#include "core/hash.h"
#include "models/database.h"
#include "models/utxo.h"
#include "models/wallet_tx.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_scan.h"
#include "coins/coins_view.h"
#include "core/serialize.h"
#include "encoding/base58.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "wallet/wallet_canary.h"
#include "services/wallet_backup_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void rpc_wallet_set_state(struct wallet *w, struct main_state *ms,
                          const char *datadir, struct wallet_sqlite *wdb,
                          struct tx_mempool *mempool,
                          struct connman *connman)
{
    wallet_rpc_context_set_base(w, ms, datadir, wdb, mempool, connman);
}

void rpc_wallet_set_node_db(struct node_db *ndb)
{
    wallet_rpc_context_set_node_db(ndb);
}

void rpc_wallet_set_coins_tip(struct coins_view_cache *tip)
{
    wallet_rpc_context_set_coins_tip(tip);
}

static struct wallet_rpc_context *wallet_ctx(void)
{
    return wallet_rpc_context_current();
}

static bool wallet_ctx_db_ready(const struct wallet_rpc_context *ctx)
{
    return ctx->node_db && ctx->node_db->open;
}

/* Snapshot the tail key_id in the keystore. Used by rollback paths
 * where we need to undo a keystore add that just happened: after a
 * successful wallet_get_new_address(), the most recently inserted
 * key is at keystore.keys[num_keys-1] (both keypool and HD paths
 * append, never insert). */
static bool wallet_last_key_id(const struct wallet *w, struct key_id *out)
{
    if (!w || !out) return false;
    const struct basic_keystore *ks = &w->keystore;
    if (ks->num_keys == 0) return false;
    /* Scan backward for the first used slot — the array compacts on
     * remove, but a prior rollback could have left a hole. */
    for (size_t i = ks->num_keys; i > 0; i--) {
        if (ks->keys[i - 1].used) {
            *out = ks->keys[i - 1].keyid;
            return true;
        }
    }
    return false;
}

static bool rpc_getnewaddress(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getnewaddress\n"
        "Returns a new ZClassic address for receiving payments.");

    ENSURE_WALLET(result);

    char addr[128];
    if (!wallet_get_new_address(ctx->wallet, addr, sizeof(addr))) {
        json_set_str(result, "Error: keypool ran out");
        LOG_FAIL("wallet", "getnewaddress: keypool ran out");
    }

    /* Persist the fresh key to wallet_keys BEFORE handing the address
     * to the user. If the flush fails, roll back the keystore add
     * (we capture the new key_id from the tail) so in-memory and
     * on-disk agree. Returning an address we cannot persist is the
     * exact "lost 0.4 ZCL" bug we're fixing. */
    if (ctx->wallet_db) {
        struct key_id new_kid;
        bool have_kid = wallet_last_key_id(ctx->wallet, &new_kid);

        if (!wallet_sqlite_flush(ctx->wallet_db, ctx->wallet)) {
            if (have_kid) {
                (void)wallet_remove_key(ctx->wallet, &new_kid);
            }
            json_set_str(result,
                "Error: wallet persistence failed. New address NOT saved. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "getnewaddress: wallet_sqlite_flush failed — "
                                "rolled back keystore (had_kid=%d)", (int)have_kid);
        }

        /* Success: kick the JSON backup writer so the mirror follows. */
        wallet_backup_service_on_key_change();
    }

    json_set_str(result, addr);
    return true;
}

static bool rpc_getbalance(const struct json_value *params, bool help,
                            struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getbalance\n"
        "Returns the total available balance.");

    ENSURE_WALLET(result);

    /* Use SQLite model layer as authoritative source */
    int64_t balance = wallet_ctx_db_ready(ctx)
        ? db_wallet_utxo_balance(ctx->node_db)
        : wallet_get_balance(ctx->wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_real(result, strtod(buf, NULL));
    return true;
}

static bool rpc_getunconfirmedbalance(const struct json_value *params,
                                       bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getunconfirmedbalance\n"
        "Returns the unconfirmed balance.");

    ENSURE_WALLET(result);

    int64_t balance = wallet_get_unconfirmed_balance(ctx->wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_real(result, strtod(buf, NULL));
    return true;
}

static bool rpc_getwalletinfo(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "getwalletinfo\n"
        "Returns wallet state info.");

    ENSURE_WALLET(result);

    json_set_object(result);
    char bal[32], ubal[32], ibal[32], fee[32];
    int64_t balance = wallet_ctx_db_ready(ctx)
        ? db_wallet_utxo_balance(ctx->node_db)
        : wallet_get_balance(ctx->wallet);
    format_amount(balance, bal, sizeof(bal));
    format_amount(wallet_get_unconfirmed_balance(ctx->wallet), ubal, sizeof(ubal));
    format_amount(wallet_get_immature_balance(ctx->wallet), ibal, sizeof(ibal));
    format_amount(ctx->wallet->default_fee, fee, sizeof(fee));
    json_push_kv_real(result, "balance", strtod(bal, NULL));
    json_push_kv_real(result, "unconfirmed_balance", strtod(ubal, NULL));
    json_push_kv_real(result, "immature_balance", strtod(ibal, NULL));
    json_push_kv_int(result, "txcount", (int64_t)wallet_history_count());
    json_push_kv_int(result, "keypoolsize", (int64_t)ctx->wallet->key_pool_size);
    json_push_kv_real(result, "paytxfee", strtod(fee, NULL));

    /* Persistence health block (plan §6). Aggregates the canary
     * status + a live count query so operators and tooling can see
     * at a glance whether the wallet storage is healthy.
     *
     *   healthy = open && canary_ok && !mismatch
     *
     * A false value here is the signal that D/E/F abort paths would
     * have fired on the next restart — surface it before the user
     * sends funds to an address that won't survive reboot. */
    sqlite3 *wallet_sqlite_handle = (ctx->wallet_db && ctx->wallet_db->open)
                                      ? ctx->wallet_db->db
                                      : NULL;
    struct wallet_persistence_health h = wallet_persistence_get_health(
        wallet_sqlite_handle, (int)ctx->wallet->keystore.num_keys);

    struct json_value persistence = {0};
    json_init(&persistence);
    json_set_object(&persistence);
    json_push_kv_bool(&persistence, "healthy",
                       h.open && h.canary_ok && !h.mismatch);
    json_push_kv_bool(&persistence, "open",              h.open);
    json_push_kv_bool(&persistence, "canary_ok",         h.canary_ok);
    json_push_kv_int (&persistence, "canary_last_ok_ts", h.canary_last_ok_ts);
    json_push_kv_int (&persistence, "row_count",         h.row_count);
    json_push_kv_int (&persistence, "keystore_count",    h.keystore_count);
    json_push_kv_bool(&persistence, "mismatch",          h.mismatch);
    json_push_kv_int (&persistence, "corrupt_rows",      h.corrupt_rows);
    json_push_kv_str (&persistence, "last_error",        h.last_error);
    json_push_kv(result, "persistence", &persistence);
    return true;
}

static bool rpc_listunspent(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "listunspent ( minconf maxconf )\n"
        "Returns array of unspent transaction outputs.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int min_conf = (int)rpc_permit_int(&p, 0, "minconf", 1);
    int max_conf = (int)rpc_permit_int(&p, 1, "maxconf", 9999999);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "listunspent: invalid params"); }

    ENSURE_WALLET(result);
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "listunspent", "Chainstate lookup"))
        return false;

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    int tip = active_chain_height(&ctx->main_state->chain_active);

    json_set_array(result);

    /* SQLite model layer — authoritative UTXO source */
    if (wallet_ctx_db_ready(ctx)) {
        struct db_wallet_utxo utxos[4096];
        int n = db_wallet_utxo_list_unspent(ctx->node_db, utxos, 4096);
        for (int i = 0; i < n; i++) {
            int h = utxos[i].height;
            /* Fix height=0: look up real height from global UTXO index */
            if (h <= 0) {
                struct db_utxo global;
                if (db_utxo_find(ctx->node_db, utxos[i].txid,
                                  utxos[i].vout, &global)) {
                    h = global.height;
                    db_utxo_free(&global);
                }
            }
            int confs = (h > 0) ? tip - h + 1 : 0;
            if (confs < min_conf || confs > max_conf)
                continue;
            if (utxos[i].is_coinbase && confs < 100)
                continue;

            struct json_value entry = {0};
            json_init(&entry);
            json_set_object(&entry);

            struct uint256 txid_u;
            memcpy(txid_u.data, utxos[i].txid, 32);
            char txid_hex[65];
            uint256_get_hex(&txid_u, txid_hex);
            json_push_kv_str(&entry, "txid", txid_hex);
            json_push_kv_int(&entry, "vout", (int64_t)utxos[i].vout);

            /* Decode address from script */
            if (utxos[i].script && utxos[i].script_len > 0 &&
                utxos[i].script_len <= MAX_SCRIPT_SIZE) {
                struct script sc;
                script_init(&sc);
                memcpy(sc.data, utxos[i].script, utxos[i].script_len);
                sc.size = utxos[i].script_len;
                struct tx_destination dest;
                if (script_extract_destination(&sc, &dest)) {
                    char addr[128];
                    if (encode_destination(&dest, pk_pfx, pk_pfx_len,
                                           sc_pfx, sc_pfx_len, addr,
                                           sizeof(addr)))
                        json_push_kv_str(&entry, "address", addr);
                }
            }

            char amt_buf[32];
            format_amount(utxos[i].value, amt_buf, sizeof(amt_buf));
            json_push_kv_real(&entry, "amount", strtod(amt_buf, NULL));
            json_push_kv_int(&entry, "confirmations", (int64_t)confs);
            json_push_kv_bool(&entry, "spendable", true);
            json_push_kv_bool(&entry, "solvable", true);

            json_push_back(result, &entry);
            json_free(&entry);
            db_wallet_utxo_free(&utxos[i]);
        }
        return true;
    }

    /* Fallback: in-memory wallet */
    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(ctx->wallet, coins, &num_coins, 4096,
                           min_conf > 0, false);
    for (size_t i = 0; i < num_coins; i++) {
        if (coins[i].depth < min_conf || coins[i].depth > max_conf)
            continue;

        struct json_value entry = {0};
        json_init(&entry);
        json_set_object(&entry);

        char txid[65];
        uint256_get_hex(&coins[i].wtx->tx.hash, txid);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "vout", (int64_t)coins[i].i);

        const struct tx_out *out = &coins[i].wtx->tx.vout[coins[i].i];
        struct tx_destination dest;
        if (script_extract_destination(&out->script_pub_key, &dest)) {
            char addr[128];
            if (encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr)))
                json_push_kv_str(&entry, "address", addr);
        }

        char amt_buf[32];
        format_amount(out->value, amt_buf, sizeof(amt_buf));
        json_push_kv_real(&entry, "amount", strtod(amt_buf, NULL));
        json_push_kv_int(&entry, "confirmations", (int64_t)coins[i].depth);
        json_push_kv_bool(&entry, "spendable", coins[i].spendable);
        json_push_kv_bool(&entry, "solvable", coins[i].solvable);

        json_push_back(result, &entry);
        json_free(&entry);
    }

    return true;
}

static bool rpc_sendtoaddress(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "sendtoaddress \"address\" amount\n"
        "Send an amount to a given address.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int64_t amount = rpc_require_amount(&p, 1, "amount");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "sendtoaddress: invalid params"); }

    ENSURE_WALLET(result);

    if (amount <= 0) {
        json_set_str(result, "Invalid amount");
        LOG_FAIL("wallet", "sendtoaddress: invalid amount %lld", (long long)amount);
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dest;
    if (!decode_destination(addr_str, pk_pfx, pk_pfx_len,
                            sc_pfx, sc_pfx_len, &dest)) {
        json_set_str(result, "Invalid address");
        LOG_FAIL("wallet", "sendtoaddress: invalid address %s", addr_str);
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *error = NULL;
    if (!wallet_create_transaction(ctx->wallet, &dest, amount,
                                    &wtx, &fee, &error)) {
        json_set_str(result, error ? error : "Transaction creation failed");
        LOG_FAIL("wallet", "sendtoaddress: create tx failed: %s", error ? error : "unknown");
    }

    if (!wallet_commit_transaction(ctx->wallet, &wtx, ctx->mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "sendtoaddress: commit transaction failed");
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);

    /* Relay to peers */
    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);

    /* Persist wallet state after sending */
    if (ctx->wallet_db)
        wallet_sqlite_flush(ctx->wallet_db, ctx->wallet);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

/* ── Direct C API for wallet view controller ──────────────── */

bool wallet_direct_sendtoaddress(const char *address, int64_t amount_sat,
                                  char *txid_out, size_t txid_out_size,
                                  char *error_out, size_t error_out_size)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)txid_out_size; /* always 65 bytes for hex txid */
    if (!ctx->wallet) {
        snprintf(error_out, error_out_size, "Wallet not loaded");
        LOG_FAIL("wallet", "direct_sendtoaddress: wallet not loaded");
    }
    if (amount_sat <= 0) {
        snprintf(error_out, error_out_size, "Invalid amount");
        LOG_FAIL("wallet", "direct_sendtoaddress: invalid amount %lld", (long long)amount_sat);
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_len, sc_len;
    const unsigned char *pk = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sc = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);

    struct tx_destination dest;
    if (!decode_destination(address, pk, pk_len, sc, sc_len, &dest)) {
        snprintf(error_out, error_out_size, "Invalid address");
        LOG_FAIL("wallet", "direct_sendtoaddress: invalid address %s", address);
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *err = NULL;
    if (!wallet_create_transaction(ctx->wallet, &dest, amount_sat, &wtx, &fee, &err)) {
        snprintf(error_out, error_out_size, "%s", err ? err : "Transaction creation failed");
        LOG_FAIL("wallet", "direct_sendtoaddress: create tx failed: %s", err ? err : "unknown");
    }

    if (!wallet_commit_transaction(ctx->wallet, &wtx, ctx->mempool)) {
        snprintf(error_out, error_out_size, "Error committing transaction");
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "direct_sendtoaddress: commit transaction failed");
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);
    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);
    if (ctx->wallet_db)
        wallet_sqlite_flush(ctx->wallet_db, ctx->wallet);

    uint256_get_hex(&wtx.tx.hash, txid_out);
    return true;
}

/* RPC call to zclassicd — uses shared rpc_call_local() */
static int shield_rpc_call(const char *method, const char *params_json,
                           char *out, size_t outmax)
{
    return rpc_call_local(8232, "zcluser:zclpass", method, params_json,
                           out, outmax);
}

/* Extract a JSON string field value (simple, no nesting) */
static bool shield_json_extract(const char *json, const char *key,
                                char *out, size_t outmax)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) LOG_FAIL("wallet", "shield_json_extract: key '%s' not found", key);
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i < outmax - 1) {
            out[i] = p[i]; i++;
        }
        out[i] = '\0';
        return i > 0;
    }
    /* Non-string value (null, number) */
    size_t i = 0;
    while (p[i] && p[i] != ',' && p[i] != '}' && i < outmax - 1) {
        out[i] = p[i]; i++;
    }
    out[i] = '\0';
    return i > 0;
}

bool wallet_direct_shield(const char *z_address, int64_t amount_sat,
                           int64_t fee_sat,
                           char *opid_out, size_t opid_out_size,
                           char *error_out, size_t error_out_size)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    if (!z_address || z_address[0] != 'z') {
        snprintf(error_out, error_out_size, "Invalid z-address");
        LOG_FAIL("wallet", "direct_shield: invalid z-address");
    }

    /* Get the wallet's transparent address.
     * Derive from pubkey_hash in wallet_keys using Base58Check encoding. */
    char t_addr[128] = "";

    if (wallet_ctx_db_ready(ctx)) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(ctx->node_db->db,
                "SELECT pubkey_hash FROM wallet_keys ORDER BY rowid LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
                const unsigned char *hash = sqlite3_column_blob(s, 0);
                int hash_len = sqlite3_column_bytes(s, 0);
                if (hash && hash_len == 20) {
                    /* ZClassic t-address: 2-byte prefix {0x1C,0xB8} + 20-byte hash160 */
                    const struct chain_params *cp = chain_params_get();
                    size_t pfx_len;
                    const unsigned char *pfx = chain_params_base58_prefix(
                        cp, B58_PUBKEY_ADDRESS, &pfx_len);
                    unsigned char payload[22];
                    memcpy(payload, pfx, pfx_len);
                    memcpy(payload + pfx_len, hash, 20);
                    size_t addr_len = 0;
                    base58check_encode(payload, pfx_len + 20,
                                       t_addr, sizeof(t_addr), &addr_len);
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* Fallback: ask zclassicd for an address */
    if (!t_addr[0]) {
        char rpc_buf[4096] = "";
        if (shield_rpc_call("getaddressesbyaccount", "[\"\"]",
                            rpc_buf, sizeof(rpc_buf)) > 0) {
            /* Extract first address from result array */
            const char *q = strstr(rpc_buf, "\"result\"");
            if (q) {
                const char *start = strchr(q + 8, '"');
                if (start) {
                    start++; /* skip opening quote */
                    const char *end = strchr(start, '"');
                    if (end && end - start < 64 && end - start > 20) {
                        size_t len = (size_t)(end - start);
                        memcpy(t_addr, start, len);
                        t_addr[len] = '\0';
                    }
                }
            }
        }
    }

    if (!t_addr[0]) {
        snprintf(error_out, error_out_size,
            "No transparent address found in wallet");
        LOG_FAIL("wallet", "direct_shield: no transparent address found");
    }

    /* Build z_sendmany params:
     * z_sendmany "from_t_addr" [{"address":"zs1...","amount":X.XX}] 1 fee */
    double amount_zcl = (double)amount_sat / 1e8;
    double fee_zcl = (double)fee_sat / 1e8;
    char params[1024];
    snprintf(params, sizeof(params),
        "[\"%s\", [{\"address\":\"%s\",\"amount\":%.8f}], 1, %.8f]",
        t_addr, z_address, amount_zcl, fee_zcl);

    char buf[4096] = "";
    int rc = shield_rpc_call("z_sendmany", params, buf, sizeof(buf));
    if (rc <= 0) {
        snprintf(error_out, error_out_size,
            "Could not connect to zclassicd (port 8232). "
            "Start it with: zclassicd -daemon");
        LOG_FAIL("wallet", "direct_shield: z_sendmany RPC call failed (rc=%d)", rc);
    }

    /* Check for error in response */
    char err_msg[256] = "";
    char result_str[256] = "";
    shield_json_extract(buf, "result", result_str, sizeof(result_str));

    /* Check if result is null (error case) */
    if (strstr(buf, "\"result\":null") || strstr(buf, "\"result\": null")) {
        /* Extract error message */
        const char *emsg = strstr(buf, "\"message\"");
        if (emsg) {
            shield_json_extract(buf, "message", err_msg, sizeof(err_msg));
            snprintf(error_out, error_out_size, "%s", err_msg);
        } else {
            snprintf(error_out, error_out_size,
                "zclassicd returned an error");
        }
        LOG_FAIL("wallet", "direct_shield: z_sendmany returned error");
    }

    /* Success — result contains the opid */
    if (result_str[0]) {
        snprintf(opid_out, opid_out_size, "%s", result_str);
    } else {
        /* Try to extract opid from raw response */
        const char *opid = strstr(buf, "opid-");
        if (opid) {
            size_t i = 0;
            while (opid[i] && opid[i] != '"' && opid[i] != '}'
                   && i < opid_out_size - 1) {
                opid_out[i] = opid[i]; i++;
            }
            opid_out[i] = '\0';
        } else {
            snprintf(opid_out, opid_out_size, "submitted");
        }
    }
    return true;
}

static bool rpc_dumpprivkey(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "dumpprivkey \"address\"\n"
        "Reveals the private key corresponding to 'address'.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "dumpprivkey: invalid params"); }

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dest;
    if (!decode_destination(addr_str, pk_pfx, pk_pfx_len,
                            sc_pfx, sc_pfx_len, &dest)) {
        json_set_str(result, "Invalid address");
        LOG_FAIL("wallet", "dumpprivkey: invalid address %s", addr_str);
    }

    if (dest.type != DEST_KEY_ID) {
        json_set_str(result, "Address does not refer to a key");
        LOG_FAIL("wallet", "dumpprivkey: address %s is not a key (type=%d)", addr_str, dest.type);
    }

    struct privkey key;
    if (!wallet_dump_key(ctx->wallet, &dest.id.key, &key)) {
        json_set_str(result, "Private key for address is not known");
        LOG_FAIL("wallet", "dumpprivkey: private key not found for %s", addr_str);
    }

    size_t sec_pfx_len;
    const unsigned char *sec_pfx = chain_params_base58_prefix(
        cp, B58_SECRET_KEY, &sec_pfx_len);

    char wif[128];
    bool ok = encode_secret(&key, sec_pfx, sec_pfx_len, wif, sizeof(wif));
    memory_cleanse(key.vch, 32);

    if (!ok) {
        json_set_str(result, "Encoding failed");
        LOG_FAIL("wallet", "dumpprivkey: WIF encoding failed for %s", addr_str);
    }

    json_set_str(result, wif);
    return true;
}

/* Verify that wallet_sqlite_write_key actually persisted the given
 * key by round-tripping a read from wallet_keys. The existing
 * wallet_sqlite module has no read-single-key entry point (Agent 2
 * will add one in plan §5.2); in the interim we query directly.
 *
 * Returns true if the row was found AND the stored privkey matches
 * the supplied one byte-for-byte. Returns false on any deviation. */
static bool wallet_readback_key(sqlite3 *db,
                                 const struct pubkey *pk,
                                 const struct privkey *want)
{
    if (!db || !pk || !want) return false;
    uint8_t pkh[20];
    hash160(pk->vch, pk->size, pkh);

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT privkey FROM wallet_keys WHERE pubkey_hash=?1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(st, 1, pkh, 20, SQLITE_STATIC);
    rc = AR_STEP_ROW_READONLY(st);
    bool ok = false;
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int         n    = sqlite3_column_bytes(st, 0);
        if (blob && n == 32 && memcmp(blob, want->vch, 32) == 0) {
            ok = true;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

static bool rpc_importprivkey(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "importprivkey \"privkey\" ( \"label\" )\n"
        "\nAdds a private key and instantly indexes UTXOs from SQLite.\n"
        "\nArguments:\n"
        "1. \"privkey\"     (string, required) The private key (WIF format)\n"
        "2. \"label\"       (string, optional) An optional label\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *wif = rpc_require_str(&p, 0, "privkey");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "importprivkey: invalid params"); }

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t sec_pfx_len;
    const unsigned char *sec_pfx = chain_params_base58_prefix(
        cp, B58_SECRET_KEY, &sec_pfx_len);

    struct privkey key;
    if (!decode_secret(wif, sec_pfx, sec_pfx_len, &key)) {
        json_set_str(result, "Invalid private key encoding");
        LOG_FAIL("wallet", "importprivkey: invalid WIF encoding");
    }

    /* Derive pubkey FIRST so we can persist (and roll back) without
     * touching the keystore on a bad input. */
    struct pubkey pk;
    if (!privkey_get_pubkey(&key, &pk)) {
        memory_cleanse(key.vch, 32);
        json_set_str(result, "Failed to derive public key");
        LOG_FAIL("wallet", "importprivkey: failed to derive pubkey from privkey");
    }

    /* Plan §5.4: persist BEFORE mutating the keystore. If the write
     * fails we have not touched wallet state — simply error out. */
    if (ctx->wallet_db) {
        if (!wallet_sqlite_write_key(ctx->wallet_db, &pk, &key)) {
            memory_cleanse(key.vch, 32);
            json_set_str(result,
                "Error: wallet persistence failed. Key NOT imported. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "importprivkey: wallet_sqlite_write_key failed");
        }

        /* Readback: prove the write hit disk with the bytes we asked
         * for. A passing write + failing readback was the exact
         * footgun behind the bug this change fixes. */
        if (!wallet_readback_key(ctx->wallet_db->db, &pk, &key)) {
            memory_cleanse(key.vch, 32);
            json_set_str(result,
                "Error: wallet persistence readback mismatch. Key NOT imported. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "importprivkey: readback mismatch after write_key");
        }
    }

    /* Persistence is verified — now it is safe to surface the key in
     * the keystore. A failure here (keystore full) rolls back the
     * just-persisted row so the two stay in sync. */
    if (!wallet_import_key(ctx->wallet, &key)) {
        if (ctx->wallet_db) {
            /* Best-effort: delete the row we just wrote. Failure is
             * tracked by the canary on next boot. */
            uint8_t pkh[20];
            hash160(pk.vch, pk.size, pkh);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(ctx->wallet_db->db,
                    "DELETE FROM wallet_keys WHERE pubkey_hash=?1",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_blob(st, 1, pkh, 20, SQLITE_STATIC);
                /* Best-effort DELETE rolling back the wallet_key row
                 * we just wrote when the keystore-add failed.  rc is
                 * intentionally discarded — the canary self-test on
                 * next boot detects a lingering row. */
                (void)sqlite3_step(st);  // raw-sql-ok:best-effort-write-rollback
                sqlite3_finalize(st);
            }
        }
        memory_cleanse(key.vch, 32);
        json_set_str(result, "Error adding key to wallet");
        LOG_FAIL("wallet", "importprivkey: failed to add key to wallet (keystore full?)");
    }

    if (ctx->wallet->time_first_key == 0)
        ctx->wallet->time_first_key = GetTime();

    /* Trigger JSON backup of the fresh wallet state. */
    if (ctx->wallet_db)
        wallet_backup_service_on_key_change();

    memory_cleanse(key.vch, 32);

    /* Instant UTXO index lookup — no rescan needed.
     * Hash160(pubkey) → query utxos table → copy to wallet_utxos. */
    uint8_t addr_hash[20];
    hash160(pk.vch, pk.size, addr_hash);

    if (ctx->node_db) {
        struct db_utxo utxos[512];
        int found = db_utxo_list_for_address(ctx->node_db, addr_hash,
                                              utxos, 512);
        for (int i = 0; i < found; i++) {
            struct db_utxo full;
            if (!db_utxo_find(ctx->node_db, utxos[i].txid, utxos[i].vout,
                              &full))
                continue;

            struct db_wallet_utxo wu;
            memset(&wu, 0, sizeof(wu));
            memcpy(wu.txid, full.txid, 32);
            wu.vout = full.vout;
            wu.value = full.value;
            memcpy(wu.address_hash, addr_hash, 20);
            wu.script = full.script;
            wu.script_len = full.script_len;
            wu.height = full.height;
            wu.is_coinbase = full.is_coinbase;

            db_wallet_utxo_save(ctx->node_db, &wu);
            db_utxo_free(&full);
        }

        int64_t bal = db_utxo_balance_for_address(ctx->node_db, addr_hash);
        printf("importprivkey: %d UTXOs, balance %.8f ZCL (instant)\n",
               found, (double)bal / 1e8);
    }

    json_set_null(result);
    return true;
}

static bool rpc_importaddress(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "importaddress \"address\"\n"
        "\nWatch a transparent address without importing its private key.\n"
        "Tracks balance and transactions but cannot spend.\n"
        "\nArguments:\n"
        "1. \"address\"     (string, required) The transparent address\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "importaddress: invalid params"); }

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dest;
    if (!decode_destination(addr_str, pk_pfx, pk_pfx_len,
                            sc_pfx, sc_pfx_len, &dest)) {
        json_set_str(result, "Invalid address");
        LOG_FAIL("wallet", "importaddress: invalid address %s", addr_str);
    }

    if (dest.type != DEST_KEY_ID) {
        json_set_str(result, "Only transparent P2PKH addresses supported");
        LOG_FAIL("wallet", "importaddress: address %s is not P2PKH (type=%d)", addr_str, dest.type);
    }

    /* Already have the private key? Skip — nothing to do. */
    if (keystore_have_key(&ctx->wallet->keystore, &dest.id.key)) {
        json_set_str(result, "Address already in wallet with private key");
        LOG_FAIL("wallet", "importaddress: address %s already has private key", addr_str);
    }

    /* Add as watch-only to keystore. */
    zcl_mutex_lock(&ctx->wallet->cs);
    bool ok = keystore_add_watch_only_id(&ctx->wallet->keystore, &dest.id.key);
    zcl_mutex_unlock(&ctx->wallet->cs);

    if (!ok) {
        json_set_str(result, "Error: watch-only keystore full");
        LOG_FAIL("wallet", "importaddress: watch-only keystore full for %s", addr_str);
    }

    /* Persist to wallet DB. On failure, roll back the keystore add
     * so user state doesn't diverge from disk. */
    if (ctx->wallet_db) {
        if (!wallet_sqlite_write_watch_only(ctx->wallet_db,
                                             dest.id.key.id.data, addr_str)) {
            zcl_mutex_lock(&ctx->wallet->cs);
            (void)keystore_remove_watch_only(&ctx->wallet->keystore, &dest.id.key);
            zcl_mutex_unlock(&ctx->wallet->cs);
            json_set_str(result,
                "Error: wallet persistence failed. Watch-only address NOT saved. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "importaddress: wallet_sqlite_write_watch_only failed "
                                "for %s — rolled back keystore", addr_str);
        }
        wallet_backup_service_on_key_change();
    }

    /* Instant UTXO index lookup — same as importprivkey. */
    uint8_t addr_hash[20];
    memcpy(addr_hash, dest.id.key.id.data, 20);

    int found = 0;
    int64_t bal = 0;
    if (ctx->node_db) {
        struct db_utxo utxos[512];
        found = db_utxo_list_for_address(ctx->node_db, addr_hash,
                                          utxos, 512);
        for (int i = 0; i < found; i++) {
            struct db_utxo full;
            if (!db_utxo_find(ctx->node_db, utxos[i].txid, utxos[i].vout,
                              &full))
                continue;

            struct db_wallet_utxo wu;
            memset(&wu, 0, sizeof(wu));
            memcpy(wu.txid, full.txid, 32);
            wu.vout = full.vout;
            wu.value = full.value;
            memcpy(wu.address_hash, addr_hash, 20);
            wu.script = full.script;
            wu.script_len = full.script_len;
            wu.height = full.height;
            wu.is_coinbase = full.is_coinbase;

            db_wallet_utxo_save(ctx->node_db, &wu);
            db_utxo_free(&full);
        }

        bal = db_utxo_balance_for_address(ctx->node_db, addr_hash);
    }

    printf("importaddress: %s watch-only, %d UTXOs, balance %.8f ZCL\n",
           addr_str, found, (double)bal / 1e8);

    json_set_object(result);
    json_push_kv_str(result, "address", addr_str);
    json_push_kv_bool(result, "watch_only", true);
    json_push_kv_int(result, "utxos", found);
    json_push_kv_real(result, "balance", (double)bal / 1e8);
    return true;
}

static bool rpc_rescanblockchain(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "rescanblockchain ( start_height stop_height )\n"
        "\nRescan the local blockchain for wallet transactions.\n"
        "\nArguments:\n"
        "1. start_height  (numeric, optional, default=0) Block height to start\n"
        "2. stop_height   (numeric, optional, default=tip) Block height to stop\n"
        "\nResult:\n"
        "{\n"
        "  \"start_height\": n,\n"
        "  \"stop_height\": n\n"
        "}\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int start_height = (int)rpc_permit_int(&p, 0, "start_height", 0);
    int stop_height = (int)rpc_permit_int(&p, 1, "stop_height", -1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "rescanblockchain: invalid params"); }

    ENSURE_WALLET(result);

    if (!ctx->main_state) {
        json_set_str(result, "Chain state not initialized");
        LOG_FAIL("wallet", "rescanblockchain: chain state not initialized");
    }
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "rescanblockchain",
            "Chainstate lookup"))
        return false;

    int tip = active_chain_height(&ctx->main_state->chain_active);
    if (stop_height < 0 || stop_height > tip)
        stop_height = tip;
    if (start_height < 0)
        start_height = 0;

    if (start_height > tip) {
        json_set_str(result, "start_height exceeds chain tip");
        LOG_FAIL("wallet", "rescanblockchain: start_height %d exceeds tip %d", start_height, tip);
    }

    wallet_rescan(ctx->wallet, &ctx->main_state->chain_active,
                  start_height, stop_height, ctx->datadir);

    json_set_object(result);
    json_push_kv_int(result, "start_height", start_height);
    json_push_kv_int(result, "stop_height", stop_height);
    return true;
}

static bool rpc_keypoolrefill(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "keypoolrefill ( newsize )\nFills the keypool.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    unsigned int new_size = (unsigned int)rpc_permit_int(&p, 0, "newsize", DEFAULT_KEYPOOL_SIZE);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "keypoolrefill: invalid params"); }

    ENSURE_WALLET(result);

    if (!wallet_top_up_key_pool(ctx->wallet, new_size)) {
        json_set_str(result, "Error refilling keypool");
        LOG_FAIL("wallet", "keypoolrefill: failed to refill keypool (size=%u)", new_size);
    }

    /* Flush the fresh keypool entries. If persistence fails the
     * keypool indices still point into the keystore, but the on-disk
     * rows won't exist — on next restart the node would hand out a
     * pre-existing address twice. Log and error; canary will flag
     * the daemon as unhealthy and operator can intervene. */
    if (ctx->wallet_db) {
        if (!wallet_sqlite_flush(ctx->wallet_db, ctx->wallet)) {
            json_set_str(result,
                "Error: keypool refilled in memory but persistence flush failed. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet", "keypoolrefill: wallet_sqlite_flush failed "
                                "(new_size=%u)", new_size);
        }
        wallet_backup_service_on_keypool_topup();
    }

    json_set_null(result);
    return true;
}

static bool rpc_listtransactions(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "listtransactions ( \"account\" count skip )\n"
        "Returns up to 'count' most recent transactions.\n"
        "Arguments:\n"
        "1. \"account\"  (string, optional) DEPRECATED. Ignored.\n"
        "2. count       (numeric, optional, default=10)\n"
        "3. skip        (numeric, optional, default=0)");

    ENSURE_WALLET(result);

    /* C++ API: listtransactions "account" count skip
     * params[0] is the account name (string, ignored).
     * params[1] is count, params[2] is skip.
     * For backward compat, if params[0] is numeric, treat as count. */
    int count = 10;
    int skip = 0;
    int param_offset = 0;
    if (json_size(params) >= 1) {
        const struct json_value *p0 = json_at(params, 0);
        if (p0 && p0->type == JSON_STR)
            param_offset = 1; /* skip account name */
        else if (p0)
            count = (int)json_get_int(p0);
    }
    if (json_size(params) >= (size_t)(param_offset + 1))
        count = (int)json_get_int(json_at(params, param_offset));
    if (json_size(params) >= (size_t)(param_offset + 2))
        skip = (int)json_get_int(json_at(params, param_offset + 1));
    if (count < 0) count = 0;
    if (skip < 0) skip = 0;

    json_set_array(result);
    if (count == 0)
        return true;

    if (wallet_history_db_ready()) {
        struct db_wallet_tx *rows =
            zcl_calloc((size_t)count, sizeof(struct db_wallet_tx), "listtransactions rows");
        if (!rows) {
            json_set_str(result, "Out of memory");
            LOG_FAIL("wallet", "listtransactions: alloc failed for %d rows", count);
        }

        int n = db_wallet_tx_list(ctx->node_db, rows, (size_t)count, (size_t)skip);
        for (int i = 0; i < n; i++) {
            struct transaction tx;
            if (wallet_db_tx_deserialize(&rows[i], &tx)) {
                wallet_append_tx_entry(&tx, rows[i].from_me, rows[i].fee,
                                       wallet_db_tx_confirmations(&rows[i]),
                                       rows[i].time_received, result);
                transaction_free(&tx);
            }
            db_wallet_tx_free(&rows[i]);
        }
        free(rows);
        return true;
    }

    int seen = 0;
    int added = 0;
    for (size_t i = 0; i < MAX_WALLET_TX && added < count; i++) {
        if (!ctx->wallet->map_wallet[i].used)
            continue;
        if (seen++ < skip)
            continue;
        const struct wallet_tx *wtx = &ctx->wallet->map_wallet[i];
        wallet_append_tx_entry(&wtx->tx, wtx->from_me, 0, wtx->confirms,
                               wtx->time_received, result);
        added++;
    }

    return true;
}

static bool rpc_gettransaction(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "gettransaction \"txid\"\n"
        "Get detailed information about wallet transaction.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet", "gettransaction: invalid params"); }

    ENSURE_WALLET(result);
    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

    if (wallet_ctx_db_ready(ctx)) {
        struct db_wallet_tx dbtx;
        if (db_wallet_tx_find(ctx->node_db, txid.data, &dbtx)) {
            struct transaction tx;
            if (!wallet_db_tx_deserialize(&dbtx, &tx)) {
                db_wallet_tx_free(&dbtx);
                json_set_str(result, "Failed to decode wallet transaction");
                LOG_FAIL("wallet", "gettransaction: failed to deserialize tx %s", txid_str);
            }

            json_set_object(result);

            int64_t credit = 0;
            for (size_t j = 0; j < tx.num_vout; j++) {
                if (wallet_is_mine(ctx->wallet, &tx.vout[j]))
                    credit += tx.vout[j].value;
            }
            int64_t debit = dbtx.from_me
                ? (transaction_get_value_out(&tx) + dbtx.fee)
                : 0;
            int64_t net = credit - debit;
            char net_str[32];
            format_amount(net, net_str, sizeof(net_str));
            json_push_kv_real(result, "amount", strtod(net_str, NULL));
            json_push_kv_int(result, "confirmations",
                             (int64_t)wallet_db_tx_confirmations(&dbtx));

            char hex_txid[65];
            uint256_get_hex(&tx.hash, hex_txid);
            json_push_kv_str(result, "txid", hex_txid);

            json_push_kv_int(result, "time", dbtx.time_received);
            json_push_kv_int(result, "timereceived", dbtx.time_received);

            if (dbtx.has_block) {
                char bhash[65];
                struct uint256 bh;
                memcpy(bh.data, dbtx.block_hash, 32);
                uint256_get_hex(&bh, bhash);
                json_push_kv_str(result, "blockhash", bhash);
            }

            if (dbtx.from_me && dbtx.fee > 0) {
                char fee_str[32];
                format_amount(-dbtx.fee, fee_str, sizeof(fee_str));
                json_push_kv_real(result, "fee", strtod(fee_str, NULL));
            }

            transaction_free(&tx);
            db_wallet_tx_free(&dbtx);
            return true;
        }
    }

    const struct wallet_tx *wtx = wallet_get_tx(ctx->wallet, &txid);
    if (!wtx) {
        json_set_str(result, "Invalid or non-wallet transaction id");
        LOG_FAIL("wallet", "gettransaction: tx %s not found in wallet", txid_str);
    }

    json_set_object(result);

    int64_t credit = 0;
    int64_t debit = wallet_get_debit(ctx->wallet, &wtx->tx);
    for (size_t j = 0; j < wtx->tx.num_vout; j++) {
        if (wallet_is_mine(ctx->wallet, &wtx->tx.vout[j]))
            credit += wtx->tx.vout[j].value;
    }

    int64_t net = credit - debit;
    char net_str[32];
    format_amount(net, net_str, sizeof(net_str));
    json_push_kv_real(result, "amount", strtod(net_str, NULL));
    json_push_kv_int(result, "confirmations", (int64_t)wtx->confirms);

    char hex_txid[65];
    uint256_get_hex(&wtx->tx.hash, hex_txid);
    json_push_kv_str(result, "txid", hex_txid);

    json_push_kv_int(result, "time", wtx->time_received);
    json_push_kv_int(result, "timereceived", wtx->time_received);

    if (!uint256_is_null(&wtx->hash_block)) {
        char bhash[65];
        uint256_get_hex(&wtx->hash_block, bhash);
        json_push_kv_str(result, "blockhash", bhash);
    }

    return true;
}

static bool rpc_createmultisig(const struct json_value *params, bool help,
                                struct json_value *result)
{
    RPC_HELP(help, result,
        "createmultisig nrequired [\"key\",...]\n"
        "Creates a multi-signature address with n required of m keys.\n"
        "Returns JSON with \"address\" and \"redeemScript\".");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        LOG_FAIL("wallet", "createmultisig: expected at least 2 params, got %zu", json_size(params));
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    int n_required = (int)rpc_require_int(&p, 0, "nrequired");
    const struct json_value *keys_arr = json_at(params, 1);
    if (!keys_arr || keys_arr->type != JSON_ARR || json_size(keys_arr) == 0) {
        json_set_str(result, "keys must be a non-empty array");
        LOG_FAIL("wallet", "createmultisig: keys must be a non-empty array");
    }

    size_t n_keys = json_size(keys_arr);
    if (n_required < 1 || n_required > (int)n_keys || n_keys > 16) {
        json_set_str(result, "Invalid nrequired or too many keys");
        LOG_FAIL("wallet", "createmultisig: invalid nrequired=%d for %zu keys", n_required, n_keys);
    }

    struct pubkey pks[16];
    for (size_t i = 0; i < n_keys; i++) {
        const char *hex = json_get_str(json_at(keys_arr, i));
        if (!hex) {
            json_set_str(result, "Invalid key in array");
            LOG_FAIL("wallet", "createmultisig: NULL key at index %zu", i);
        }
        size_t hex_len = strlen(hex);
        if (hex_len != 66 && hex_len != 130) {
            json_set_str(result, "Invalid public key length");
            LOG_FAIL("wallet", "createmultisig: bad key length %zu at index %zu", hex_len, i);
        }
        unsigned char buf[65];
        size_t buf_len = ParseHex(hex, buf, sizeof(buf));
        if (buf_len != 33 && buf_len != 65) {
            json_set_str(result, "Invalid hex in key");
            LOG_FAIL("wallet", "createmultisig: invalid hex at index %zu", i);
        }
        pubkey_set(&pks[i], buf, buf_len);
        if (!pubkey_is_valid(&pks[i])) {
            json_set_str(result, "Invalid public key (not a valid EC point)");
            LOG_FAIL("wallet", "createmultisig: invalid EC point at index %zu", i);
        }
    }

    struct script redeem;
    script_for_multisig(&redeem, n_required, pks, n_keys);

    struct script_id sid;
    script_id_from_script(&sid, &redeem);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dest;
    dest.type = DEST_SCRIPT_ID;
    dest.id.script = sid;
    char addr[128];
    encode_destination(&dest, pk_pfx, pk_pfx_len,
                       sc_pfx, sc_pfx_len, addr, sizeof(addr));

    char redeem_hex[MAX_SCRIPT_SIZE * 2 + 1];
    HexStr(redeem.data, redeem.size, false, redeem_hex, sizeof(redeem_hex));

    json_set_object(result);
    json_push_kv_str(result, "address", addr);
    json_push_kv_str(result, "redeemScript", redeem_hex);
    return true;
}

static bool rpc_sendmany(const struct json_value *params, bool help,
                          struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "sendmany \"\" {\"address\":amount,...}\n"
        "Send to multiple addresses in one transaction.\n"
        "First argument must be \"\" (empty string).\n"
        "Second argument is a JSON object of address:amount pairs.");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        LOG_FAIL("wallet", "sendmany: expected at least 2 params, got %zu", json_size(params));
    }

    ENSURE_WALLET(result);

    const struct json_value *amounts = json_at(params, 1);
    if (!amounts || amounts->type != JSON_OBJ) {
        json_set_str(result, "amounts must be a JSON object");
        LOG_FAIL("wallet", "sendmany: amounts param is not a JSON object");
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dests[256];
    int64_t values[256];
    size_t n = 0;

    for (size_t i = 0; i < json_size(amounts) && n < 256; i++) {
        const char *addr = amounts->keys ? amounts->keys[i] : NULL;
        const struct json_value *val = json_at(amounts, i);
        if (!addr || !val) continue;

        if (!decode_destination(addr, pk_pfx, pk_pfx_len,
                                sc_pfx, sc_pfx_len, &dests[n])) {
            json_set_str(result, "Invalid address");
            LOG_FAIL("wallet", "sendmany: invalid address at index %zu", i);
        }

        values[n] = parse_amount(val);
        if (values[n] <= 0) {
            json_set_str(result, "Invalid amount");
            LOG_FAIL("wallet", "sendmany: invalid amount at index %zu", i);
        }
        n++;
    }

    if (n == 0) {
        json_set_str(result, "No recipients");
        LOG_FAIL("wallet", "sendmany: no recipients specified");
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *error = NULL;
    if (!wallet_create_transaction_multi(ctx->wallet, dests, values, n,
                                          &wtx, &fee, &error)) {
        json_set_str(result, error ? error : "Transaction creation failed");
        LOG_FAIL("wallet", "sendmany: create multi-tx failed (%zu recipients): %s", n, error ? error : "unknown");
    }

    if (!wallet_commit_transaction(ctx->wallet, &wtx, ctx->mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet", "sendmany: commit transaction failed (%zu recipients)", n);
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);

    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);

    if (ctx->wallet_db)
        wallet_sqlite_flush(ctx->wallet_db, ctx->wallet);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

static bool rpc_addmultisigaddress(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "addmultisigaddress nrequired [\"key\",...]\n"
        "Add a multisig address to the wallet.\n"
        "Each key is a hex-encoded public key.\n"
        "The redeem script is stored in the wallet for spending.");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        LOG_FAIL("wallet", "addmultisigaddress: expected at least 2 params, got %zu", json_size(params));
    }

    ENSURE_WALLET(result);

    struct rpc_params p;
    rpc_params_init(&p, params);
    int n_required = (int)rpc_require_int(&p, 0, "nrequired");
    const struct json_value *keys_arr = json_at(params, 1);
    if (!keys_arr || keys_arr->type != JSON_ARR || json_size(keys_arr) == 0) {
        json_set_str(result, "keys must be a non-empty array");
        LOG_FAIL("wallet", "addmultisigaddress: keys must be a non-empty array");
    }

    size_t n_keys = json_size(keys_arr);
    if (n_required < 1 || n_required > (int)n_keys || n_keys > 16) {
        json_set_str(result, "Invalid nrequired or too many keys");
        LOG_FAIL("wallet", "addmultisigaddress: invalid nrequired=%d for %zu keys", n_required, n_keys);
    }

    struct pubkey pks[16];
    for (size_t i = 0; i < n_keys; i++) {
        const char *hex = json_get_str(json_at(keys_arr, i));
        if (!hex) {
            json_set_str(result, "Invalid key in array");
            LOG_FAIL("wallet", "addmultisigaddress: NULL key at index %zu", i);
        }
        unsigned char buf[65];
        size_t buf_len = ParseHex(hex, buf, sizeof(buf));
        if (buf_len != 33 && buf_len != 65) {
            json_set_str(result, "Invalid public key");
            LOG_FAIL("wallet", "addmultisigaddress: bad key length %zu at index %zu", buf_len, i);
        }
        pubkey_set(&pks[i], buf, buf_len);
        if (!pubkey_is_valid(&pks[i])) {
            json_set_str(result, "Invalid public key (not a valid EC point)");
            LOG_FAIL("wallet", "addmultisigaddress: invalid EC point at index %zu", i);
        }
    }

    struct script redeem;
    script_for_multisig(&redeem, n_required, pks, n_keys);

    /* Store redeem script in wallet keystore */
    keystore_add_cscript(&ctx->wallet->keystore, &redeem);

    struct script_id sid;
    script_id_from_script(&sid, &redeem);

    /* Persist script to wallet DB */
    if (ctx->wallet_db)
        wallet_sqlite_write_script(ctx->wallet_db, &sid.hash, &redeem);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination dest;
    dest.type = DEST_SCRIPT_ID;
    dest.id.script = sid;
    char addr[128];
    encode_destination(&dest, pk_pfx, pk_pfx_len,
                       sc_pfx, sc_pfx_len, addr, sizeof(addr));

    char redeem_hex[MAX_SCRIPT_SIZE * 2 + 1];
    HexStr(redeem.data, redeem.size, false, redeem_hex, sizeof(redeem_hex));

    json_set_object(result);
    json_push_kv_str(result, "address", addr);
    json_push_kv_str(result, "redeemScript", redeem_hex);
    return true;
}


void register_wallet_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "wallet", "getnewaddress",        rpc_getnewaddress,        false },
        { "wallet", "getbalance",           rpc_getbalance,           false },
        { "wallet", "getunconfirmedbalance", rpc_getunconfirmedbalance, false },
        { "wallet", "getwalletinfo",        rpc_getwalletinfo,        false },
        { "wallet", "listunspent",          rpc_listunspent,          false },
        { "wallet", "sendtoaddress",        rpc_sendtoaddress,        false },
        { "wallet", "dumpprivkey",          rpc_dumpprivkey,          false },
        { "wallet", "importprivkey",        rpc_importprivkey,        false },
        { "wallet", "importaddress",       rpc_importaddress,        false },
        { "wallet", "keypoolrefill",        rpc_keypoolrefill,        false },
        { "wallet", "listtransactions",     rpc_listtransactions,     false },
        { "wallet", "gettransaction",       rpc_gettransaction,       false },
        { "wallet", "rescanblockchain",     rpc_rescanblockchain,     false },
        { "wallet", "sendmany",             rpc_sendmany,             false },
        { "wallet", "createmultisig",       rpc_createmultisig,       false },
        { "wallet", "addmultisigaddress",   rpc_addmultisigaddress,   false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);

    /* Register shielded and diagnostic sub-controllers */
    register_wallet_shielded_rpc_commands(t);
    register_wallet_diagnostic_rpc_commands(t);
    register_wallet_rescan_rpc_commands(t);
}
