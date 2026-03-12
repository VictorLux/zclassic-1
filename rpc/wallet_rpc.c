/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/wallet_rpc.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "chain/chainparams.h"
#include "encoding/utilmoneystr.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "core/random.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/txmempool.h"
#include "wallet/wallet_db.h"
#include "net/connman.h"
#include "zcash/sapling.h"
#include "zcash/fr.h"
#include "consensus/upgrades.h"
#include "db/db.h"
#include "db/model_block.h"
#include "db/model_utxo.h"
#include "db/model_wallet_key.h"
#include "db/model_wallet_tx.h"
#include "db/model_mempool.h"
#include "db/model_peer.h"
#include "db/node_db_sync.h"
#include "db/wallet_scan.h"
#include "db/legacy_import.h"
#include "core/serialize.h"
#include "coins/coins_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t json_param_int(const struct json_value *v)
{
    if (!v) return 0;
    if (v->type == JSON_INT) return json_get_int(v);
    if (v->type == JSON_STR) return strtoll(json_get_str(v), NULL, 10);
    return 0;
}

/* Integer-only amount formatting — no floating-point rounding.
 * Always produces exactly 8 decimal places (e.g. "0.99990000"). */
static void format_amount(int64_t satoshis, char *out, size_t out_size)
{
    bool neg = satoshis < 0;
    int64_t abs_val = neg ? -satoshis : satoshis;
    int64_t whole = abs_val / 100000000;
    int64_t frac = abs_val % 100000000;
    snprintf(out, out_size, "%s%lld.%08lld",
             neg ? "-" : "",
             (long long)whole, (long long)frac);
}

/* Parse a JSON value (number or string) to satoshis without floating-point
 * intermediate. Returns satoshis, or -1 on error. */
static int64_t parse_amount(const struct json_value *v)
{
    if (!v) return -1;

    /* If it's already an integer (rare but possible), use directly */
    if (v->type == JSON_INT) {
        int64_t val = json_get_int(v);
        return val * 100000000;
    }

    /* For JSON_REAL or JSON_STR, parse the decimal string precisely */
    const char *str = NULL;
    char tmp[64];
    if (v->type == JSON_STR) {
        str = json_get_str(v);
    } else if (v->type == JSON_REAL) {
        /* Convert the double to string first to get exact representation,
         * then parse the string. This avoids (double * 1e8 + 0.5) rounding. */
        snprintf(tmp, sizeof(tmp), "%.8f", json_get_real(v));
        str = tmp;
    }
    if (!str) return -1;

    /* Skip leading whitespace and sign */
    const char *p = str;
    while (*p == ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }

    /* Parse whole part */
    int64_t whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }

    /* Parse fractional part — up to 8 digits */
    int64_t frac = 0;
    int frac_digits = 0;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && frac_digits < 8) {
            frac = frac * 10 + (*p - '0');
            frac_digits++;
            p++;
        }
    }
    /* Pad remaining fractional digits with zeros */
    while (frac_digits < 8) {
        frac *= 10;
        frac_digits++;
    }

    int64_t satoshis = whole * 100000000 + frac;
    return neg ? -satoshis : satoshis;
}

static struct wallet *g_wallet = NULL;
static struct main_state *g_main_state = NULL;
static const char *g_datadir = NULL;
static struct wallet_db *g_wallet_db = NULL;
static struct tx_mempool *g_mempool = NULL;
static struct connman *g_connman_ptr = NULL;
static struct node_db *g_node_db = NULL;
static struct coins_view_cache *g_coins_tip = NULL;

static int wallet_history_count(void)
{
    int mem_count = g_wallet ? (int)g_wallet->num_wallet_tx : 0;
    int db_count = (g_node_db && g_node_db->open) ? db_wallet_tx_count(g_node_db) : 0;
    return db_count > mem_count ? db_count : mem_count;
}

static bool wallet_history_db_ready(void)
{
    if (!g_wallet || !g_node_db || !g_node_db->open)
        return false;

    int db_count = db_wallet_tx_count(g_node_db);
    if (db_count >= (int)g_wallet->num_wallet_tx)
        return true;

    return g_wallet->num_wallet_tx >= MAX_WALLET_TX;
}

static bool wallet_db_tx_deserialize(const struct db_wallet_tx *dbtx,
                                     struct transaction *tx)
{
    if (!dbtx || !dbtx->raw_tx || dbtx->raw_tx_len == 0)
        return false;

    struct byte_stream s;
    stream_init_from_data(&s, dbtx->raw_tx, dbtx->raw_tx_len);
    transaction_init(tx);
    if (!transaction_deserialize(tx, &s)) {
        transaction_free(tx);
        return false;
    }

    transaction_compute_hash(tx);
    return true;
}

static int wallet_db_tx_confirmations(const struct db_wallet_tx *dbtx)
{
    if (!dbtx || !dbtx->has_block || !g_main_state)
        return 0;

    int tip_height = active_chain_height(&g_main_state->chain_active);
    if (tip_height < dbtx->block_height)
        return 0;

    return tip_height - dbtx->block_height + 1;
}

static bool wallet_append_tx_entry(const struct transaction *tx,
                                   bool from_me,
                                   int64_t fee,
                                   int confirmations,
                                   int64_t time_received,
                                   struct json_value *result)
{
    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct json_value entry;
    json_set_object(&entry);

    char txid[65];
    uint256_get_hex(&tx->hash, txid);
    json_push_kv_str(&entry, "txid", txid);
    json_push_kv_int(&entry, "confirmations", confirmations);
    json_push_kv_int(&entry, "time", time_received);
    json_push_kv_int(&entry, "timereceived", time_received);

    if (from_me) {
        json_push_kv_str(&entry, "category", "send");
        int64_t amount = -transaction_get_value_out(tx) - fee;
        char a[32];
        format_amount(amount, a, sizeof(a));
        json_push_kv_real(&entry, "amount", strtod(a, NULL));
        if (fee > 0) {
            char f[32];
            format_amount(-fee, f, sizeof(f));
            json_push_kv_real(&entry, "fee", strtod(f, NULL));
        }
    } else {
        int64_t credit = 0;
        for (size_t j = 0; j < tx->num_vout; j++) {
            if (wallet_is_mine(g_wallet, &tx->vout[j]))
                credit += tx->vout[j].value;
        }
        json_push_kv_str(&entry, "category",
                         confirmations > 0 ? "receive" : "immature");
        char a[32];
        format_amount(credit, a, sizeof(a));
        json_push_kv_real(&entry, "amount", strtod(a, NULL));

        if (tx->num_vout > 0) {
            struct tx_destination dest;
            if (script_extract_destination(
                    &tx->vout[0].script_pub_key, &dest)) {
                char addr[128];
                if (encode_destination(&dest, pk_pfx, pk_pfx_len,
                                       sc_pfx, sc_pfx_len,
                                       addr, sizeof(addr)))
                    json_push_kv_str(&entry, "address", addr);
            }
        }
    }

    json_push_back(result, &entry);
    json_free(&entry);
    return true;
}

#define ENSURE_WALLET(result) do {                        \
    if (!g_wallet) {                                      \
        json_set_str((result), "Wallet not available");   \
        return false;                                     \
    }                                                     \
} while (0)

void rpc_wallet_set_state(struct wallet *w, struct main_state *ms,
                          const char *datadir, struct wallet_db *wdb,
                          struct tx_mempool *mempool,
                          struct connman *connman)
{
    g_wallet = w;
    g_main_state = ms;
    g_wallet_db = wdb;
    g_mempool = mempool;
    g_connman_ptr = connman;
    g_datadir = datadir;
}

void rpc_wallet_set_node_db(struct node_db *ndb)
{
    g_node_db = ndb;
}

void rpc_wallet_set_coins_tip(struct coins_view_cache *tip)
{
    g_coins_tip = tip;
}

static bool rpc_getnewaddress(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getnewaddress\n"
            "Returns a new ZClassic address for receiving payments.");
        return true;
    }

    ENSURE_WALLET(result);

    char addr[128];
    if (!wallet_get_new_address(g_wallet, addr, sizeof(addr))) {
        json_set_str(result, "Error: keypool ran out");
        return false;
    }

    /* Persist new key to wallet DB */
    if (g_wallet_db)
        wallet_db_flush(g_wallet_db, g_wallet);

    json_set_str(result, addr);
    return true;
}

static bool rpc_getbalance(const struct json_value *params, bool help,
                            struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getbalance\n"
            "Returns the total available balance.");
        return true;
    }

    ENSURE_WALLET(result);

    int64_t balance = wallet_get_balance(g_wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_real(result, strtod(buf, NULL));
    return true;
}

static bool rpc_getunconfirmedbalance(const struct json_value *params,
                                       bool help, struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getunconfirmedbalance\n"
            "Returns the unconfirmed balance.");
        return true;
    }

    ENSURE_WALLET(result);

    int64_t balance = wallet_get_unconfirmed_balance(g_wallet);
    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_real(result, strtod(buf, NULL));
    return true;
}

static bool rpc_getwalletinfo(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getwalletinfo\n"
            "Returns wallet state info.");
        return true;
    }

    ENSURE_WALLET(result);

    json_set_object(result);
    char bal[32], ubal[32], ibal[32], fee[32];
    format_amount(wallet_get_balance(g_wallet), bal, sizeof(bal));
    format_amount(wallet_get_unconfirmed_balance(g_wallet), ubal, sizeof(ubal));
    format_amount(wallet_get_immature_balance(g_wallet), ibal, sizeof(ibal));
    format_amount(g_wallet->default_fee, fee, sizeof(fee));
    json_push_kv_real(result, "balance", strtod(bal, NULL));
    json_push_kv_real(result, "unconfirmed_balance", strtod(ubal, NULL));
    json_push_kv_real(result, "immature_balance", strtod(ibal, NULL));
    json_push_kv_int(result, "txcount", (int64_t)wallet_history_count());
    json_push_kv_int(result, "keypoolsize", (int64_t)g_wallet->key_pool_size);
    json_push_kv_real(result, "paytxfee", strtod(fee, NULL));
    return true;
}

static bool rpc_listunspent(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "listunspent ( minconf maxconf )\n"
            "Returns array of unspent transaction outputs.");
        return true;
    }

    ENSURE_WALLET(result);

    int min_conf = 1;
    int max_conf = 9999999;
    if (json_size(params) >= 1)
        min_conf = (int)json_get_int(json_at(params, 0));
    if (json_size(params) >= 2)
        max_conf = (int)json_get_int(json_at(params, 1));

    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(g_wallet, coins, &num_coins, 4096,
                           min_conf > 0, false);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    json_set_array(result);
    for (size_t i = 0; i < num_coins; i++) {
        if (coins[i].depth < min_conf || coins[i].depth > max_conf)
            continue;

        /* Skip UTXO set cross-check — wallet tracks spent state.
         * Avoids concurrent access to g_coins_tip from RPC thread. */

        struct json_value entry;
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
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "sendtoaddress \"address\" amount\n"
            "Send an amount to a given address.");
        return true;
    }

    ENSURE_WALLET(result);

    const struct json_value *addr_val = json_at(params, 0);
    const struct json_value *amt_val = json_at(params, 1);
    if (!addr_val || !amt_val) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    const char *addr_str = json_get_str(addr_val);
    int64_t amount = parse_amount(amt_val);

    if (amount <= 0) {
        json_set_str(result, "Invalid amount");
        return false;
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
        return false;
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *error = NULL;
    if (!wallet_create_transaction(g_wallet, &dest, amount,
                                    &wtx, &fee, &error)) {
        json_set_str(result, error ? error : "Transaction creation failed");
        return false;
    }

    if (!wallet_commit_transaction(g_wallet, &wtx, g_mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        return false;
    }

    if (g_node_db && g_node_db->open)
        node_db_sync_wallet_tx(g_node_db, &wtx.tx, g_wallet, 0);

    /* Relay to peers */
    if (g_connman_ptr)
        connman_relay_transaction(g_connman_ptr, &wtx.tx.hash);

    /* Persist wallet state after sending */
    if (g_wallet_db)
        wallet_db_flush(g_wallet_db, g_wallet);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

static bool rpc_dumpprivkey(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "dumpprivkey \"address\"\n"
            "Reveals the private key corresponding to 'address'.");
        return true;
    }

    ENSURE_WALLET(result);

    const char *addr_str = json_get_str(json_at(params, 0));

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
        return false;
    }

    if (dest.type != DEST_KEY_ID) {
        json_set_str(result, "Address does not refer to a key");
        return false;
    }

    struct privkey key;
    if (!wallet_dump_key(g_wallet, &dest.id.key, &key)) {
        json_set_str(result, "Private key for address is not known");
        return false;
    }

    size_t sec_pfx_len;
    const unsigned char *sec_pfx = chain_params_base58_prefix(
        cp, B58_SECRET_KEY, &sec_pfx_len);

    char wif[128];
    bool ok = encode_secret(&key, sec_pfx, sec_pfx_len, wif, sizeof(wif));
    memory_cleanse(key.vch, 32);

    if (!ok) {
        json_set_str(result, "Encoding failed");
        return false;
    }

    json_set_str(result, wif);
    return true;
}

static bool rpc_importprivkey(const struct json_value *params, bool help,
                                struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "importprivkey \"privkey\" ( \"label\" rescan start_height )\n"
            "\nAdds a private key to your wallet.\n"
            "\nArguments:\n"
            "1. \"privkey\"     (string, required) The private key (WIF format)\n"
            "2. \"label\"       (string, optional) An optional label\n"
            "3. rescan          (boolean, optional, default=true) Rescan the blockchain\n"
            "4. start_height    (numeric, optional) Block height to start rescan\n");
        return true;
    }

    ENSURE_WALLET(result);

    const char *wif = json_get_str(json_at(params, 0));
    bool rescan = true;
    int start_height = 0;
    if (json_size(params) >= 3) {
        const struct json_value *v = json_at(params, 2);
        if (v && v->type == JSON_BOOL)
            rescan = json_get_bool(v);
        else if (v && v->type == JSON_INT)
            rescan = json_get_int(v) != 0;
    }
    if (json_size(params) >= 4) {
        start_height = (int)json_param_int(json_at(params, 3));
    }

    const struct chain_params *cp = chain_params_get();
    size_t sec_pfx_len;
    const unsigned char *sec_pfx = chain_params_base58_prefix(
        cp, B58_SECRET_KEY, &sec_pfx_len);

    struct privkey key;
    if (!decode_secret(wif, sec_pfx, sec_pfx_len, &key)) {
        json_set_str(result, "Invalid private key encoding");
        return false;
    }

    if (!wallet_import_key(g_wallet, &key)) {
        memory_cleanse(key.vch, 32);
        json_set_str(result, "Error adding key to wallet");
        return false;
    }

    if (g_wallet->time_first_key == 0)
        g_wallet->time_first_key = GetTime();

    /* Persist key to wallet DB */
    if (g_wallet_db) {
        struct pubkey pk;
        if (privkey_get_pubkey(&key, &pk))
            wallet_db_write_key(g_wallet_db, &pk, &key);
    }

    memory_cleanse(key.vch, 32);

    if (rescan && g_main_state) {
        wallet_rescan(g_wallet, &g_main_state->chain_active,
                      start_height, -1, g_datadir);
    }

    json_set_null(result);
    return true;
}

static bool rpc_rescanblockchain(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "rescanblockchain ( start_height stop_height )\n"
            "\nRescan the local blockchain for wallet transactions.\n"
            "\nArguments:\n"
            "1. start_height  (numeric, optional, default=0) Block height to start\n"
            "2. stop_height   (numeric, optional, default=tip) Block height to stop\n"
            "\nResult:\n"
            "{\n"
            "  \"start_height\": n,\n"
            "  \"stop_height\": n\n"
            "}\n");
        return true;
    }

    if (!g_wallet || !g_main_state) {
        json_set_str(result, "Wallet or chain state not initialized");
        return false;
    }

    int start_height = 0;
    int stop_height = -1;

    if (json_size(params) >= 1)
        start_height = (int)json_param_int(json_at(params, 0));
    if (json_size(params) >= 2)
        stop_height = (int)json_param_int(json_at(params, 1));

    int tip = active_chain_height(&g_main_state->chain_active);
    if (stop_height < 0 || stop_height > tip)
        stop_height = tip;
    if (start_height < 0)
        start_height = 0;

    if (start_height > tip) {
        json_set_str(result, "start_height exceeds chain tip");
        return false;
    }

    wallet_rescan(g_wallet, &g_main_state->chain_active,
                  start_height, stop_height, g_datadir);

    json_set_object(result);
    json_push_kv_int(result, "start_height", start_height);
    json_push_kv_int(result, "stop_height", stop_height);
    return true;
}

static bool rpc_keypoolrefill(const struct json_value *params, bool help,
                                struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "keypoolrefill ( newsize )\n"
            "Fills the keypool.");
        return true;
    }

    ENSURE_WALLET(result);

    unsigned int new_size = DEFAULT_KEYPOOL_SIZE;
    if (json_size(params) >= 1)
        new_size = (unsigned int)json_get_int(json_at(params, 0));

    if (!wallet_top_up_key_pool(g_wallet, new_size)) {
        json_set_str(result, "Error refilling keypool");
        return false;
    }

    json_set_null(result);
    return true;
}

static bool rpc_listtransactions(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "listtransactions ( count skip )\n"
            "Returns up to 'count' most recent transactions.");
        return true;
    }

    ENSURE_WALLET(result);

    int count = 10;
    int skip = 0;
    if (json_size(params) >= 1)
        count = (int)json_get_int(json_at(params, 0));
    if (json_size(params) >= 2)
        skip = (int)json_get_int(json_at(params, 1));
    if (count < 0) count = 0;
    if (skip < 0) skip = 0;

    json_set_array(result);
    if (count == 0)
        return true;

    if (wallet_history_db_ready()) {
        struct db_wallet_tx *rows =
            calloc((size_t)count, sizeof(struct db_wallet_tx));
        if (!rows) {
            json_set_str(result, "Out of memory");
            return false;
        }

        int n = db_wallet_tx_list(g_node_db, rows, (size_t)count, (size_t)skip);
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
        if (!g_wallet->map_wallet[i].used)
            continue;
        if (seen++ < skip)
            continue;
        const struct wallet_tx *wtx = &g_wallet->map_wallet[i];
        wallet_append_tx_entry(&wtx->tx, wtx->from_me, 0, wtx->confirms,
                               wtx->time_received, result);
        added++;
    }

    return true;
}

static bool rpc_gettransaction(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "gettransaction \"txid\"\n"
            "Get detailed information about wallet transaction.");
        return true;
    }

    ENSURE_WALLET(result);

    const char *txid_str = json_get_str(json_at(params, 0));
    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

    if (g_node_db && g_node_db->open) {
        struct db_wallet_tx dbtx;
        if (db_wallet_tx_find(g_node_db, txid.data, &dbtx)) {
            struct transaction tx;
            if (!wallet_db_tx_deserialize(&dbtx, &tx)) {
                db_wallet_tx_free(&dbtx);
                json_set_str(result, "Failed to decode wallet transaction");
                return false;
            }

            json_set_object(result);

            int64_t credit = 0;
            for (size_t j = 0; j < tx.num_vout; j++) {
                if (wallet_is_mine(g_wallet, &tx.vout[j]))
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

    const struct wallet_tx *wtx = wallet_get_tx(g_wallet, &txid);
    if (!wtx) {
        json_set_str(result, "Invalid or non-wallet transaction id");
        return false;
    }

    json_set_object(result);

    int64_t credit = 0;
    int64_t debit = wallet_get_debit(g_wallet, &wtx->tx);
    for (size_t j = 0; j < wtx->tx.num_vout; j++) {
        if (wallet_is_mine(g_wallet, &wtx->tx.vout[j]))
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

static bool rpc_z_getnewaddress(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "z_getnewaddress\n"
            "\nReturns a new Sapling shielded address.\n"
            "\nResult:\n"
            "\"address\"  (string) The new z-address\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    uint8_t diversifier[11];
    uint8_t pk_d[32];
    if (!sapling_keystore_new_address(&g_wallet->sapling_keys,
                                       diversifier, pk_d)) {
        json_set_str(result, "Failed to generate Sapling address");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    char addr[128];
    if (!sapling_encode_payment_address(diversifier, pk_d,
            cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
            addr, sizeof(addr))) {
        json_set_str(result, "Failed to encode address");
        return false;
    }

    /* Persist sapling keys to wallet DB */
    if (g_wallet_db) {
        struct sapling_keystore *sks = &g_wallet->sapling_keys;
        if (sks->has_seed)
            wallet_db_write_sapling_seed(g_wallet_db, sks->seed);
        if (sks->num_keys > 0)
            wallet_db_write_sapling_key(g_wallet_db,
                sks->keys[sks->num_keys - 1].child_index,
                &sks->keys[sks->num_keys - 1]);
    }

    json_set_str(result, addr);
    return true;
}

static bool rpc_z_listaddresses(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "z_listaddresses\n"
            "\nReturns all Sapling z-addresses in the wallet.\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    json_set_array(result);
    const struct chain_params *cp = chain_params_get();

    for (size_t i = 0; i < g_wallet->sapling_keys.num_keys; i++) {
        if (!g_wallet->sapling_keys.keys[i].used) continue;
        char addr[128];
        if (sapling_encode_payment_address(
                g_wallet->sapling_keys.keys[i].diversifier,
                g_wallet->sapling_keys.keys[i].pk_d,
                cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                addr, sizeof(addr))) {
            struct json_value s;
            json_init(&s);
            json_set_str(&s, addr);
            json_push_back(result, &s);
            json_free(&s);
        }
    }

    return true;
}

static bool rpc_createmultisig(const struct json_value *params, bool help,
                                struct json_value *result)
{
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "createmultisig nrequired [\"key\",...]\n"
            "Creates a multi-signature address with n required of m keys.\n"
            "Returns JSON with \"address\" and \"redeemScript\".");
        return true;
    }

    int n_required = (int)json_param_int(json_at(params, 0));
    const struct json_value *keys_arr = json_at(params, 1);
    if (!keys_arr || keys_arr->type != JSON_ARR || json_size(keys_arr) == 0) {
        json_set_str(result, "keys must be a non-empty array");
        return false;
    }

    size_t n_keys = json_size(keys_arr);
    if (n_required < 1 || n_required > (int)n_keys || n_keys > 16) {
        json_set_str(result, "Invalid nrequired or too many keys");
        return false;
    }

    struct pubkey pks[16];
    for (size_t i = 0; i < n_keys; i++) {
        const char *hex = json_get_str(json_at(keys_arr, i));
        if (!hex) {
            json_set_str(result, "Invalid key in array");
            return false;
        }
        size_t hex_len = strlen(hex);
        if (hex_len != 66 && hex_len != 130) {
            json_set_str(result, "Invalid public key length");
            return false;
        }
        unsigned char buf[65];
        size_t buf_len = ParseHex(hex, buf, sizeof(buf));
        if (buf_len != 33 && buf_len != 65) {
            json_set_str(result, "Invalid hex in key");
            return false;
        }
        pubkey_set(&pks[i], buf, buf_len);
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
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "sendmany \"\" {\"address\":amount,...}\n"
            "Send to multiple addresses in one transaction.\n"
            "First argument must be \"\" (empty string).\n"
            "Second argument is a JSON object of address:amount pairs.");
        return true;
    }

    ENSURE_WALLET(result);

    const struct json_value *amounts = json_at(params, 1);
    if (!amounts || amounts->type != JSON_OBJ) {
        json_set_str(result, "amounts must be a JSON object");
        return false;
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
            return false;
        }

        values[n] = parse_amount(val);
        if (values[n] <= 0) {
            json_set_str(result, "Invalid amount");
            return false;
        }
        n++;
    }

    if (n == 0) {
        json_set_str(result, "No recipients");
        return false;
    }

    struct wallet_tx wtx;
    int64_t fee = 0;
    const char *error = NULL;
    if (!wallet_create_transaction_multi(g_wallet, dests, values, n,
                                          &wtx, &fee, &error)) {
        json_set_str(result, error ? error : "Transaction creation failed");
        return false;
    }

    if (!wallet_commit_transaction(g_wallet, &wtx, g_mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        return false;
    }

    if (g_node_db && g_node_db->open)
        node_db_sync_wallet_tx(g_node_db, &wtx.tx, g_wallet, 0);

    if (g_connman_ptr)
        connman_relay_transaction(g_connman_ptr, &wtx.tx.hash);

    if (g_wallet_db)
        wallet_db_flush(g_wallet_db, g_wallet);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

static bool rpc_addmultisigaddress(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "addmultisigaddress nrequired [\"key\",...]\n"
            "Add a multisig address to the wallet.\n"
            "Each key is a hex-encoded public key.\n"
            "The redeem script is stored in the wallet for spending.");
        return true;
    }

    ENSURE_WALLET(result);

    int n_required = (int)json_param_int(json_at(params, 0));
    const struct json_value *keys_arr = json_at(params, 1);
    if (!keys_arr || keys_arr->type != JSON_ARR || json_size(keys_arr) == 0) {
        json_set_str(result, "keys must be a non-empty array");
        return false;
    }

    size_t n_keys = json_size(keys_arr);
    if (n_required < 1 || n_required > (int)n_keys || n_keys > 16) {
        json_set_str(result, "Invalid nrequired or too many keys");
        return false;
    }

    struct pubkey pks[16];
    for (size_t i = 0; i < n_keys; i++) {
        const char *hex = json_get_str(json_at(keys_arr, i));
        if (!hex) {
            json_set_str(result, "Invalid key in array");
            return false;
        }
        unsigned char buf[65];
        size_t buf_len = ParseHex(hex, buf, sizeof(buf));
        if (buf_len != 33 && buf_len != 65) {
            json_set_str(result, "Invalid public key");
            return false;
        }
        pubkey_set(&pks[i], buf, buf_len);
    }

    struct script redeem;
    script_for_multisig(&redeem, n_required, pks, n_keys);

    /* Store redeem script in wallet keystore */
    keystore_add_cscript(&g_wallet->keystore, &redeem);

    struct script_id sid;
    script_id_from_script(&sid, &redeem);

    /* Persist script to wallet DB */
    if (g_wallet_db)
        wallet_db_write_script(g_wallet_db, &sid.hash, &redeem);

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

/* z_getbalance: get balance for a transparent or Sapling address */
static bool rpc_z_getbalance(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "z_getbalance \"address\" ( minconf )\n"
            "\nReturns the balance for a taddr or zaddr.\n");
        return true;
    }

    const char *addr_str = json_get_str(json_at(params, 0));
    ENSURE_WALLET(result);

    if (!addr_str) {
        json_set_str(result, "Missing address parameter");
        return false;
    }

    int minconf = 1;
    if (json_size(params) > 1)
        minconf = (int)json_param_int(json_at(params, 1));

    /* Check if Sapling address */
    uint8_t z_d[11], z_pkd[32];
    if (sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        int64_t balance = 0;
        for (size_t i = 0; i < g_wallet->num_sapling_notes; i++) {
            const struct sapling_received_note *n = &g_wallet->sapling_notes[i];
            if (!n->used || n->spent)
                continue;
            if (memcmp(n->diversifier, z_d, 11) == 0 &&
                memcmp(n->pk_d, z_pkd, 32) == 0) {
                if (n->confirms >= minconf)
                    balance += (int64_t)n->value;
            }
        }
        char buf[32];
        format_amount(balance, buf, sizeof(buf));
        json_set_str(result, buf);
        return true;
    }

    /* Transparent address — sum UTXOs */
    struct tx_destination dest;
    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);
    if (!decode_destination(addr_str, pk_pfx, pk_pfx_len,
                             sc_pfx, sc_pfx_len, &dest)) {
        json_set_str(result, "Invalid address");
        return false;
    }

    int64_t balance = 0;
    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(g_wallet, coins, &num_coins, 4096,
                            minconf > 0, false);

    struct script addr_script;
    addr_script.size = 0;
    script_for_destination(&addr_script, &dest);

    for (size_t i = 0; i < num_coins; i++) {
        const struct tx_out *out = &coins[i].wtx->tx.vout[coins[i].i];
        if (out->script_pub_key.size == addr_script.size &&
            memcmp(out->script_pub_key.data, addr_script.data,
                   addr_script.size) == 0) {
            if (coins[i].depth >= minconf)
                balance += out->value;
        }
    }

    char buf[32];
    format_amount(balance, buf, sizeof(buf));
    json_set_str(result, buf);
    return true;
}

/* z_listunspent: list unspent Sapling notes */
static bool rpc_z_listunspent(const struct json_value *params, bool help,
                               struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "z_listunspent ( minconf maxconf )\n"
            "\nReturns list of unspent shielded notes.\n");
        return true;
    }

    ENSURE_WALLET(result);

    int minconf = 0;
    if (json_size(params) > 0)
        minconf = (int)json_param_int(json_at(params, 0));

    json_set_array(result);
    for (size_t i = 0; i < g_wallet->num_sapling_notes; i++) {
        const struct sapling_received_note *n = &g_wallet->sapling_notes[i];
        if (!n->used || n->spent)
            continue;
        if (n->confirms < minconf)
            continue;

        struct json_value entry;
        json_set_object(&entry);

        char txid_hex[65];
        uint256_get_hex(&n->txid, txid_hex);
        json_push_kv_str(&entry, "txid", txid_hex);
        json_push_kv_int(&entry, "outindex", n->output_index);

        /* Encode address */
        char z_addr[128];
        sapling_encode_payment_address(n->diversifier, n->pk_d,
                                        "zs", z_addr, sizeof(z_addr));
        json_push_kv_str(&entry, "address", z_addr);

        char amount_buf[32];
        format_amount((int64_t)n->value, amount_buf, sizeof(amount_buf));
        json_push_kv_str(&entry, "amount", amount_buf);

        /* Memo (show if non-default) */
        bool has_memo = false;
        for (int j = 0; j < 512 && !has_memo; j++) {
            if (n->memo[j] != 0xf6 && n->memo[j] != 0x00)
                has_memo = true;
        }
        if (has_memo) {
            char memo_hex[1025];
            for (int j = 0; j < 512; j++)
                snprintf(memo_hex + j * 2, 3, "%02x", n->memo[j]);
            json_push_kv_str(&entry, "memo", memo_hex);
        }

        json_push_kv_int(&entry, "confirmations", n->confirms);
        json_push_back(result, &entry);
    }
    return true;
}

/* z_sendmany: send from transparent address to one or more Sapling/transparent recipients */
static bool rpc_z_sendmany(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "z_sendmany \"fromaddress\" [{\"address\":\"...\",\"amount\":...,\"memo\":\"...\"},...]\n"
            "\nSend from a transparent or shielded address to multiple recipients.\n"
            "Currently supports transparent → Sapling (shielding) and transparent → transparent.\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    const char *from_addr = json_get_str(json_at(params, 0));
    const struct json_value *recipients = json_at(params, 1);
    if (!from_addr || !recipients || recipients->type != JSON_ARR || json_size(recipients) == 0) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    /* Check if from address is transparent (t1/t3) or shielded (zs1) */
    bool from_is_shielded = (strncmp(from_addr, "zs1", 3) == 0);
    if (from_is_shielded) {
        json_set_str(result, "Shielded spends not yet implemented (need Groth16 prover)");
        return false;
    }

    /* Verify we own the from address */
    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct tx_destination from_dest;
    if (!decode_destination(from_addr, pk_pfx, pk_pfx_len, sc_pfx, sc_pfx_len, &from_dest)) {
        json_set_str(result, "Invalid from address");
        return false;
    }

    /* Parse recipients */
    size_t num_recip = json_size(recipients);
    if (num_recip > 50) {
        json_set_str(result, "Too many recipients");
        return false;
    }

    /* Separate into transparent and shielded outputs */
    struct tx_destination t_dests[50];
    int64_t t_amounts[50];
    size_t num_t_out = 0;

    uint8_t z_diversifiers[50][11];
    uint8_t z_pk_ds[50][32];
    int64_t z_amounts[50];
    uint8_t z_memos[50][512];
    bool z_has_memo[50];
    size_t num_z_out = 0;
    int64_t total_amount = 0;

    for (size_t i = 0; i < num_recip; i++) {
        const struct json_value *r = json_at(recipients, i);
        if (!r || r->type != JSON_OBJ) {
            json_set_str(result, "Invalid recipient");
            return false;
        }
        const char *addr = json_get_str(json_get(r, "address"));
        int64_t amount = parse_amount(json_get(r, "amount"));
        if (!addr || amount <= 0) {
            json_set_str(result, "Invalid recipient address or amount");
            return false;
        }
        total_amount += amount;

        if (strncmp(addr, "zs1", 3) == 0) {
            /* Sapling shielded output */
            if (!sapling_decode_payment_address(addr,
                    z_diversifiers[num_z_out], z_pk_ds[num_z_out])) {
                json_set_str(result, "Invalid Sapling address");
                return false;
            }
            z_amounts[num_z_out] = amount;
            /* Parse memo if present */
            const struct json_value *memo_val = json_get(r, "memo");
            if (memo_val && json_get_str(memo_val)) {
                const char *memo_str = json_get_str(memo_val);
                size_t memo_len = strlen(memo_str);
                if (memo_len > 512) memo_len = 512;
                memset(z_memos[num_z_out], 0xF6, 512);
                memcpy(z_memos[num_z_out], memo_str, memo_len);
                z_has_memo[num_z_out] = true;
            } else {
                z_has_memo[num_z_out] = false;
            }
            num_z_out++;
        } else {
            /* Transparent output */
            if (!decode_destination(addr, pk_pfx, pk_pfx_len,
                                     sc_pfx, sc_pfx_len, &t_dests[num_t_out])) {
                json_set_str(result, "Invalid transparent address");
                return false;
            }
            t_amounts[num_t_out] = amount;
            num_t_out++;
        }
    }

    /* Select coins — filter to only UTXOs from the specified from address */
    int64_t fee = g_wallet->default_fee;
    struct coin_entry available[4096];
    size_t num_available = 0;
    wallet_available_coins(g_wallet, available, &num_available, 4096, false, false);

    /* Filter to coins matching the from address */
    struct coin_entry filtered[4096];
    size_t num_filtered = 0;
    for (size_t i = 0; i < num_available; i++) {
        const struct tx_out *out = &available[i].wtx->tx.vout[available[i].i];
        struct tx_destination coin_dest;
        if (!script_extract_destination(&out->script_pub_key, &coin_dest))
            continue;
        /* Check if this coin's address matches from_dest */
        bool match = false;
        if (coin_dest.type == from_dest.type) {
            if (coin_dest.type == DEST_KEY_ID)
                match = (memcmp(coin_dest.id.key.id.data, from_dest.id.key.id.data, 20) == 0);
            else if (coin_dest.type == DEST_SCRIPT_ID)
                match = (memcmp(coin_dest.id.script.hash.data, from_dest.id.script.hash.data, 20) == 0);
        }
        if (match)
            filtered[num_filtered++] = available[i];
    }

    struct coin_entry selected[4096];
    size_t num_selected = 0;
    int64_t selected_value = 0;

    if (!wallet_select_coins(g_wallet, filtered, num_filtered,
                              total_amount + fee, selected, &num_selected,
                              4096, &selected_value)) {
        json_set_str(result, "Insufficient funds from specified address");
        return false;
    }

    /* Build transaction */
    struct wallet_tx wtx;
    memset(&wtx, 0, sizeof(wtx));
    transaction_init(&wtx.tx);

    int height = g_wallet->best_block_height;
    wtx.tx.overwintered = true;
    wtx.tx.version = SAPLING_TX_VERSION;
    wtx.tx.version_group_id = SAPLING_VERSION_GROUP_ID;
    wtx.tx.expiry_height = (uint32_t)(height + 20);

    /* Transparent outputs: recipients + change */
    int64_t change = selected_value - total_amount - fee;
    size_t total_t_out = num_t_out + (change > 0 ? 1 : 0);

    if (!transaction_alloc(&wtx.tx, num_selected, total_t_out)) {
        json_set_str(result, "Transaction allocation failed");
        return false;
    }

    /* Fill transparent outputs */
    for (size_t i = 0; i < num_t_out; i++) {
        struct script dest_script;
        script_for_destination(&dest_script, &t_dests[i]);
        wtx.tx.vout[i].value = t_amounts[i];
        wtx.tx.vout[i].script_pub_key = dest_script;
    }

    /* Change output */
    if (change > 0) {
        struct pubkey change_pk;
        if (!wallet_get_key_from_pool(g_wallet, &change_pk)) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Cannot get change address");
            return false;
        }
        struct key_id change_kid = pubkey_get_id(&change_pk);
        struct tx_destination change_dest;
        change_dest.type = DEST_KEY_ID;
        change_dest.id.key = change_kid;
        struct script change_script;
        script_for_destination(&change_script, &change_dest);
        wtx.tx.vout[num_t_out].value = change;
        wtx.tx.vout[num_t_out].script_pub_key = change_script;
    }

    /* value_balance = -(sum of shielded outputs) for shielding (negative = transparent→shielded) */
    int64_t shielded_total = 0;
    for (size_t i = 0; i < num_z_out; i++)
        shielded_total += z_amounts[i];
    wtx.tx.value_balance = -shielded_total;

    /* Build Sapling output descriptions */
    if (num_z_out > 0) {
        wtx.tx.v_shielded_output = calloc(num_z_out, sizeof(struct output_description));
        if (!wtx.tx.v_shielded_output) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Allocation failed");
            return false;
        }
        wtx.tx.num_shielded_output = num_z_out;

        /* Get OVK from sapling keystore */
        uint8_t ovk[32];
        if (g_wallet->sapling_keys.num_keys > 0)
            memcpy(ovk, g_wallet->sapling_keys.keys[0].xfvk.fvk.ovk, 32);
        else
            GetRandBytes(ovk, 32);

        /* Use librustzcash proving context for output proofs + binding sig */
        extern void *librustzcash_sapling_proving_ctx_init(void);
        extern bool librustzcash_sapling_binding_sig(
            const void *ctx, int64_t valueBalance,
            const unsigned char *sighash, unsigned char *result_out);
        extern void librustzcash_sapling_proving_ctx_free(void *);

        void *proving_ctx = librustzcash_sapling_proving_ctx_init();
        if (!proving_ctx) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Failed to init proving context");
            return false;
        }

        for (size_t i = 0; i < num_z_out; i++) {
            struct output_description *od = &wtx.tx.v_shielded_output[i];

            if (!sapling_build_output_with_ctx(
                    proving_ctx,
                    ovk, z_diversifiers[i], z_pk_ds[i],
                    (uint64_t)z_amounts[i],
                    z_has_memo[i] ? z_memos[i] : NULL,
                    od->cv.data, od->cm.data, od->ephemeral_key.data,
                    od->enc_ciphertext, od->out_ciphertext, od->zkproof)) {
                librustzcash_sapling_proving_ctx_free(proving_ctx);
                transaction_free(&wtx.tx);
                json_set_str(result, "Failed to build Sapling output");
                return false;
            }
        }

        /* Fill transparent inputs first (needed for sighash) */
        for (size_t i = 0; i < num_selected; i++) {
            wtx.tx.vin[i].prevout.hash = selected[i].wtx->tx.hash;
            wtx.tx.vin[i].prevout.n = selected[i].i;
            wtx.tx.vin[i].sequence = UINT32_MAX - 1;
        }

        /* Sign transparent inputs */
        zcl_mutex_lock(&g_wallet->cs);
        for (size_t i = 0; i < num_selected; i++) {
            struct privkey skey;
            const struct tx_out *prev_out = &selected[i].wtx->tx.vout[selected[i].i];
            struct tx_destination prev_dest;
            if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest)) {
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Cannot determine input destination");
                return false;
            }
            if (!keystore_get_key(&g_wallet->keystore, &prev_dest.id.key, &skey)) {
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Private key not available");
                return false;
            }

            struct pubkey spk;
            privkey_get_pubkey(&skey, &spk);

            uint32_t branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);
            struct sighash_type ht;
            ht.raw = SIGHASH_ALL;
            struct precomputed_tx_data txdata;
            precompute_tx_data(&wtx.tx, &txdata);

            struct uint256 sighash;
            if (!signature_hash(&prev_out->script_pub_key, &wtx.tx,
                                (unsigned int)i, ht, prev_out->value,
                                branch_id, &txdata, &sighash)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Sighash computation failed");
                return false;
            }

            unsigned char sig[SIGNATURE_SIZE + 1];
            size_t siglen = 0;
            if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Signing failed");
                return false;
            }
            sig[siglen++] = 0x01;

            struct script *ss = &wtx.tx.vin[i].script_sig;
            ss->size = 0;
            ss->data[ss->size++] = (unsigned char)siglen;
            memcpy(&ss->data[ss->size], sig, siglen);
            ss->size += siglen;
            ss->data[ss->size++] = (unsigned char)spk.size;
            memcpy(&ss->data[ss->size], spk.vch, spk.size);
            ss->size += spk.size;

            memory_cleanse(skey.vch, 32);
        }
        zcl_mutex_unlock(&g_wallet->cs);

        /* Compute binding signature using librustzcash proving context */
        transaction_compute_hash(&wtx.tx);

        uint32_t branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);
        struct sighash_type ht;
        ht.raw = SIGHASH_ALL;
        struct precomputed_tx_data txdata;
        precompute_tx_data(&wtx.tx, &txdata);

        struct script empty_script;
        empty_script.size = 0;
        struct uint256 binding_sighash;
        signature_hash(&empty_script, &wtx.tx, NOT_AN_INPUT, ht, 0,
                       branch_id, &txdata, &binding_sighash);

        if (!librustzcash_sapling_binding_sig(proving_ctx,
                                               wtx.tx.value_balance,
                                               binding_sighash.data,
                                               wtx.tx.binding_sig)) {
            librustzcash_sapling_proving_ctx_free(proving_ctx);
            transaction_free(&wtx.tx);
            json_set_str(result, "Binding signature failed");
            return false;
        }
        librustzcash_sapling_proving_ctx_free(proving_ctx);
    } else {
        /* No shielded outputs — just transparent */
        for (size_t i = 0; i < num_selected; i++) {
            wtx.tx.vin[i].prevout.hash = selected[i].wtx->tx.hash;
            wtx.tx.vin[i].prevout.n = selected[i].i;
            wtx.tx.vin[i].sequence = UINT32_MAX - 1;
        }

        zcl_mutex_lock(&g_wallet->cs);
        for (size_t i = 0; i < num_selected; i++) {
            struct privkey skey;
            const struct tx_out *prev_out = &selected[i].wtx->tx.vout[selected[i].i];
            struct tx_destination prev_dest;
            if (!script_extract_destination(&prev_out->script_pub_key, &prev_dest)) {
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Cannot determine input destination");
                return false;
            }
            if (!keystore_get_key(&g_wallet->keystore, &prev_dest.id.key, &skey)) {
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Private key not available");
                return false;
            }

            struct pubkey spk;
            privkey_get_pubkey(&skey, &spk);
            uint32_t branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);
            struct sighash_type ht;
            ht.raw = SIGHASH_ALL;
            struct precomputed_tx_data txdata;
            precompute_tx_data(&wtx.tx, &txdata);

            struct uint256 sighash;
            signature_hash(&prev_out->script_pub_key, &wtx.tx,
                           (unsigned int)i, ht, prev_out->value,
                           branch_id, &txdata, &sighash);

            unsigned char sig[SIGNATURE_SIZE + 1];
            size_t siglen = 0;
            if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&g_wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Signing failed");
                return false;
            }
            sig[siglen++] = 0x01;

            struct script *ss = &wtx.tx.vin[i].script_sig;
            ss->size = 0;
            ss->data[ss->size++] = (unsigned char)siglen;
            memcpy(&ss->data[ss->size], sig, siglen);
            ss->size += siglen;
            ss->data[ss->size++] = (unsigned char)spk.size;
            memcpy(&ss->data[ss->size], spk.vch, spk.size);
            ss->size += spk.size;

            memory_cleanse(skey.vch, 32);
        }
        zcl_mutex_unlock(&g_wallet->cs);
    }

    transaction_compute_hash(&wtx.tx);
    wtx.time_received = GetTime();
    wtx.from_me = true;
    wtx.used = true;

    if (!wallet_commit_transaction(g_wallet, &wtx, g_mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        return false;
    }

    if (g_node_db && g_node_db->open)
        node_db_sync_wallet_tx(g_node_db, &wtx.tx, g_wallet, 0);

    if (g_connman_ptr)
        connman_relay_transaction(g_connman_ptr, &wtx.tx.hash);

    if (g_wallet_db)
        wallet_db_flush(g_wallet_db, g_wallet);

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

static bool rpc_scanblockfiles(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "scanblockfiles\n"
            "\nScan all block files on disk for wallet transactions.\n"
            "Faster than rescanblockchain — reads raw block files sequentially.\n"
            "Updates the spent-outpoint index for accurate balances.\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    const char *dir = g_datadir ? g_datadir : "/home/bob/.zclassic-c23";
    int found = wallet_scan_blockfiles(g_wallet, dir);

    /* Also persist wallet updates */
    if (g_wallet_db)
        wallet_db_flush(g_wallet_db, g_wallet);

    json_set_object(result);
    json_push_kv_int(result, "wallet_outputs_found", found);
    json_push_kv_int(result, "spent_outpoints", (int64_t)g_wallet->num_spent);

    /* Report corrected balance */
    int64_t balance = wallet_get_balance(g_wallet);
    char tbal[32], zbal[32];
    format_amount(balance, tbal, sizeof(tbal));
    json_push_kv_real(result, "transparent_balance", strtod(tbal, NULL));

    int64_t z_balance = wallet_get_sapling_balance(g_wallet);
    format_amount(z_balance, zbal, sizeof(zbal));
    json_push_kv_real(result, "shielded_balance", strtod(zbal, NULL));

    return true;
}

static bool rpc_z_gettotalbalance(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "z_gettotalbalance ( minconf )\n"
            "\nReturn the total value of funds stored in the wallet.\n"
            "\nResult:\n"
            "{\n"
            "  \"transparent\": \"x.xxxx\",\n"
            "  \"private\": \"x.xxxx\",\n"
            "  \"total\": \"x.xxxx\"\n"
            "}\n");
        return true;
    }

    ENSURE_WALLET(result);

    int minconf = 1;
    if (json_size(params) >= 1)
        minconf = (int)json_param_int(json_at(params, 0));

    /* Transparent balance via available_coins */
    int64_t t_balance = 0;
    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(g_wallet, coins, &num_coins, 4096,
                            minconf > 0, false);
    for (size_t i = 0; i < num_coins; i++) {
        if (coins[i].depth >= minconf)
            t_balance += coins[i].wtx->tx.vout[coins[i].i].value;
    }

    /* Shielded balance */
    int64_t z_balance = 0;
    for (size_t i = 0; i < g_wallet->num_sapling_notes; i++) {
        const struct sapling_received_note *n = &g_wallet->sapling_notes[i];
        if (n->used && !n->spent && n->confirms >= minconf)
            z_balance += (int64_t)n->value;
    }

    int64_t total = t_balance + z_balance;

    char t_str[32], z_str[32], tot_str[32];
    format_amount(t_balance, t_str, sizeof(t_str));
    format_amount(z_balance, z_str, sizeof(z_str));
    format_amount(total, tot_str, sizeof(tot_str));

    json_set_object(result);
    json_push_kv_str(result, "transparent", t_str);
    json_push_kv_str(result, "private", z_str);
    json_push_kv_str(result, "total", tot_str);
    return true;
}

static bool rpc_z_listreceivedbyaddress(const struct json_value *params,
                                          bool help, struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "z_listreceivedbyaddress \"address\" ( minconf )\n"
            "\nReturn a list of amounts received by a zaddr.\n");
        return true;
    }

    ENSURE_WALLET(result);

    const char *addr_str = json_get_str(json_at(params, 0));
    int minconf = 1;
    if (json_size(params) >= 2)
        minconf = (int)json_param_int(json_at(params, 1));

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Not a valid Sapling address");
        return false;
    }

    json_set_array(result);
    for (size_t i = 0; i < g_wallet->num_sapling_notes; i++) {
        const struct sapling_received_note *n = &g_wallet->sapling_notes[i];
        if (!n->used) continue;
        if (memcmp(n->diversifier, z_d, 11) != 0 ||
            memcmp(n->pk_d, z_pkd, 32) != 0)
            continue;
        if (n->confirms < minconf)
            continue;

        struct json_value entry;
        json_set_object(&entry);

        char txid[65];
        uint256_get_hex(&n->txid, txid);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "outindex", n->output_index);
        char amt[32];
        format_amount((int64_t)n->value, amt, sizeof(amt));
        json_push_kv_real(&entry, "amount", strtod(amt, NULL));
        json_push_kv_int(&entry, "confirmations", n->confirms);
        json_push_kv_bool(&entry, "change", false);
        json_push_kv_bool(&entry, "spent", n->spent);

        /* Memo — show as hex if non-empty, or as text */
        bool has_memo = false;
        for (int j = 0; j < 512; j++) {
            if (n->memo[j] != 0 && n->memo[j] != 0xf6) {
                has_memo = true;
                break;
            }
        }
        if (has_memo) {
            /* If starts with printable text, show as string */
            if (n->memo[0] >= 0x20 && n->memo[0] < 0x7f) {
                size_t len = 0;
                while (len < 512 && n->memo[len] != 0)
                    len++;
                char memo_str[513];
                memcpy(memo_str, n->memo, len);
                memo_str[len] = '\0';
                json_push_kv_str(&entry, "memo", memo_str);
            } else {
                /* Hex-encode */
                char hex[1025];
                size_t last_nonzero = 0;
                for (size_t j = 0; j < 512; j++)
                    if (n->memo[j]) last_nonzero = j;
                for (size_t j = 0; j <= last_nonzero; j++)
                    snprintf(hex + j * 2, 3, "%02x", n->memo[j]);
                hex[(last_nonzero + 1) * 2] = '\0';
                json_push_kv_str(&entry, "memo", hex);
            }
        }

        json_push_back(result, &entry);
        json_free(&entry);
    }
    return true;
}

static bool rpc_z_exportkey(const struct json_value *params, bool help,
                             struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "z_exportkey \"zaddr\"\n"
            "\nReveals the spending key for a Sapling z-address.\n"
            "The key can be imported into another wallet with z_importkey.\n"
            "\nArguments:\n"
            "1. \"zaddr\"  (string, required) The z-address\n"
            "\nResult:\n"
            "\"key\"  (string) The spending key (bech32 encoded)\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    const char *addr_str = json_get_str(json_at(params, 0));
    if (!addr_str) {
        json_set_str(result, "Missing address parameter");
        return false;
    }

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Invalid Sapling address");
        return false;
    }

    const struct sapling_key_entry *ke =
        sapling_keystore_find_by_address(&g_wallet->sapling_keys, z_d, z_pkd);
    if (!ke) {
        json_set_str(result, "Wallet does not hold spending key for this z-address");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    char encoded[512];
    if (!sapling_encode_extended_spending_key(&ke->xsk,
            cp->bech32HRPs[BECH32_SAPLING_EXTENDED_SPEND_KEY],
            encoded, sizeof(encoded))) {
        json_set_str(result, "Failed to encode spending key");
        return false;
    }

    json_set_str(result, encoded);
    memory_cleanse(encoded, sizeof(encoded));
    return true;
}

static bool rpc_z_importkey(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "z_importkey \"key\" ( rescan startHeight )\n"
            "\nImports a Sapling spending key (as returned by z_exportkey).\n"
            "\nArguments:\n"
            "1. \"key\"          (string, required) The spending key (bech32)\n"
            "2. rescan           (string, optional, default=\"whenkeyisnew\")\n"
            "                    \"yes\", \"no\", or \"whenkeyisnew\"\n"
            "3. startHeight      (numeric, optional, default=0) Start rescan height\n"
            "\nExamples:\n"
            "  z_importkey \"secret-extended-key-main1...\"\n"
            "  z_importkey \"secret-extended-key-main1...\" whenkeyisnew 500000\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    const char *key_str = json_get_str(json_at(params, 0));
    if (!key_str) {
        json_set_str(result, "Missing key parameter");
        return false;
    }

    /* Parse rescan option */
    bool do_rescan = true;
    bool ignore_existing = true;
    if (json_size(params) >= 2) {
        const char *rescan_str = json_get_str(json_at(params, 1));
        if (rescan_str) {
            if (strcmp(rescan_str, "no") == 0) {
                do_rescan = false;
                ignore_existing = false;
            } else if (strcmp(rescan_str, "yes") == 0) {
                do_rescan = true;
                ignore_existing = false;
            }
        }
    }

    int start_height = 0;
    if (json_size(params) >= 3)
        start_height = (int)json_param_int(json_at(params, 2));

    /* Decode spending key */
    struct zip32_xsk xsk;
    if (!sapling_decode_extended_spending_key(key_str, &xsk)) {
        json_set_str(result, "Invalid spending key");
        return false;
    }

    /* Import into keystore */
    if (!sapling_keystore_import_xsk(&g_wallet->sapling_keys, &xsk)) {
        memory_cleanse(&xsk, sizeof(xsk));
        if (ignore_existing) {
            json_set_null(result);
            return true;
        }
        json_set_str(result, "Key already exists in wallet");
        return false;
    }
    memory_cleanse(&xsk, sizeof(xsk));

    /* Persist to wallet DB */
    if (g_wallet_db) {
        struct sapling_keystore *sks = &g_wallet->sapling_keys;
        if (sks->has_seed)
            wallet_db_write_sapling_seed(g_wallet_db, sks->seed);
        wallet_db_write_sapling_key(g_wallet_db,
            sks->keys[sks->num_keys - 1].child_index,
            &sks->keys[sks->num_keys - 1]);
    }

    if (do_rescan && g_main_state) {
        wallet_rescan(g_wallet, &g_main_state->chain_active,
                      start_height, -1, g_datadir);
    }

    json_set_null(result);
    return true;
}

static bool rpc_z_exportviewingkey(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "z_exportviewingkey \"zaddr\"\n"
            "\nReveals the viewing key for a Sapling z-address.\n"
            "A viewing key allows seeing incoming transactions but not spending.\n"
            "\nArguments:\n"
            "1. \"zaddr\"  (string, required) The z-address\n"
            "\nResult:\n"
            "\"vkey\"  (string) The viewing key (bech32 encoded)\n");
        return true;
    }

    if (!g_wallet) {
        json_set_str(result, "Wallet not available");
        return false;
    }

    const char *addr_str = json_get_str(json_at(params, 0));
    if (!addr_str) {
        json_set_str(result, "Missing address parameter");
        return false;
    }

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Invalid Sapling address");
        return false;
    }

    const struct sapling_key_entry *ke =
        sapling_keystore_find_by_address(&g_wallet->sapling_keys, z_d, z_pkd);
    if (!ke) {
        json_set_str(result,
            "Wallet does not hold key for this z-address");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    char encoded[512];
    if (!sapling_encode_extended_full_viewing_key(&ke->xfvk,
            cp->bech32HRPs[BECH32_SAPLING_FULL_VIEWING_KEY],
            encoded, sizeof(encoded))) {
        json_set_str(result, "Failed to encode viewing key");
        return false;
    }

    json_set_str(result, encoded);
    return true;
}

/* reindexdb — wipe and rebuild wallet tables from chain data */
static bool rpc_reindexdb(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "reindexdb\n"
            "Wipes wallet_utxos, wallet_transactions, and wallet_sapling_notes,\n"
            "then re-scans all blocks from disk to rebuild them.\n"
            "Returns the corrected balance.\n");
        return true;
    }

    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "SQLite database not available");
        return false;
    }
    if (!g_wallet) {
        json_set_str(result, "Wallet not loaded");
        return false;
    }
    if (!g_main_state) {
        json_set_str(result, "Chain state not available");
        return false;
    }

    int chain_tip = active_chain_height(&g_main_state->chain_active);
    printf("reindexdb: fast wallet scan of %d blocks...\n", chain_tip + 1);
    fflush(stdout);

    int found = wallet_scan_blocks(g_node_db,
        &g_main_state->chain_active, g_wallet, g_datadir,
        0, chain_tip);

    node_db_sync_wallet_keys(g_node_db, g_wallet);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", chain_tip + 1);
    json_push_kv_int(result, "wallet_transactions", found);

    int64_t t_bal = db_wallet_utxo_balance(g_node_db);
    char bal_str[32];
    format_amount(t_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t z_bal = db_sapling_note_balance(g_node_db);
    char zbal_str[32];
    format_amount(z_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int64_t total = t_bal + z_bal;
    char tot_str[32];
    format_amount(total, tot_str, sizeof(tot_str));
    json_push_kv_str(result, "total_balance", tot_str);

    struct db_wallet_utxo utxos[256];
    int utxo_count = db_wallet_utxo_list_unspent(g_node_db, utxos, 256);
    json_push_kv_int(result, "unspent_utxos", utxo_count);

    printf("reindexdb: complete — balance %s ZCL (%d UTXOs)\n",
           tot_str, utxo_count);
    fflush(stdout);

    return true;
}

/* importlegacy — import wallet data from legacy C++ node */
static bool rpc_importlegacy(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "importlegacy ( \"datadir\" )\n"
            "Import wallet data from a stopped legacy C++ node's data directory.\n"
            "Reads the LevelDB block index and scans block files directly.\n"
            "The legacy node MUST be stopped first.\n"
            "\nArguments:\n"
            "1. datadir  (string, optional) Legacy data directory "
            "(default: ~/.zclassic)\n"
            "\nResult: { blocks_scanned, wallet_transactions, balance }\n");
        return true;
    }

    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "SQLite database not available");
        return false;
    }
    if (!g_wallet) {
        json_set_str(result, "Wallet not loaded");
        return false;
    }

    const char *legacy_dir = NULL;
    char default_dir[512];
    if (params && json_size(params) > 0) {
        const struct json_value *p0 = json_at(params, 0);
        if (p0 && p0->type == JSON_STR)
            legacy_dir = json_get_str(p0);
    }
    if (!legacy_dir || legacy_dir[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(default_dir, sizeof(default_dir),
                 "%s/.zclassic", home ? home : "/root");
        legacy_dir = default_dir;
    }

    printf("importlegacy: importing from %s...\n", legacy_dir);
    fflush(stdout);

    int found = legacy_import(legacy_dir, g_node_db, g_wallet, true);
    if (found < 0) {
        json_set_str(result,
            "Import failed — is the legacy node stopped?");
        return false;
    }

    json_set_object(result);
    json_push_kv_int(result, "wallet_transactions", found);

    int64_t t_bal = db_wallet_utxo_balance(g_node_db);
    char bal_str[32];
    format_amount(t_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t z_bal = db_sapling_note_balance(g_node_db);
    char zbal_str[32];
    format_amount(z_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int64_t total = t_bal + z_bal;
    char tot_str[32];
    format_amount(total, tot_str, sizeof(tot_str));
    json_push_kv_str(result, "total_balance", tot_str);

    struct db_wallet_utxo utxos[256];
    int utxo_count = db_wallet_utxo_list_unspent(g_node_db, utxos, 256);
    json_push_kv_int(result, "unspent_utxos", utxo_count);

    return true;
}

/* db_info — SQLite database statistics */
static bool rpc_db_info(const struct json_value *params, bool help,
                        struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "db_info\n"
            "Returns SQLite node database statistics.\n");
        return true;
    }

    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "SQLite database not available");
        return false;
    }

    json_set_object(result);

    int tip_h = node_db_sync_get_tip_height(g_node_db);
    json_push_kv_int(result, "tip_height", tip_h);

    int64_t utxo_count = db_utxo_count(g_node_db);
    json_push_kv_int(result, "utxo_count", utxo_count);

    int block_count = db_block_count(g_node_db);
    json_push_kv_int(result, "blocks_indexed", block_count);

    int max_h = db_block_max_height(g_node_db);
    json_push_kv_int(result, "max_block_height", max_h);

    int64_t wallet_bal = db_wallet_utxo_balance(g_node_db);
    char bal_str[32];
    format_amount(wallet_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t sapling_bal = db_sapling_note_balance(g_node_db);
    char zbal_str[32];
    format_amount(sapling_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int mempool_count = db_mempool_count(g_node_db);
    json_push_kv_int(result, "mempool_persisted", mempool_count);

    int peer_count = db_peer_count(g_node_db);
    json_push_kv_int(result, "peers_stored", peer_count);

    int wkey_count = db_wallet_key_count(g_node_db);
    json_push_kv_int(result, "wallet_keys", wkey_count);

    int skey_count = db_sapling_key_count(g_node_db);
    json_push_kv_int(result, "sapling_keys", skey_count);

    int wtx_count = db_wallet_tx_count(g_node_db);
    json_push_kv_int(result, "wallet_transactions", wtx_count);

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
        { "wallet", "keypoolrefill",        rpc_keypoolrefill,        false },
        { "wallet", "listtransactions",     rpc_listtransactions,     false },
        { "wallet", "gettransaction",       rpc_gettransaction,       false },
        { "wallet", "rescanblockchain",     rpc_rescanblockchain,     false },
        { "wallet", "scanblockfiles",      rpc_scanblockfiles,       false },
        { "wallet", "sendmany",             rpc_sendmany,             false },
        { "wallet", "createmultisig",      rpc_createmultisig,       false },
        { "wallet", "z_getnewaddress",     rpc_z_getnewaddress,      false },
        { "wallet", "z_listaddresses",     rpc_z_listaddresses,      false },
        { "wallet", "z_sendmany",          rpc_z_sendmany,           false },
        { "wallet", "z_getbalance",        rpc_z_getbalance,         false },
        { "wallet", "z_gettotalbalance", rpc_z_gettotalbalance,    false },
        { "wallet", "z_listunspent",       rpc_z_listunspent,        false },
        { "wallet", "z_listreceivedbyaddress", rpc_z_listreceivedbyaddress, false },
        { "wallet", "addmultisigaddress", rpc_addmultisigaddress,   false },
        { "wallet", "z_exportkey",       rpc_z_exportkey,          false },
        { "wallet", "z_importkey",       rpc_z_importkey,          false },
        { "wallet", "z_exportviewingkey", rpc_z_exportviewingkey,  false },
        { "wallet", "reindexdb",       rpc_reindexdb,            false },
        { "wallet", "importlegacy",   rpc_importlegacy,         false },
        { "wallet", "db_info",          rpc_db_info,              false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
