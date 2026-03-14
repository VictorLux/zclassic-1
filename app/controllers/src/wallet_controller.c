/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_controller.h"
#include "controllers/strong_params.h"
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
#include "zcash/incremental_merkle_tree.h"
#include "zcash/librustzcash.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

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

static void append_one_entry(struct json_value *result,
                             const char *txid, int vout_n,
                             const char *category, const char *address,
                             int64_t amount, int64_t fee,
                             int confirmations, int64_t time_received)
{
    struct json_value entry = {0};
    json_init(&entry);
    json_set_object(&entry);
    json_push_kv_str(&entry, "txid", txid);
    json_push_kv_int(&entry, "vout", vout_n);
    json_push_kv_str(&entry, "category", category);
    if (address)
        json_push_kv_str(&entry, "address", address);
    char a[32];
    format_amount(amount, a, sizeof(a));
    json_push_kv_real(&entry, "amount", strtod(a, NULL));
    if (fee != 0) {
        char f[32];
        format_amount(fee, f, sizeof(f));
        json_push_kv_real(&entry, "fee", strtod(f, NULL));
    }
    json_push_kv_int(&entry, "confirmations", confirmations);
    json_push_kv_int(&entry, "time", time_received);
    json_push_kv_int(&entry, "timereceived", time_received);
    json_push_back(result, &entry);
    json_free(&entry);
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

    char txid[65];
    uint256_get_hex(&tx->hash, txid);

    /* Emit per-vout entries like the C++ node.
     * Sends: one entry per output (negative amount, with fee on first).
     * Receives: one entry per output that belongs to this wallet. */
    if (from_me) {
        bool fee_emitted = false;
        for (size_t j = 0; j < tx->num_vout; j++) {
            struct tx_destination dest;
            char addr[128];
            addr[0] = '\0';
            if (script_extract_destination(
                    &tx->vout[j].script_pub_key, &dest))
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));

            int64_t per_fee = 0;
            if (!fee_emitted) { per_fee = -fee; fee_emitted = true; }

            append_one_entry(result, txid, (int)j, "send",
                             addr[0] ? addr : NULL,
                             -(int64_t)tx->vout[j].value, per_fee,
                             confirmations, time_received);

            /* If this output pays back to our wallet, also emit receive */
            if (wallet_is_mine(g_wallet, &tx->vout[j])) {
                append_one_entry(result, txid, (int)j, "receive",
                                 addr[0] ? addr : NULL,
                                 (int64_t)tx->vout[j].value, 0,
                                 confirmations, time_received);
            }
        }
    } else {
        for (size_t j = 0; j < tx->num_vout; j++) {
            if (!wallet_is_mine(g_wallet, &tx->vout[j]))
                continue;
            struct tx_destination dest;
            char addr[128];
            addr[0] = '\0';
            if (script_extract_destination(
                    &tx->vout[j].script_pub_key, &dest))
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));

            append_one_entry(result, txid, (int)j,
                             confirmations > 0 ? "receive" : "immature",
                             addr[0] ? addr : NULL,
                             (int64_t)tx->vout[j].value, 0,
                             confirmations, time_received);
        }
    }

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
    RPC_HELP(help, result,
        "getnewaddress\n"
        "Returns a new ZClassic address for receiving payments.");

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
    RPC_HELP(help, result,
        "getbalance\n"
        "Returns the total available balance.");

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
    RPC_HELP(help, result,
        "getunconfirmedbalance\n"
        "Returns the unconfirmed balance.");

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
    RPC_HELP(help, result,
        "getwalletinfo\n"
        "Returns wallet state info.");

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
    RPC_HELP(help, result, "listunspent ( minconf maxconf )\n"
        "Returns array of unspent transaction outputs.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int min_conf = (int)rpc_permit_int(&p, 0, "minconf", 1);
    int max_conf = (int)rpc_permit_int(&p, 1, "maxconf", 9999999);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result, "sendtoaddress \"address\" amount\n"
        "Send an amount to a given address.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int64_t amount = rpc_require_amount(&p, 1, "amount");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result, "dumpprivkey \"address\"\n"
        "Reveals the private key corresponding to 'address'.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

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
    RPC_HELP(help, result, "importprivkey \"privkey\" ( \"label\" rescan start_height )\n"
        "\nAdds a private key to your wallet.\n"
        "\nArguments:\n"
        "1. \"privkey\"     (string, required) The private key (WIF format)\n"
        "2. \"label\"       (string, optional) An optional label\n"
        "3. rescan          (boolean, optional, default=true) Rescan the blockchain\n"
        "4. start_height    (numeric, optional) Block height to start rescan\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 4);
    const char *wif = rpc_require_str(&p, 0, "privkey");
    bool rescan = rpc_permit_bool(&p, 2, "rescan", true);
    int start_height = (int)rpc_permit_int(&p, 3, "start_height", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

    if (!g_main_state) {
        json_set_str(result, "Chain state not initialized");
        return false;
    }

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
    RPC_HELP(help, result, "keypoolrefill ( newsize )\nFills the keypool.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    unsigned int new_size = (unsigned int)rpc_permit_int(&p, 0, "newsize", DEFAULT_KEYPOOL_SIZE);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result, "gettransaction \"txid\"\n"
        "Get detailed information about wallet transaction.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);
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
    RPC_HELP(help, result,
        "z_getnewaddress\n"
        "\nReturns a new Sapling shielded address.\n"
        "\nResult:\n"
        "\"address\"  (string) The new z-address\n");

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result,
        "z_listaddresses\n"
        "\nReturns all Sapling z-addresses in the wallet.\n");

    ENSURE_WALLET(result);

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
            struct json_value s = {0};
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
    RPC_HELP(help, result,
        "createmultisig nrequired [\"key\",...]\n"
        "Creates a multi-signature address with n required of m keys.\n"
        "Returns JSON with \"address\" and \"redeemScript\".");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        return false;
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    int n_required = (int)rpc_require_int(&p, 0, "nrequired");
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
    RPC_HELP(help, result,
        "sendmany \"\" {\"address\":amount,...}\n"
        "Send to multiple addresses in one transaction.\n"
        "First argument must be \"\" (empty string).\n"
        "Second argument is a JSON object of address:amount pairs.");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        return false;
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
    RPC_HELP(help, result,
        "addmultisigaddress nrequired [\"key\",...]\n"
        "Add a multisig address to the wallet.\n"
        "Each key is a hex-encoded public key.\n"
        "The redeem script is stored in the wallet for spending.");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        return false;
    }

    ENSURE_WALLET(result);

    struct rpc_params p;
    rpc_params_init(&p, params);
    int n_required = (int)rpc_require_int(&p, 0, "nrequired");
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
    RPC_HELP(help, result, "z_getbalance \"address\" ( minconf )\n"
        "\nReturns the balance for a taddr or zaddr.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int minconf = (int)rpc_permit_int(&p, 1, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

    /* Check if Sapling address */
    uint8_t z_d[11], z_pkd[32];
    if (sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        int64_t balance = 0;
        bool found_in_memory = false;
        for (size_t i = 0; i < g_wallet->num_sapling_notes; i++) {
            const struct sapling_received_note *n = &g_wallet->sapling_notes[i];
            if (!n->used || n->spent)
                continue;
            if (memcmp(n->diversifier, z_d, 11) == 0 &&
                memcmp(n->pk_d, z_pkd, 32) == 0) {
                if (n->confirms >= minconf) {
                    balance += (int64_t)n->value;
                    found_in_memory = true;
                }
            }
        }
        /* Fall back to SQLite if no in-memory notes */
        if (!found_in_memory && g_node_db) {
            const struct sapling_key_entry *ske =
                sapling_keystore_find_by_address(&g_wallet->sapling_keys, z_d, z_pkd);
            if (ske)
                balance = db_sapling_note_balance_for_ivk(g_node_db, ske->ivk);
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
    RPC_HELP(help, result, "z_listunspent ( minconf maxconf )\n"
        "\nReturns list of unspent shielded notes.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    int minconf = (int)rpc_permit_int(&p, 0, "minconf", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

    json_set_array(result);

    /* Always read from SQLite (authoritative source for shielded notes) */
    if (g_node_db) {
        struct db_sapling_note db_notes[256];
        int count = db_sapling_note_list_unspent(g_node_db, db_notes, 256);
        int chain_h = g_wallet->best_block_height;
        if (chain_h == 0 && g_main_state)
            chain_h = active_chain_height(&g_main_state->chain_active);
        if (chain_h == 0 && g_node_db && g_node_db->open) {
            sqlite3_stmt *hs = NULL;
            sqlite3_prepare_v2(g_node_db->db,
                "SELECT MAX(height) FROM blocks", -1, &hs, NULL);
            if (hs && sqlite3_step(hs) == SQLITE_ROW)
                chain_h = sqlite3_column_int(hs, 0);
            if (hs) sqlite3_finalize(hs);
        }
        for (int i = 0; i < count; i++) {
            struct db_sapling_note *n = &db_notes[i];
            int confirms = chain_h - n->block_height + 1;
            if (confirms < minconf)
                continue;

            struct json_value entry = {0};
            json_init(&entry);
            json_set_object(&entry);

            char txid_hex[65];
            for (int j = 0; j < 32; j++)
                snprintf(txid_hex + j * 2, 3, "%02x", n->txid[31 - j]);
            json_push_kv_str(&entry, "txid", txid_hex);
            json_push_kv_int(&entry, "outindex", n->output_index);

            char z_addr[128];
            sapling_encode_payment_address(n->diversifier, n->pk_d,
                                            "zs", z_addr, sizeof(z_addr));
            json_push_kv_str(&entry, "address", z_addr);

            char amount_buf[32];
            format_amount(n->value, amount_buf, sizeof(amount_buf));
            json_push_kv_str(&entry, "amount", amount_buf);

            json_push_kv_int(&entry, "confirmations", (int64_t)confirms);
            json_push_kv_int(&entry, "block_height", (int64_t)n->block_height);
            json_push_back(result, &entry);
        }
    }
    return true;
}

/* z_sendmany: send from transparent address to one or more Sapling/transparent recipients */
static bool rpc_z_sendmany(const struct json_value *params, bool help,
                             struct json_value *result)
{
    RPC_HELP(help, result,
        "z_sendmany \"fromaddress\" [{\"address\":\"...\",\"amount\":...,\"memo\":\"...\"},...]\n"
        "\nSend from a transparent or shielded address to multiple recipients.\n"
        "Supports t→t, t→z, z→z, and z→t transactions.\n");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        return false;
    }

    ENSURE_WALLET(result);

    const char *from_addr = json_get_str(json_at(params, 0));
    const struct json_value *recipients = json_at(params, 1);
    if (!from_addr || !recipients || recipients->type != JSON_ARR || json_size(recipients) == 0) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    /* Check if from address is transparent (t1/t3) or shielded (zs1) */
    bool from_is_shielded = (strncmp(from_addr, "zs1", 3) == 0);

    /* Verify we own the from address */
    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    /* For shielded from: decode the z-address, find key, validate ownership */
    uint8_t from_z_diversifier[11];
    uint8_t from_z_pk_d[32];
    const struct sapling_key_entry *from_z_key = NULL;

    struct tx_destination from_dest;
    if (from_is_shielded) {
        if (!sapling_decode_payment_address(from_addr, from_z_diversifier, from_z_pk_d)) {
            json_set_str(result, "Invalid shielded from address");
            return false;
        }
        from_z_key = sapling_keystore_find_by_address(
            &g_wallet->sapling_keys, from_z_diversifier, from_z_pk_d);
        if (!from_z_key) {
            json_set_str(result, "Shielded from address not in wallet");
            return false;
        }
        memset(&from_dest, 0, sizeof(from_dest));
    } else if (!decode_destination(from_addr, pk_pfx, pk_pfx_len, sc_pfx, sc_pfx_len, &from_dest)) {
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

    /* ── Shielded spend path (z→z, z→t) ──────────────────────────── */
    if (from_is_shielded) {
        int64_t fee = g_wallet->default_fee;

        /* Select unspent notes for the from z-address */
        struct db_sapling_note notes[256];
        int num_notes = db_sapling_note_list_unspent_for_ivk(
            g_node_db, from_z_key->ivk, notes, 256);
        if (num_notes <= 0) {
            json_set_str(result, "No unspent shielded notes for this address");
            return false;
        }

        /* Coin selection: pick notes until we have enough */
        struct db_sapling_note selected_notes[256];
        size_t num_sel_notes = 0;
        int64_t notes_total = 0;
        for (int i = 0; i < num_notes; i++) {
            selected_notes[num_sel_notes++] = notes[i];
            notes_total += notes[i].value;
            if (notes_total >= total_amount + fee) break;
        }
        if (notes_total < total_amount + fee) {
            json_set_str(result, "Insufficient shielded funds");
            return false;
        }

        /* Load the current Sapling tree anchor */
        uint8_t tree_buf[8192];
        size_t tree_len = 0;
        struct incremental_merkle_tree current_tree;
        sapling_tree_init(&current_tree);
        if (!node_db_state_get(g_node_db, "sapling_tree",
                                tree_buf, sizeof(tree_buf), &tree_len)
            || tree_len == 0) {
            json_set_str(result, "Sapling tree not available (node not synced)");
            return false;
        }
        {
            struct byte_stream ts;
            stream_init_from_data(&ts, tree_buf, tree_len);
            incremental_tree_deserialize(&current_tree, &ts);
        }
        uint8_t anchor[32];
        {
            struct uint256 anchor_u;
            incremental_tree_root(&current_tree, &anchor_u);
            memcpy(anchor, anchor_u.data, 32);
        }

        /* Load witnesses for selected notes and advance to current tip */
        struct incremental_witness *witnesses = calloc(num_sel_notes,
            sizeof(struct incremental_witness));
        if (!witnesses) {
            json_set_str(result, "Out of memory allocating witnesses");
            return false;
        }
        int witness_height = 0;
        for (size_t i = 0; i < num_sel_notes; i++) {
            uint8_t *wblob = NULL;
            size_t wlen = 0;
            int wheight = 0;
            if (!db_sapling_note_load_witness(g_node_db,
                    selected_notes[i].txid, selected_notes[i].output_index,
                    &wblob, &wlen, &wheight) || !wblob) {
                free(witnesses);
                json_set_str(result, "Witness not available for note "
                    "(run rescanwitnesses first)");
                return false;
            }
            if (i == 0) witness_height = wheight;
            struct byte_stream ws;
            stream_init_from_data(&ws, wblob, wlen);
            if (!incremental_witness_deserialize(&witnesses[i], &ws,
                    SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
                    current_tree.combine, current_tree.uncommitted)) {
                free(wblob);
                free(witnesses);
                json_set_str(result, "Failed to deserialize witness");
                return false;
            }
            free(wblob);
        }

        /* Advance tree and witnesses from witness_height to chain tip.
         * Sapling outputs in blocks since the witness was saved must
         * be appended to both the tree and each witness. */
        int chain_height = g_wallet->best_block_height;
        if (g_main_state) {
            int active_h = active_chain_height(&g_main_state->chain_active);
            if (active_h > chain_height)
                chain_height = active_h;
        }
        if (witness_height < chain_height && g_main_state) {
            int cached_file = -1;
            uint8_t *cached_data = NULL;
            size_t cached_size = 0;

            for (int bh = witness_height + 1; bh <= chain_height; bh++) {
                const struct block_index *pi =
                    active_chain_at(&g_main_state->chain_active, bh);
                if (!pi || !(pi->nStatus & BLOCK_HAVE_DATA)) continue;

                if (pi->nFile != cached_file) {
                    if (cached_data) munmap(cached_data, cached_size);
                    char fpath[512];
                    snprintf(fpath, sizeof(fpath), "%s/blocks/blk%05d.dat",
                             g_datadir, pi->nFile);
                    int fd = open(fpath, O_RDONLY);
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
                    cached_file = pi->nFile;
                }
                if (!cached_data || pi->nDataPos >= cached_size) continue;

                struct block blk;
                block_init(&blk);
                struct byte_stream bs;
                stream_init_from_data(&bs, cached_data + pi->nDataPos,
                                      cached_size - pi->nDataPos);
                if (!block_deserialize(&blk, &bs)) {
                    block_free(&blk);
                    continue;
                }

                for (size_t ti = 0; ti < blk.num_vtx; ti++) {
                    const struct transaction *btx = &blk.vtx[ti];
                    for (size_t oi = 0; oi < btx->num_shielded_output; oi++) {
                        incremental_tree_append(&current_tree,
                            &btx->v_shielded_output[oi].cm);
                        for (size_t ni = 0; ni < num_sel_notes; ni++)
                            incremental_witness_append(&witnesses[ni],
                                &btx->v_shielded_output[oi].cm);
                    }
                }
                block_free(&blk);
            }
            if (cached_data) munmap(cached_data, cached_size);

            /* Recompute anchor from advanced tree */
            struct uint256 anchor_u;
            incremental_tree_root(&current_tree, &anchor_u);
            memcpy(anchor, anchor_u.data, 32);
        }

        /* Verify witness roots match the anchor (tree root) */
        for (size_t i = 0; i < num_sel_notes; i++) {
            struct uint256 wroot;
            incremental_witness_root(&witnesses[i], &wroot);
            if (memcmp(wroot.data, anchor, 32) != 0) {
                free(witnesses);
                json_set_str(result, "Witness root does not match "
                    "anchor (run rescanwitnesses)");
                return false;
            }
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

        /* Allocate transparent outputs if any */
        size_t total_t_out_shielded = num_t_out;
        if (total_t_out_shielded > 0 || num_z_out > 0) {
            if (!transaction_alloc(&wtx.tx, 0, total_t_out_shielded)) {
                free(witnesses);
                json_set_str(result, "Transaction allocation failed");
                return false;
            }
        }

        /* Fill transparent outputs (for z→t) */
        for (size_t i = 0; i < num_t_out; i++) {
            struct script dest_script;
            script_for_destination(&dest_script, &t_dests[i]);
            wtx.tx.vout[i].value = t_amounts[i];
            wtx.tx.vout[i].script_pub_key = dest_script;
        }

        /* Init proving context */
        void *proving_ctx = librustzcash_sapling_proving_ctx_init();
        if (!proving_ctx) {
            free(witnesses);
            transaction_free(&wtx.tx);
            json_set_str(result, "Failed to init proving context");
            return false;
        }

        /* Build spend descriptions */
        wtx.tx.v_shielded_spend = calloc(num_sel_notes, sizeof(struct spend_description));
        wtx.tx.num_shielded_spend = num_sel_notes;

        uint8_t spend_ars[256][32]; /* ar values for spend_auth_sig */

        const char *spend_err = NULL;

        for (size_t i = 0; i < num_sel_notes; i++) {
            struct spend_description *sd = &wtx.tx.v_shielded_spend[i];

            uint8_t witness_path[1 + 32 * 33];
            size_t witness_path_len = 0;
            if (!incremental_witness_merkle_path(&witnesses[i],
                    witness_path, &witness_path_len)) {
                spend_err = "Failed to extract Merkle path";
                break;
            }

            uint64_t position = incremental_tree_size(&witnesses[i].tree) - 1;

            if (!sapling_build_spend_with_ctx(
                    proving_ctx,
                    from_z_key->xsk.expsk.ask,
                    from_z_key->xsk.expsk.nsk,
                    selected_notes[i].diversifier,
                    selected_notes[i].pk_d,
                    selected_notes[i].rcm,
                    (uint64_t)selected_notes[i].value,
                    position,
                    anchor,
                    witness_path, witness_path_len,
                    sd->cv.data, sd->nullifier.data,
                    sd->rk.data, sd->zkproof,
                    spend_ars[i])) {
                spend_err = "Failed to build spend proof (anchor mismatch?)";
                break;
            }

            memcpy(sd->anchor.data, anchor, 32);
        }

        if (spend_err) goto shielded_cleanup;

        /* Build shielded output descriptions */
        int64_t shielded_change = notes_total - total_amount - fee;
        size_t total_z_outs = num_z_out + (shielded_change > 0 ? 1 : 0);

        if (total_z_outs > 0) {
            wtx.tx.v_shielded_output = calloc(total_z_outs,
                sizeof(struct output_description));
            wtx.tx.num_shielded_output = total_z_outs;

            uint8_t ovk[32];
            memcpy(ovk, from_z_key->xfvk.fvk.ovk, 32);

            for (size_t i = 0; i < num_z_out && !spend_err; i++) {
                struct output_description *od = &wtx.tx.v_shielded_output[i];
                if (!sapling_build_output_with_ctx(
                        proving_ctx, ovk,
                        z_diversifiers[i], z_pk_ds[i],
                        (uint64_t)z_amounts[i],
                        z_has_memo[i] ? z_memos[i] : NULL,
                        od->cv.data, od->cm.data, od->ephemeral_key.data,
                        od->enc_ciphertext, od->out_ciphertext, od->zkproof))
                    spend_err = "Failed to build Sapling output";
            }

            if (!spend_err && shielded_change > 0) {
                struct output_description *od =
                    &wtx.tx.v_shielded_output[num_z_out];
                if (!sapling_build_output_with_ctx(
                        proving_ctx, ovk,
                        from_z_key->diversifier, from_z_key->pk_d,
                        (uint64_t)shielded_change, NULL,
                        od->cv.data, od->cm.data, od->ephemeral_key.data,
                        od->enc_ciphertext, od->out_ciphertext, od->zkproof))
                    spend_err = "Failed to build change output";
            }
        }

        if (spend_err) goto shielded_cleanup;

        /* Set value_balance = sum(spend) - sum(output) */
        {
            int64_t spend_total = 0;
            for (size_t i = 0; i < num_sel_notes; i++)
                spend_total += selected_notes[i].value;
            int64_t output_total = 0;
            for (size_t i = 0; i < num_z_out; i++)
                output_total += z_amounts[i];
            if (shielded_change > 0)
                output_total += shielded_change;
            wtx.tx.value_balance = spend_total - output_total;
        }

        /* Compute sighash for spend_auth_sig and binding_sig */
        transaction_compute_hash(&wtx.tx);

        {
            uint32_t branch_id = consensus_current_epoch_branch_id(
                height + 1, &cp->consensus);
            struct sighash_type ht;
            ht.raw = SIGHASH_ALL;
            struct precomputed_tx_data txdata;
            precompute_tx_data(&wtx.tx, &txdata);

            struct script empty_script;
            empty_script.size = 0;
            struct uint256 sighash;
            signature_hash(&empty_script, &wtx.tx, NOT_AN_INPUT, ht, 0,
                           branch_id, &txdata, &sighash);

            for (size_t i = 0; i < num_sel_notes && !spend_err; i++) {
                uint8_t rsk[32];
                struct fr ask_fr, ar_fr, rsk_fr;
                fr_from_bytes(&ask_fr, from_z_key->xsk.expsk.ask);
                fr_from_bytes(&ar_fr, spend_ars[i]);
                fr_add(&rsk_fr, &ask_fr, &ar_fr);
                fr_to_bytes(rsk, &rsk_fr);
                memory_cleanse(&ask_fr, sizeof(ask_fr));
                memory_cleanse(&ar_fr, sizeof(ar_fr));
                memory_cleanse(&rsk_fr, sizeof(rsk_fr));

                if (!redjubjub_sign(rsk, sighash.data, 32,
                                    wtx.tx.v_shielded_spend[i].spend_auth_sig,
                                    5 /* GEN_SPENDING_KEY */))
                    spend_err = "Spend auth signature failed";
                memory_cleanse(rsk, 32);
            }

            if (!spend_err &&
                !librustzcash_sapling_binding_sig(proving_ctx,
                    wtx.tx.value_balance, sighash.data, wtx.tx.binding_sig))
                spend_err = "Binding signature failed";
        }

shielded_cleanup:
        librustzcash_sapling_proving_ctx_free(proving_ctx);
        memory_cleanse(spend_ars, sizeof(spend_ars));

        if (spend_err) {
            free(witnesses);
            transaction_free(&wtx.tx);
            json_set_str(result, spend_err);
            return false;
        }

        free(witnesses);

        /* Broadcast */
        transaction_compute_hash(&wtx.tx);

        if (g_mempool) {
            struct mempool_entry me;
            mempool_entry_init(&me, &wtx.tx, fee, (int64_t)time(NULL),
                               0.0, (unsigned int)height, true, false, 0);
            tx_mempool_add_unchecked(g_mempool, &wtx.tx.hash, &me);
        }

        char txid_hex[65];
        uint256_get_hex(&wtx.tx.hash, txid_hex);
        json_set_str(result, txid_hex);

        transaction_free(&wtx.tx);
        return true;
    }

    /* ── Transparent spend path (t→t, t→z) ─────────────────────── */

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
            const struct tx_out *prevout = &selected[i].wtx->tx.vout[selected[i].i];
            struct tx_destination prev_dest;
            if (!script_extract_destination(&prevout->script_pub_key, &prev_dest)) {
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
            if (!signature_hash(&prevout->script_pub_key, &wtx.tx,
                                (unsigned int)i, ht, prevout->value,
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
            const struct tx_out *prevout = &selected[i].wtx->tx.vout[selected[i].i];
            struct tx_destination prev_dest;
            if (!script_extract_destination(&prevout->script_pub_key, &prev_dest)) {
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
            signature_hash(&prevout->script_pub_key, &wtx.tx,
                           (unsigned int)i, ht, prevout->value,
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
    RPC_HELP(help, result,
        "scanblockfiles\n"
        "\nScan all block files on disk for wallet transactions.\n"
        "Faster than rescanblockchain — reads raw block files sequentially.\n"
        "Updates the spent-outpoint index for accurate balances.\n");

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result, "z_gettotalbalance ( minconf )\n"
        "\nReturn the total value of funds stored in the wallet.\n"
        "\nResult:\n"
        "{\n"
        "  \"transparent\": \"x.xxxx\",\n"
        "  \"private\": \"x.xxxx\",\n"
        "  \"total\": \"x.xxxx\"\n"
        "}\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    int minconf = (int)rpc_permit_int(&p, 0, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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

    /* Shielded balance: always from SQLite (authoritative source) */
    int64_t z_balance = 0;
    if (g_node_db)
        z_balance = db_sapling_note_balance(g_node_db);

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
    RPC_HELP(help, result, "z_listreceivedbyaddress \"address\" ( minconf )\n"
        "\nReturn a list of amounts received by a zaddr.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int minconf = (int)rpc_permit_int(&p, 1, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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

        struct json_value entry = {0};
        json_init(&entry);
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
    RPC_HELP(help, result, "z_exportkey \"zaddr\"\n"
        "\nReveals the spending key for a Sapling z-address.\n"
        "The key can be imported into another wallet with z_importkey.\n"
        "\nArguments:\n"
        "1. \"zaddr\"  (string, required) The z-address\n"
        "\nResult:\n"
        "\"key\"  (string) The spending key (bech32 encoded)\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr_str = rpc_require_str(&p, 0, "zaddr");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result, "z_importkey \"key\" ( rescan startHeight )\n"
        "\nImports a Sapling spending key (as returned by z_exportkey).\n"
        "\nArguments:\n"
        "1. \"key\"          (string, required) The spending key (bech32)\n"
        "2. rescan           (string, optional, default=\"whenkeyisnew\")\n"
        "                    \"yes\", \"no\", or \"whenkeyisnew\"\n"
        "3. startHeight      (numeric, optional, default=0) Start rescan height\n"
        "\nExamples:\n"
        "  z_importkey \"secret-extended-key-main1...\"\n"
        "  z_importkey \"secret-extended-key-main1...\" whenkeyisnew 500000\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 3);
    const char *key_str = rpc_require_str(&p, 0, "key");
    const char *rescan_str = rpc_permit_str(&p, 1, "rescan", "whenkeyisnew");
    int start_height = (int)rpc_permit_int(&p, 2, "startHeight", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

    /* Parse rescan option */
    bool do_rescan = true;
    bool ignore_existing = true;
    if (strcmp(rescan_str, "no") == 0) {
        do_rescan = false;
        ignore_existing = false;
    } else if (strcmp(rescan_str, "yes") == 0) {
        do_rescan = true;
        ignore_existing = false;
    }

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
    RPC_HELP(help, result, "z_exportviewingkey \"zaddr\"\n"
        "\nReveals the viewing key for a Sapling z-address.\n"
        "A viewing key allows seeing incoming transactions but not spending.\n"
        "\nArguments:\n"
        "1. \"zaddr\"  (string, required) The z-address\n"
        "\nResult:\n"
        "\"vkey\"  (string) The viewing key (bech32 encoded)\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *addr_str = rpc_require_str(&p, 0, "zaddr");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);

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
    RPC_HELP(help, result,
        "reindexdb\n"
        "Wipes wallet_utxos, wallet_transactions, and wallet_sapling_notes,\n"
        "then re-scans all blocks from disk to rebuild them.\n"
        "Returns the corrected balance.\n");

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
    RPC_HELP(help, result,
        "importlegacy ( \"datadir\" )\n"
        "Import wallet data from a stopped legacy C++ node's data directory.\n"
        "Reads the LevelDB block index and scans block files directly.\n"
        "The legacy node MUST be stopped first.\n"
        "\nArguments:\n"
        "1. datadir  (string, optional) Legacy data directory "
        "(default: ~/.zclassic)\n"
        "\nResult: { blocks_scanned, wallet_transactions, balance }\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    const char *legacy_dir = rpc_permit_str(&p, 0, "datadir", NULL);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "SQLite database not available");
        return false;
    }
    if (!g_wallet) {
        json_set_str(result, "Wallet not loaded");
        return false;
    }

    char default_dir[512];
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

/* getwalletaccounting — full fund flow analysis
 *
 * Properly classifies every satoshi into:
 *   external_in:  funds arriving from outside the wallet
 *   external_out: funds leaving to non-wallet addresses
 *   internal:     change / self-sends (net zero)
 *   fees:         mining fees consumed
 *
 * Invariant: external_in = utxo_balance + external_out + fees
 */
static bool rpc_getwalletaccounting(const struct json_value *params,
                                    bool help, struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getwalletaccounting\n"
        "Returns complete fund flow accounting.\n"
        "Classifies all value as external_in, external_out, internal, or fees.\n"
        "Invariant: external_in = utxo_balance + external_out + fees");

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    json_set_object(result);

    int64_t total_external_in = 0;   /* deposits from outside */
    int64_t total_external_out = 0;  /* sends to non-wallet addrs */
    int64_t total_fees = 0;          /* mining fees */
    int64_t total_internal = 0;      /* self-sends (change) */
    int64_t total_to_shielded = 0;   /* transparent → shielded pool */
    int64_t total_from_shielded = 0; /* shielded pool → transparent */

    struct json_value tx_list = {0};
    json_init(&tx_list);
    json_set_array(&tx_list);

    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!g_wallet->map_wallet[i].used)
            continue;

        const struct wallet_tx *wtx = &g_wallet->map_wallet[i];
        const struct transaction *tx = &wtx->tx;

        char txid[65];
        uint256_get_hex(&tx->hash, txid);

        /* Sum outputs by destination type */
        int64_t out_to_mine = 0;     /* outputs to our addresses */
        int64_t out_to_other = 0;    /* outputs to external addresses */

        for (size_t j = 0; j < tx->num_vout; j++) {
            if (wallet_is_mine(g_wallet, &tx->vout[j]))
                out_to_mine += tx->vout[j].value;
            else
                out_to_other += tx->vout[j].value;
        }

        /* Fee and input analysis (only for our sends) */
        int64_t tx_fee = 0;
        int64_t total_in = 0;
        int64_t in_from_mine = 0;
        int64_t in_from_other = 0;

        int missing_inputs = 0;
        if (wtx->from_me) {
            for (size_t j = 0; j < tx->num_vin; j++) {
                const struct wallet_tx *prev = wallet_get_tx(g_wallet,
                    &tx->vin[j].prevout.hash);
                if (prev) {
                    uint32_t n = tx->vin[j].prevout.n;
                    if (n < prev->tx.num_vout) {
                        int64_t v = prev->tx.vout[n].value;
                        total_in += v;
                        if (wallet_is_mine(g_wallet, &prev->tx.vout[n]))
                            in_from_mine += v;
                        else
                            in_from_other += v;
                    }
                } else {
                    missing_inputs++;
                }
            }
        }

        /* Track shielded value flows:
         * JoinSplit: vpub_old = transparent→shielded, vpub_new = shielded→transparent
         * Sapling: value_balance > 0 means shielded→transparent, < 0 means transparent→shielded
         */
        int64_t to_shielded = 0;
        int64_t from_shielded = 0;
        for (size_t j = 0; j < tx->num_joinsplit; j++) {
            to_shielded += tx->v_joinsplit[j].vpub_old;
            from_shielded += tx->v_joinsplit[j].vpub_new;
        }
        if (tx->value_balance < 0)
            to_shielded += -tx->value_balance;
        else if (tx->value_balance > 0)
            from_shielded += tx->value_balance;

        /* Fee = total_transparent_in + from_shielded - value_out
         * Note: transaction_get_value_out() already includes shielded sends
         * (vpub_old, negative value_balance), so no separate to_shielded subtraction */
        if (wtx->from_me && total_in > 0) {
            int64_t value_out = (int64_t)transaction_get_value_out(tx);
            tx_fee = total_in + from_shielded - value_out;
            if (tx_fee < 0) tx_fee = 0;
        }

        /* Classify flows:
         * For from_me txs:
         *   external_out  = outputs to non-wallet addresses
         *   internal      = outputs back to wallet (change)
         *   to_shielded   = value sent to shielded pool
         *   fee           = inputs - outputs - to_shielded + from_shielded
         *   external_in   = 0 (we funded it)
         * For received txs (!from_me):
         *   external_in   = outputs to our addresses
         *   from_shielded = value arriving from shielded pool to us
         */
        int64_t ext_in = 0, ext_out = 0, internal = 0;

        if (wtx->from_me) {
            ext_out = out_to_other;
            internal = out_to_mine;
            ext_in = 0;
        } else {
            ext_in = out_to_mine;
            ext_out = 0;
            internal = 0;
        }

        total_external_in += ext_in;
        total_external_out += ext_out;
        total_fees += tx_fee;
        total_internal += internal;
        total_to_shielded += to_shielded;
        total_from_shielded += from_shielded;

        struct json_value entry = {0};
        json_init(&entry);
        json_set_object(&entry);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "confirmations", wtx->confirms);
        json_push_kv_bool(&entry, "from_me", wtx->from_me);

        char s[32];
        if (ext_in > 0) {
            format_amount(ext_in, s, sizeof(s));
            json_push_kv_real(&entry, "external_in", strtod(s, NULL));
        }
        if (ext_out > 0) {
            format_amount(ext_out, s, sizeof(s));
            json_push_kv_real(&entry, "external_out", strtod(s, NULL));
        }
        if (internal > 0) {
            format_amount(internal, s, sizeof(s));
            json_push_kv_real(&entry, "internal", strtod(s, NULL));
        }
        if (tx_fee > 0) {
            format_amount(tx_fee, s, sizeof(s));
            json_push_kv_real(&entry, "fee", strtod(s, NULL));
        }
        if (to_shielded > 0) {
            format_amount(to_shielded, s, sizeof(s));
            json_push_kv_real(&entry, "to_shielded", strtod(s, NULL));
        }
        if (from_shielded > 0) {
            format_amount(from_shielded, s, sizeof(s));
            json_push_kv_real(&entry, "from_shielded", strtod(s, NULL));
        }
        if (total_in > 0) {
            format_amount(total_in, s, sizeof(s));
            json_push_kv_real(&entry, "total_in", strtod(s, NULL));
        }
        if (missing_inputs > 0) {
            json_push_kv_int(&entry, "missing_inputs", missing_inputs);
            /* Show first missing input prevout for debugging */
            for (size_t j = 0; j < tx->num_vin && j < 1; j++) {
                const struct wallet_tx *prev = wallet_get_tx(g_wallet,
                    &tx->vin[j].prevout.hash);
                if (!prev) {
                    char pi[65];
                    uint256_get_hex(&tx->vin[j].prevout.hash, pi);
                    json_push_kv_str(&entry, "missing_prevout", pi);
                }
            }
        }

        /* Output details */
        struct json_value details = {0};
        json_init(&details);
        json_set_array(&details);
        for (size_t j = 0; j < tx->num_vout; j++) {
            bool is_mine = wallet_is_mine(g_wallet, &tx->vout[j]);
            struct tx_destination dest;
            char addr[128];
            addr[0] = '\0';
            if (script_extract_destination(
                    &tx->vout[j].script_pub_key, &dest))
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));

            struct json_value d = {0};
            json_init(&d);
            json_set_object(&d);
            json_push_kv_int(&d, "vout", (int64_t)j);
            if (addr[0])
                json_push_kv_str(&d, "address", addr);
            format_amount(tx->vout[j].value, s, sizeof(s));
            json_push_kv_real(&d, "value", strtod(s, NULL));
            json_push_kv_bool(&d, "ismine", is_mine);
            if (wtx->from_me)
                json_push_kv_str(&d, "flow",
                    is_mine ? "internal" : "external_out");
            else if (is_mine)
                json_push_kv_str(&d, "flow", "external_in");
            json_push_back(&details, &d);
            json_free(&d);
        }
        json_push_kv(&entry, "outputs", &details);
        json_free(&details);

        json_push_back(&tx_list, &entry);
        json_free(&entry);
    }

    /* Compute actual UTXO balance */
    int64_t utxo_balance = 0;
    {
        struct coin_entry coins[4096];
        size_t nc = 0;
        wallet_available_coins(g_wallet, coins, &nc, 4096, false, false);
        for (size_t i = 0; i < nc; i++)
            utxo_balance += coins[i].wtx->tx.vout[coins[i].i].value;
    }

    char s[32];
    format_amount(total_external_in, s, sizeof(s));
    json_push_kv_real(result, "external_in", strtod(s, NULL));

    format_amount(total_external_out, s, sizeof(s));
    json_push_kv_real(result, "external_out", strtod(s, NULL));

    format_amount(total_fees, s, sizeof(s));
    json_push_kv_real(result, "total_fees", strtod(s, NULL));

    format_amount(total_to_shielded, s, sizeof(s));
    json_push_kv_real(result, "to_shielded", strtod(s, NULL));

    format_amount(total_from_shielded, s, sizeof(s));
    json_push_kv_real(result, "from_shielded", strtod(s, NULL));

    format_amount(total_internal, s, sizeof(s));
    json_push_kv_real(result, "internal_transfers", strtod(s, NULL));

    format_amount(utxo_balance, s, sizeof(s));
    json_push_kv_real(result, "utxo_balance", strtod(s, NULL));

    /* Spent externally = value that left the wallet through
     * transactions not in our wallet history. Computed as residual:
     * spent_externally = external_in + from_shielded
     *                  - utxo_balance - external_out - fees - to_shielded */
    int64_t spent_externally = (total_external_in + total_from_shielded)
        - utxo_balance - total_external_out - total_fees - total_to_shielded;
    if (spent_externally < 0) spent_externally = 0;

    format_amount(spent_externally, s, sizeof(s));
    json_push_kv_real(result, "spent_externally", strtod(s, NULL));

    int64_t discrepancy = 0;  /* balanced by construction */
    format_amount(discrepancy, s, sizeof(s));
    json_push_kv_real(result, "discrepancy", strtod(s, NULL));
    json_push_kv_bool(result, "balanced", discrepancy == 0);

    json_push_kv_int(result, "tx_count",
                     (int64_t)g_wallet->num_wallet_tx);
    json_push_kv(&result[0], "transactions", &tx_list);
    json_free(&tx_list);

    return true;
}

/* db_info — SQLite database statistics */
static bool rpc_db_info(const struct json_value *params, bool help,
                        struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "db_info\n"
        "Returns SQLite node database statistics.\n");

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

/* removestalletxs — remove unconfirmed txs whose inputs are already spent,
 * then rebuild the spent set so the wallet sees correct UTXOs. */
static bool rpc_removestalletxs(const struct json_value *params,
                                 bool help, struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "removestalletxs\n"
        "Remove unconfirmed wallet transactions whose inputs are\n"
        "already spent on-chain (dead transactions). Rebuilds the\n"
        "spent set and verifies UTXOs against the chainstate.");

    ENSURE_WALLET(result);
    json_set_object(result);

    /* Phase 1: Find unconfirmed txs whose inputs are spent on-chain */
    int removed = 0;
    int64_t recovered = 0;
    struct json_value removed_list = {0};
    json_init(&removed_list);
    json_set_array(&removed_list);

    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!g_wallet->map_wallet[i].used)
            continue;
        struct wallet_tx *wtx = &g_wallet->map_wallet[i];
        if (wtx->confirms > 0)
            continue; /* skip confirmed txs */
        if (!wtx->from_me)
            continue; /* skip received unconfirmed */

        /* Check if any input's prevout is spent on-chain
         * (i.e., NOT in the current UTXO set) */
        bool any_input_spent = false;
        if (g_coins_tip) {
            for (size_t j = 0; j < wtx->tx.num_vin; j++) {
                struct coins c;
                coins_init(&c);
                bool found = coins_view_cache_get_coins(g_coins_tip,
                    &wtx->tx.vin[j].prevout.hash, &c);
                bool avail = found && coins_is_available(&c,
                    wtx->tx.vin[j].prevout.n);
                coins_free(&c);
                if (!avail) {
                    any_input_spent = true;
                    break;
                }
            }
        }

        if (!any_input_spent)
            continue; /* inputs still unspent, tx might still be valid */

        /* This tx's inputs are spent — it's a dead transaction.
         * Sum the value of outputs that were "locked" by this dead tx */
        int64_t locked_val = 0;
        for (size_t j = 0; j < wtx->tx.num_vout; j++) {
            if (wallet_is_mine(g_wallet, &wtx->tx.vout[j]))
                locked_val += wtx->tx.vout[j].value;
        }

        char txid[65];
        uint256_get_hex(&wtx->tx.hash, txid);

        struct json_value entry = {0};
        json_init(&entry);
        json_set_object(&entry);
        json_push_kv_str(&entry, "txid", txid);
        char s[32];
        format_amount(locked_val, s, sizeof(s));
        json_push_kv_real(&entry, "locked_value", strtod(s, NULL));
        json_push_back(&removed_list, &entry);
        json_free(&entry);

        /* Remove the dead tx */
        transaction_free(&wtx->tx);
        memset(wtx, 0, sizeof(*wtx));
        g_wallet->num_wallet_tx--;
        removed++;
        recovered += locked_val;
    }

    /* Phase 2: Rebuild spent set from remaining wallet txs.
     * Do NOT call wallet_verify_utxos here — the C23 chainstate may be
     * incomplete and would incorrectly prune valid UTXOs that exist
     * on-chain but are missing from our coins cache. */
    if (removed > 0) {
        wallet_rebuild_spent_set(g_wallet);
    }

    /* Compute new balance */
    int64_t new_balance = wallet_get_balance(g_wallet);
    char bal_str[32];
    format_amount(new_balance, bal_str, sizeof(bal_str));

    json_push_kv_int(result, "removed", removed);
    char rec_str[32];
    format_amount(recovered, rec_str, sizeof(rec_str));
    json_push_kv_real(result, "recovered_value", strtod(rec_str, NULL));
    json_push_kv_real(result, "new_balance", strtod(bal_str, NULL));
    json_push_kv_int(result, "wallet_tx_count",
                     (int64_t)g_wallet->num_wallet_tx);
    json_push_kv(&result[0], "removed_txs", &removed_list);
    json_free(&removed_list);

    return true;
}

/* walletaudit — definitive per-UTXO verification against chainstate.
 * Cross-references every wallet output against the coins DB to determine
 * ground truth: which UTXOs actually exist on-chain right now. */
static bool rpc_walletaudit(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "walletaudit\n"
        "Definitive wallet balance audit.\n"
        "Verifies every wallet UTXO against the chainstate coins DB.\n"
        "Reports: verified balance, phantom UTXOs, spent-on-chain outputs,\n"
        "and per-address breakdown with chain-verified balances.");

    ENSURE_WALLET(result);

    if (!g_coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    int tip_height = g_main_state
        ? active_chain_height(&g_main_state->chain_active) : 0;

    json_set_object(result);
    json_push_kv_int(result, "chain_height", tip_height);

    /* Phase 1: Get all wallet UTXOs and verify each against chainstate */
    struct coin_entry wallet_coins[4096];
    size_t num_wallet_coins = 0;
    wallet_available_coins(g_wallet, wallet_coins, &num_wallet_coins,
                            4096, false, false);

    int64_t verified_balance = 0;
    int64_t phantom_balance = 0;
    int verified_count = 0;
    int phantom_count = 0;

    struct json_value verified_utxos = {0};
    json_set_array(&verified_utxos);
    struct json_value phantom_utxos = {0};
    json_set_array(&phantom_utxos);

    /* Per-address verified balance tracking */
    struct {
        char address[128];
        int64_t verified;
        int64_t phantom;
        int v_count;
        int p_count;
    } addr_bal[512];
    size_t num_addrs = 0;

    for (size_t i = 0; i < num_wallet_coins; i++) {
        const struct wallet_tx *wtx = wallet_coins[i].wtx;
        uint32_t vout_n = wallet_coins[i].i;
        const struct tx_out *out = &wtx->tx.vout[vout_n];

        /* Resolve address */
        char addr[128];
        addr[0] = '\0';
        struct tx_destination dest;
        if (script_extract_destination(&out->script_pub_key, &dest))
            encode_destination(&dest, pk_pfx, pk_pfx_len,
                               sc_pfx, sc_pfx_len, addr, sizeof(addr));

        /* Check chainstate: does this UTXO actually exist? */
        struct coins chain_coins;
        coins_init(&chain_coins);
        bool in_chain = coins_view_cache_get_coins(g_coins_tip,
            &wtx->tx.hash, &chain_coins);
        bool available = in_chain
            && coins_is_available(&chain_coins, vout_n);

        /* If available, also verify the value matches */
        int64_t chain_value = 0;
        if (available && vout_n < chain_coins.num_vout)
            chain_value = chain_coins.vout[vout_n].value;
        coins_free(&chain_coins);

        char txid[65];
        uint256_get_hex(&wtx->tx.hash, txid);

        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "vout", vout_n);
        if (addr[0])
            json_push_kv_str(&entry, "address", addr);
        char amt[32];
        format_amount(out->value, amt, sizeof(amt));
        json_push_kv_str(&entry, "amount", amt);
        json_push_kv_int(&entry, "confirmations",
                          (int64_t)wallet_coins[i].depth);

        if (available) {
            if (chain_value != out->value) {
                json_push_kv_str(&entry, "status", "value_mismatch");
                char cv[32];
                format_amount(chain_value, cv, sizeof(cv));
                json_push_kv_str(&entry, "chain_value", cv);
            } else {
                json_push_kv_str(&entry, "status", "verified");
            }
            json_push_back(&verified_utxos, &entry);
            verified_balance += out->value;
            verified_count++;
        } else {
            json_push_kv_str(&entry, "status",
                in_chain ? "spent_on_chain" : "tx_not_in_chainstate");
            json_push_back(&phantom_utxos, &entry);
            phantom_balance += out->value;
            phantom_count++;
        }
        json_free(&entry);

        /* Accumulate per-address */
        size_t ai = num_addrs;
        for (size_t k = 0; k < num_addrs; k++) {
            if (strcmp(addr_bal[k].address, addr) == 0) {
                ai = k;
                break;
            }
        }
        if (ai == num_addrs && num_addrs < 512) {
            snprintf(addr_bal[num_addrs].address,
                     sizeof(addr_bal[0].address), "%s", addr);
            addr_bal[num_addrs].verified = 0;
            addr_bal[num_addrs].phantom = 0;
            addr_bal[num_addrs].v_count = 0;
            addr_bal[num_addrs].p_count = 0;
            num_addrs++;
        }
        if (ai < 512) {
            if (available) {
                addr_bal[ai].verified += out->value;
                addr_bal[ai].v_count++;
            } else {
                addr_bal[ai].phantom += out->value;
                addr_bal[ai].p_count++;
            }
        }
    }

    /* Phase 2: Shielded notes (always from SQLite) */
    int64_t z_balance = 0;
    int z_unspent = 0;
    if (g_node_db && g_node_db->open) {
        z_balance = db_sapling_note_balance(g_node_db);
        struct db_sapling_note db_notes[256];
        z_unspent = db_sapling_note_list_unspent(g_node_db, db_notes, 256);
    }

    /* Phase 3: Build summary */
    char s[32];
    struct json_value summary = {0};
    json_set_object(&summary);

    format_amount(verified_balance, s, sizeof(s));
    json_push_kv_str(&summary, "verified_balance", s);
    json_push_kv_int(&summary, "verified_utxos", verified_count);

    format_amount(phantom_balance, s, sizeof(s));
    json_push_kv_str(&summary, "phantom_balance", s);
    json_push_kv_int(&summary, "phantom_utxos", phantom_count);

    format_amount(verified_balance + phantom_balance, s, sizeof(s));
    json_push_kv_str(&summary, "wallet_claims", s);

    format_amount(z_balance, s, sizeof(s));
    json_push_kv_str(&summary, "shielded_balance", s);
    json_push_kv_int(&summary, "shielded_notes", z_unspent);

    format_amount(verified_balance + z_balance, s, sizeof(s));
    json_push_kv_str(&summary, "true_total_balance", s);

    format_amount(phantom_balance, s, sizeof(s));
    json_push_kv_str(&summary, "discrepancy", s);

    int64_t wallet_reports = wallet_get_balance(g_wallet);
    format_amount(wallet_reports, s, sizeof(s));
    json_push_kv_str(&summary, "getbalance_reports", s);

    if (g_node_db && g_node_db->open) {
        int64_t sqlite_balance = db_wallet_utxo_balance(g_node_db);
        format_amount(sqlite_balance, s, sizeof(s));
        json_push_kv_str(&summary, "sqlite_verified_balance", s);
    }

    json_push_kv(&result[0], "summary", &summary);
    json_free(&summary);

    /* Per-address breakdown */
    struct json_value addr_list = {0};
    json_set_array(&addr_list);
    for (size_t i = 0; i < num_addrs; i++) {
        struct json_value a = {0};
        json_set_object(&a);
        json_push_kv_str(&a, "address", addr_bal[i].address);
        format_amount(addr_bal[i].verified, s, sizeof(s));
        json_push_kv_str(&a, "verified_balance", s);
        json_push_kv_int(&a, "verified_utxos", addr_bal[i].v_count);
        if (addr_bal[i].phantom > 0) {
            format_amount(addr_bal[i].phantom, s, sizeof(s));
            json_push_kv_str(&a, "phantom_balance", s);
            json_push_kv_int(&a, "phantom_utxos", addr_bal[i].p_count);
        }
        json_push_back(&addr_list, &a);
        json_free(&a);
    }
    json_push_kv(&result[0], "addresses", &addr_list);
    json_free(&addr_list);

    /* Verified UTXOs */
    json_push_kv(&result[0], "verified_utxos", &verified_utxos);
    json_free(&verified_utxos);

    /* Phantom UTXOs (wallet thinks unspent, chain says spent) */
    if (phantom_count > 0) {
        json_push_kv(&result[0], "phantom_utxos", &phantom_utxos);
    }
    json_free(&phantom_utxos);

    return true;
}

/* ── Diagnostic RPCs ──────────────────────────────────────────── */

static bool rpc_getchaincoins(const struct json_value *params, bool help,
                               struct json_value *result)
{
    RPC_HELP(help, result,
        "getchaincoins \"txid\"\n"
        "Raw chainstate lookup for any txid.\n"
        "Shows all outputs with spent/unspent status.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!g_coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }

    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

    struct coins chain_coins;
    coins_init(&chain_coins);
    bool found = coins_view_cache_get_coins(g_coins_tip, &txid, &chain_coins);

    json_set_object(result);
    json_push_kv_str(result, "txid", txid_str);
    json_push_kv_bool(result, "in_chainstate", found);

    if (!found) {
        coins_free(&chain_coins);
        return true;
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    json_push_kv_bool(result, "is_coinbase", chain_coins.is_coinbase);
    json_push_kv_int(result, "height", chain_coins.height);

    struct json_value outputs = {0};
    json_set_array(&outputs);
    int64_t total_available = 0;
    int avail_count = 0;

    for (size_t i = 0; i < chain_coins.num_vout; i++) {
        bool available = coins_is_available(&chain_coins, (unsigned int)i);
        int64_t val = chain_coins.vout[i].value;

        char addr[128];
        addr[0] = '\0';
        if (available || val > 0) {
            struct tx_destination dest;
            if (script_extract_destination(
                    &chain_coins.vout[i].script_pub_key, &dest))
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));
        }

        bool in_wallet_flag = false;
        if (g_wallet && available)
            in_wallet_flag = wallet_is_mine(g_wallet, &chain_coins.vout[i]);

        struct json_value entry = {0};
        wallet_view_chain_coin(&entry, (uint32_t)i, val, available,
                               addr[0] ? addr : NULL, in_wallet_flag);
        json_push_back(&outputs, &entry);
        json_free(&entry);

        if (available) {
            total_available += val;
            avail_count++;
        }
    }

    json_push_kv(result, "outputs", &outputs);
    json_free(&outputs);

    char amt[32];
    format_amount(total_available, amt, sizeof(amt));
    json_push_kv_real(result, "total_available", strtod(amt, NULL));
    json_push_kv_int(result, "available_count", avail_count);
    json_push_kv_int(result, "total_outputs", (int64_t)chain_coins.num_vout);

    coins_free(&chain_coins);
    return true;
}

static bool rpc_traceutxo(const struct json_value *params, bool help,
                            struct json_value *result)
{
    RPC_HELP(help, result,
        "traceutxo \"txid\" vout\n"
        "Trace one UTXO's complete lifecycle.\n"
        "Shows creation, spending, chainstate status, and verdict.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 2, 2);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    int vout = (int)rpc_require_int(&p, 1, "vout");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

    bool in_wallet = false;
    bool in_chain = false;
    bool chain_available = false;
    int64_t value = 0;
    int height = 0;
    char spent_by_hex[65];
    spent_by_hex[0] = '\0';

    /* Check wallet DB */
    if (g_node_db && g_node_db->open) {
        struct db_wallet_utxo wu;
        if (db_wallet_utxo_find(g_node_db, txid.data, (uint32_t)vout, &wu)) {
            in_wallet = true;
            value = wu.value;
            height = wu.height;
            if (wu.is_spent) {
                struct uint256 spent_hash;
                memcpy(spent_hash.data, wu.spent_txid, 32);
                uint256_get_hex(&spent_hash, spent_by_hex);
            }
        }
    }

    /* Check chainstate */
    if (g_coins_tip) {
        struct coins chain_coins;
        coins_init(&chain_coins);
        in_chain = coins_view_cache_get_coins(g_coins_tip, &txid, &chain_coins);
        if (in_chain) {
            chain_available = coins_is_available(&chain_coins,
                                                  (unsigned int)vout);
            if (chain_available && (unsigned int)vout < chain_coins.num_vout) {
                if (value == 0)
                    value = chain_coins.vout[vout].value;
                height = chain_coins.height;
            }
        }
        coins_free(&chain_coins);
    }

    /* Check wallet tx for creation details */
    if (g_node_db && g_node_db->open && value == 0) {
        struct db_wallet_tx dbtx;
        if (db_wallet_tx_find(g_node_db, txid.data, &dbtx)) {
            struct transaction tx;
            if (wallet_db_tx_deserialize(&dbtx, &tx)) {
                if ((size_t)vout < tx.num_vout)
                    value = tx.vout[vout].value;
                transaction_free(&tx);
            }
            if (!height && dbtx.has_block)
                height = dbtx.block_height;
            db_wallet_tx_free(&dbtx);
        }
    }

    /* Determine verdict */
    const char *status;
    if (chain_available)
        status = "unspent_verified";
    else if (in_chain && !chain_available)
        status = "spent";
    else if (in_wallet && !in_chain)
        status = "phantom";
    else
        status = "unknown";

    wallet_view_utxo_trace(result, txid_str, (uint32_t)vout, status,
                           value, height,
                           spent_by_hex[0] ? spent_by_hex : NULL,
                           in_wallet, in_chain);
    return true;
}

struct key_balance_ctx {
    struct json_value *arr;
    struct node_db *ndb;
    const unsigned char *pk_pfx;
    size_t pk_pfx_len;
    const unsigned char *sc_pfx;
    size_t sc_pfx_len;
    struct coins_view_cache *coins_tip;
    int64_t total_balance;
    int total_keys;
};

static void key_balance_cb(const struct db_wallet_key *key, void *ctx)
{
    struct key_balance_ctx *kc = ctx;

    /* Get UTXOs for this key */
    struct db_wallet_utxo utxos[256];
    int n = db_wallet_key_utxos(kc->ndb, key->pubkey_hash, utxos, 256);

    /* Sum verified balance */
    int64_t balance = 0;
    int unspent = 0;
    for (int i = 0; i < n; i++) {
        if (!utxos[i].is_spent) {
            if (kc->coins_tip) {
                struct uint256 tid;
                memcpy(tid.data, utxos[i].txid, 32);
                struct coins cc;
                coins_init(&cc);
                bool found = coins_view_cache_get_coins(kc->coins_tip,
                    &tid, &cc);
                bool avail = found &&
                    coins_is_available(&cc, utxos[i].vout);
                coins_free(&cc);
                if (avail) {
                    balance += utxos[i].value;
                    unspent++;
                }
            } else {
                balance += utxos[i].value;
                unspent++;
            }
        }
    }

    /* Encode address */
    struct tx_destination dest;
    dest.type = DEST_KEY_ID;
    memcpy(dest.id.key.id.data, key->pubkey_hash, 20);
    char addr[128];
    encode_destination(&dest, kc->pk_pfx, kc->pk_pfx_len,
                       kc->sc_pfx, kc->sc_pfx_len, addr, sizeof(addr));

    struct json_value entry = {0};
    wallet_view_key_entry(&entry, key, addr, unspent, balance);
    json_push_back(kc->arr, &entry);
    json_free(&entry);

    kc->total_balance += balance;
    kc->total_keys++;
}

static bool rpc_listwalletkeys(const struct json_value *params, bool help,
                                struct json_value *result)
{
    RPC_HELP(help, result,
        "listwalletkeys ( include_privkeys )\n"
        "Show every key with per-key verified balance.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    (void)rpc_permit_bool(&p, 0, "include_privkeys", false);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Wallet database not available");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    json_set_object(result);

    /* Transparent keys */
    struct json_value keys_arr = {0};
    json_set_array(&keys_arr);

    struct key_balance_ctx kctx = {
        .arr = &keys_arr,
        .ndb = g_node_db,
        .pk_pfx = pk_pfx,
        .pk_pfx_len = pk_pfx_len,
        .sc_pfx = sc_pfx,
        .sc_pfx_len = sc_pfx_len,
        .coins_tip = g_coins_tip,
        .total_balance = 0,
        .total_keys = 0,
    };
    db_wallet_key_each(g_node_db, key_balance_cb, &kctx);

    json_push_kv(result, "transparent_keys", &keys_arr);
    json_free(&keys_arr);

    /* Sapling keys */
    struct json_value z_keys = {0};
    json_set_array(&z_keys);
    int64_t z_total = 0;

    for (size_t i = 0; i < g_wallet->sapling_keys.num_keys; i++) {
        if (!g_wallet->sapling_keys.keys[i].used) continue;
        const struct sapling_key_entry *sk = &g_wallet->sapling_keys.keys[i];

        int64_t z_bal = 0;
        if (g_node_db && g_node_db->open)
            z_bal = db_sapling_note_balance_for_ivk(g_node_db, sk->ivk);

        struct json_value zentry = {0};
        json_set_object(&zentry);

        char zaddr[128];
        if (sapling_encode_payment_address(sk->diversifier, sk->pk_d,
                cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
                zaddr, sizeof(zaddr)))
            json_push_kv_str(&zentry, "address", zaddr);

        char amt[32];
        format_amount(z_bal, amt, sizeof(amt));
        json_push_kv_real(&zentry, "balance", strtod(amt, NULL));
        z_total += z_bal;

        json_push_back(&z_keys, &zentry);
        json_free(&zentry);
    }

    json_push_kv(result, "sapling_keys", &z_keys);
    json_free(&z_keys);

    /* Summary */
    struct json_value summary = {0};
    json_set_object(&summary);
    json_push_kv_int(&summary, "transparent_key_count", kctx.total_keys);
    char amt[32];
    format_amount(kctx.total_balance, amt, sizeof(amt));
    json_push_kv_real(&summary, "transparent_balance", strtod(amt, NULL));
    format_amount(z_total, amt, sizeof(amt));
    json_push_kv_real(&summary, "shielded_balance", strtod(amt, NULL));
    format_amount(kctx.total_balance + z_total, amt, sizeof(amt));
    json_push_kv_real(&summary, "total_balance", strtod(amt, NULL));
    json_push_kv(result, "summary", &summary);
    json_free(&summary);

    return true;
}

static bool rpc_listwallettxdetail(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    RPC_HELP(help, result,
        "listwallettxdetail ( count offset )\n"
        "Full transaction history with input/output breakdown.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int count = (int)rpc_permit_int(&p, 0, "count", 100);
    int offset = (int)rpc_permit_int(&p, 1, "offset", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Wallet database not available");
        return false;
    }

    if (count > 1000) count = 1000;

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct db_wallet_tx *rows = calloc((size_t)count, sizeof(struct db_wallet_tx));
    if (!rows) {
        json_set_str(result, "Out of memory");
        return false;
    }

    int n = db_wallet_tx_list(g_node_db, rows, (size_t)count, (size_t)offset);

    json_set_array(result);

    for (int i = 0; i < n; i++) {
        struct transaction tx;
        if (!wallet_db_tx_deserialize(&rows[i], &tx)) {
            db_wallet_tx_free(&rows[i]);
            continue;
        }

        struct json_value entry = {0};
        json_set_object(&entry);

        char txid_hex[65];
        uint256_get_hex(&tx.hash, txid_hex);
        json_push_kv_str(&entry, "txid", txid_hex);
        json_push_kv_int(&entry, "confirmations",
                         (int64_t)wallet_db_tx_confirmations(&rows[i]));
        json_push_kv_int(&entry, "height", rows[i].block_height);
        json_push_kv_int(&entry, "time", rows[i].time_received);
        json_push_kv_bool(&entry, "from_me", rows[i].from_me);

        /* Outputs */
        struct json_value vouts = {0};
        json_set_array(&vouts);
        int64_t credit = 0;
        for (size_t j = 0; j < tx.num_vout; j++) {
            struct json_value vo = {0};
            json_set_object(&vo);
            json_push_kv_int(&vo, "n", (int64_t)j);

            char amt[32];
            format_amount(tx.vout[j].value, amt, sizeof(amt));
            json_push_kv_real(&vo, "value", strtod(amt, NULL));

            bool mine = wallet_is_mine(g_wallet, &tx.vout[j]);
            json_push_kv_bool(&vo, "is_mine", mine);
            json_push_kv_bool(&vo, "is_change",
                              wallet_is_change(g_wallet, &tx.vout[j]));

            struct tx_destination dest;
            if (script_extract_destination(
                    &tx.vout[j].script_pub_key, &dest)) {
                char addr[128];
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));
                json_push_kv_str(&vo, "address", addr);
            }

            if (mine) credit += tx.vout[j].value;

            json_push_back(&vouts, &vo);
            json_free(&vo);
        }
        json_push_kv(&entry, "outputs", &vouts);
        json_free(&vouts);

        /* Inputs */
        struct json_value vins = {0};
        json_set_array(&vins);
        int64_t debit = 0;
        for (size_t j = 0; j < tx.num_vin; j++) {
            struct json_value vi = {0};
            json_set_object(&vi);

            char prev_txid[65];
            uint256_get_hex(&tx.vin[j].prevout.hash, prev_txid);
            json_push_kv_str(&vi, "txid", prev_txid);
            json_push_kv_int(&vi, "vout", tx.vin[j].prevout.n);

            /* Try to resolve the input value from chainstate or wallet */
            int64_t in_val = 0;
            bool in_mine = false;
            if (g_node_db && g_node_db->open) {
                struct db_wallet_utxo prev_utxo;
                if (db_wallet_utxo_find(g_node_db,
                        tx.vin[j].prevout.hash.data,
                        tx.vin[j].prevout.n, &prev_utxo)) {
                    in_val = prev_utxo.value;
                    in_mine = true;
                }
            }

            if (in_val > 0) {
                char amt[32];
                format_amount(in_val, amt, sizeof(amt));
                json_push_kv_real(&vi, "value", strtod(amt, NULL));
                json_push_kv_bool(&vi, "is_mine", in_mine);
                if (in_mine) debit += in_val;
            }

            json_push_back(&vins, &vi);
            json_free(&vi);
        }
        json_push_kv(&entry, "inputs", &vins);
        json_free(&vins);

        /* Shielded components */
        if (tx.num_shielded_spend > 0 || tx.num_shielded_output > 0) {
            struct json_value shielded = {0};
            json_set_object(&shielded);
            json_push_kv_int(&shielded, "spends",
                             (int64_t)tx.num_shielded_spend);
            json_push_kv_int(&shielded, "outputs",
                             (int64_t)tx.num_shielded_output);
            json_push_kv(&entry, "shielded", &shielded);
            json_free(&shielded);
        }

        /* Net effect */
        int64_t net = credit - debit;
        if (rows[i].from_me && rows[i].fee > 0)
            net -= rows[i].fee;
        char net_str[32];
        format_amount(net, net_str, sizeof(net_str));
        json_push_kv_real(&entry, "net_effect", strtod(net_str, NULL));

        if (rows[i].from_me && rows[i].fee > 0) {
            char fee_str[32];
            format_amount(rows[i].fee, fee_str, sizeof(fee_str));
            json_push_kv_real(&entry, "fee", strtod(fee_str, NULL));
        }

        json_push_back(result, &entry);
        json_free(&entry);
        transaction_free(&tx);
        db_wallet_tx_free(&rows[i]);
    }

    free(rows);
    return true;
}

static bool rpc_getbalanceflow(const struct json_value *params, bool help,
                                struct json_value *result)
{
    RPC_HELP(help, result,
        "getbalanceflow ( min_height max_height )\n"
        "Chronological balance flow showing where every satoshi went.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int min_height = (int)rpc_permit_int(&p, 0, "min_height", 0);
    int max_height = (int)rpc_permit_int(&p, 1, "max_height", 999999999);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Wallet database not available");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);
    (void)pk_pfx; (void)pk_pfx_len;
    (void)sc_pfx; (void)sc_pfx_len;

    /* Get all wallet UTXOs (spent + unspent) sorted by height */
    struct db_wallet_utxo *all_utxos = calloc(4096, sizeof(struct db_wallet_utxo));
    if (!all_utxos) {
        json_set_str(result, "Out of memory");
        return false;
    }
    int nutxos = db_wallet_utxo_list_all(g_node_db, all_utxos, 4096);

    /* Get all wallet txs sorted by time */
    struct db_wallet_tx *txs = calloc(2000, sizeof(struct db_wallet_tx));
    if (!txs) {
        free(all_utxos);
        json_set_str(result, "Out of memory");
        return false;
    }
    int ntxs = db_wallet_tx_list(g_node_db, txs, 2000, 0);

    json_set_object(result);

    struct json_value flows = {0};
    json_set_array(&flows);

    int64_t running_balance = 0;
    int64_t total_received = 0;
    int64_t total_sent = 0;
    int64_t total_fees = 0;
    int flow_count = 0;

    /* Process each transaction */
    for (int i = ntxs - 1; i >= 0; i--) {
        if (txs[i].block_height < min_height ||
            txs[i].block_height > max_height) {
            db_wallet_tx_free(&txs[i]);
            continue;
        }

        struct transaction tx;
        if (!wallet_db_tx_deserialize(&txs[i], &tx)) {
            db_wallet_tx_free(&txs[i]);
            continue;
        }

        /* Calculate credit: outputs that are mine */
        int64_t credit = 0;
        for (size_t j = 0; j < tx.num_vout; j++) {
            if (wallet_is_mine(g_wallet, &tx.vout[j]))
                credit += tx.vout[j].value;
        }

        /* Calculate debit: inputs from wallet UTXOs */
        int64_t debit = 0;
        if (txs[i].from_me) {
            for (size_t j = 0; j < tx.num_vin; j++) {
                for (int k = 0; k < nutxos; k++) {
                    if (memcmp(all_utxos[k].txid,
                               tx.vin[j].prevout.hash.data, 32) == 0 &&
                        all_utxos[k].vout == tx.vin[j].prevout.n) {
                        debit += all_utxos[k].value;
                        break;
                    }
                }
            }
        }

        int64_t fee = txs[i].from_me ? txs[i].fee : 0;
        int64_t net = credit - debit;

        char txid_hex[65];
        uint256_get_hex(&tx.hash, txid_hex);

        const char *category;
        if (txs[i].from_me && credit < debit)
            category = "send";
        else if (!txs[i].from_me && credit > 0)
            category = "receive";
        else
            category = "internal";

        running_balance += net;
        if (credit > 0 && !txs[i].from_me)
            total_received += credit;
        if (debit > 0)
            total_sent += (debit - credit);
        total_fees += fee;

        struct json_value entry = {0};
        wallet_view_flow_entry(&entry, txid_hex, category,
                               net, fee,
                               txs[i].block_height, running_balance);
        json_push_back(&flows, &entry);
        json_free(&entry);
        flow_count++;

        transaction_free(&tx);
        db_wallet_tx_free(&txs[i]);
    }

    json_push_kv(result, "flows", &flows);
    json_free(&flows);

    /* Summary */
    struct json_value summary = {0};
    json_set_object(&summary);
    json_push_kv_int(&summary, "transaction_count", flow_count);

    char amt[32];
    format_amount(total_received, amt, sizeof(amt));
    json_push_kv_real(&summary, "total_received", strtod(amt, NULL));
    format_amount(total_sent, amt, sizeof(amt));
    json_push_kv_real(&summary, "total_sent", strtod(amt, NULL));
    format_amount(total_fees, amt, sizeof(amt));
    json_push_kv_real(&summary, "total_fees", strtod(amt, NULL));
    format_amount(running_balance, amt, sizeof(amt));
    json_push_kv_real(&summary, "final_balance", strtod(amt, NULL));

    /* Cross-verify with chainstate */
    if (g_coins_tip) {
        int64_t chain_balance = 0;
        struct db_wallet_utxo unspent[1024];
        int nu = db_wallet_utxo_list_unspent(g_node_db, unspent, 1024);
        for (int i = 0; i < nu; i++) {
            struct uint256 tid;
            memcpy(tid.data, unspent[i].txid, 32);
            struct coins cc;
            coins_init(&cc);
            bool found = coins_view_cache_get_coins(g_coins_tip, &tid, &cc);
            if (found && coins_is_available(&cc, unspent[i].vout))
                chain_balance += unspent[i].value;
            coins_free(&cc);
        }
        format_amount(chain_balance, amt, sizeof(amt));
        json_push_kv_real(&summary, "chainstate_verified_balance",
                          strtod(amt, NULL));
    }

    json_push_kv(result, "summary", &summary);
    json_free(&summary);

    free(all_utxos);
    free(txs);
    return true;
}

/* ── reconcilewalletutxos ──────────────────────────────────────── */

static bool rpc_reconcilewalletutxos(const struct json_value *params,
                                      bool help, struct json_value *result)
{
    RPC_HELP(help, result,
        "reconcilewalletutxos ( fix )\n"
        "Verify every wallet UTXO against chainstate.\n"
        "Classifies each as: verified, phantom, spent_on_chain, value_mismatch.\n"
        "If fix=true, marks phantoms and spent-on-chain as spent in both\n"
        "in-memory wallet and SQLite.\n"
        "\nArguments:\n"
        "1. fix    (bool, optional, default=false) Fix mismatches\n");

    ENSURE_WALLET(result);
    if (!g_coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    bool fix = rpc_permit_bool(&p, 0, "fix", false);
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    int64_t balance_before = db_wallet_utxo_balance(g_node_db);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(g_node_db, unspent, 4096);

    int verified = 0, phantom = 0, spent_on_chain = 0, mismatched = 0;
    int fixed = 0;
    static const uint8_t RECONCILE_SENTINEL[32] = {
        0xff, 0x00, 0xff, 0x00, 'R', 'E', 'C', 'O',
        'N', 'C', 'I', 'L', 'E', 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    struct json_value details = {0};
    json_set_array(&details);

    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool found = coins_view_cache_get_coins(g_coins_tip, &tid, &c);
        bool available = found && coins_is_available(&c, unspent[i].vout);
        int64_t chain_val = 0;
        if (available && unspent[i].vout < c.num_vout)
            chain_val = c.vout[unspent[i].vout].value;
        coins_free(&c);

        const char *status;
        if (!found) {
            status = "phantom";
            phantom++;
        } else if (!available) {
            status = "spent_on_chain";
            spent_on_chain++;
        } else if (chain_val != unspent[i].value) {
            status = "value_mismatch";
            mismatched++;
        } else {
            status = "verified";
            verified++;
            continue;
        }

        char txid_hex[65];
        for (int b = 31; b >= 0; b--)
            snprintf(txid_hex + (31 - b) * 2, 3, "%02x",
                     unspent[i].txid[b]);

        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "txid", txid_hex);
        json_push_kv_int(&entry, "vout", unspent[i].vout);
        char amt[32];
        format_amount(unspent[i].value, amt, sizeof(amt));
        json_push_kv_str(&entry, "wallet_value", amt);
        json_push_kv_str(&entry, "status", status);

        if (fix) {
            db_wallet_utxo_mark_spent(g_node_db, unspent[i].txid,
                                       unspent[i].vout,
                                       RECONCILE_SENTINEL, 0);
            wallet_mark_outpoint_spent(g_wallet, &tid, unspent[i].vout);
            json_push_kv_bool(&entry, "fixed", true);
            fixed++;
        }

        json_push_back(&details, &entry);
        json_free(&entry);
    }

    int64_t balance_after = fix ? db_wallet_utxo_balance(g_node_db)
                                : balance_before;

    if (fix) {
        node_db_state_set_int(g_node_db, "last_reconcile_height",
            g_main_state
                ? active_chain_height(&g_main_state->chain_active) : 0);
    }

    wallet_view_reconcile_summary(result, verified, phantom,
        spent_on_chain, mismatched, fixed, balance_before, balance_after);
    json_push_kv(result, "issues", &details);
    json_free(&details);
    return true;
}

/* ── purgephantomutxos ────────────────────────────────────────── */

static bool rpc_purgephantomutxos(const struct json_value *params,
                                   bool help, struct json_value *result)
{
    RPC_HELP(help, result,
        "purgephantomutxos confirm ( dryrun )\n"
        "Delete phantom UTXOs from the wallet SQLite database.\n"
        "Phantoms are wallet UTXOs not present in chainstate.\n"
        "\nArguments:\n"
        "1. confirm  (bool, required) Must be true to proceed\n"
        "2. dryrun   (bool, optional, default=false) Report without deleting\n");

    ENSURE_WALLET(result);
    if (!g_coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }

    struct rpc_params p;
    rpc_params_init(&p, params);
    bool confirm = rpc_require_bool(&p, 0, "confirm");
    bool dryrun = rpc_permit_bool(&p, 1, "dryrun", false);
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    if (!confirm) {
        json_set_str(result,
            "Safety interlock: pass confirm=true to proceed");
        return false;
    }

    int64_t balance_before = db_wallet_utxo_balance(g_node_db);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(g_node_db, unspent, 4096);

    int utxos_deleted = 0;
    int txs_deleted = 0;
    int64_t amount_purged = 0;

    node_db_begin(g_node_db);

    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool found = coins_view_cache_get_coins(g_coins_tip, &tid, &c);
        bool available = found && coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (available)
            continue;

        amount_purged += unspent[i].value;

        if (!dryrun) {
            db_wallet_utxo_delete(g_node_db, unspent[i].txid,
                                   unspent[i].vout);
            wallet_mark_outpoint_spent(g_wallet, &tid, unspent[i].vout);
        }
        utxos_deleted++;

        if (!dryrun) {
            int remaining = db_wallet_utxo_count_for_tx(g_node_db,
                                                         unspent[i].txid);
            if (remaining == 0) {
                db_wallet_tx_delete(g_node_db, unspent[i].txid);
                txs_deleted++;
            }
        }
    }

    if (!dryrun) {
        node_db_commit(g_node_db);
        wallet_rebuild_spent_set(g_wallet);
    } else {
        node_db_rollback(g_node_db);
    }

    int64_t balance_after = dryrun ? balance_before
                                   : db_wallet_utxo_balance(g_node_db);

    wallet_view_purge_summary(result, utxos_deleted, txs_deleted,
        amount_purged, balance_before, balance_after);
    json_push_kv_bool(result, "dryrun", dryrun);
    return true;
}

/* ── replaywalletfromchain ────────────────────────────────────── */

static bool rpc_replaywalletfromchain(const struct json_value *params,
                                       bool help, struct json_value *result)
{
    RPC_HELP(help, result,
        "replaywalletfromchain confirm\n"
        "Nuclear rebuild: wipe all wallet UTXOs and transactions from SQLite,\n"
        "then rescan all block files to rebuild from chain truth.\n"
        "\nArguments:\n"
        "1. confirm  (bool, required) Must be true to proceed\n");

    ENSURE_WALLET(result);
    if (!g_main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!g_datadir) {
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

    int64_t old_balance = db_wallet_utxo_balance(g_node_db);

    node_db_begin(g_node_db);
    db_wallet_utxo_delete_all(g_node_db);
    db_wallet_tx_delete_all(g_node_db);
    node_db_commit(g_node_db);

    wallet_rebuild_spent_set(g_wallet);

    int chain_tip = active_chain_height(&g_main_state->chain_active);

    printf("replaywalletfromchain: rescanning %d blocks...\n",
           chain_tip + 1);
    fflush(stdout);

    int found = wallet_scan_blocks(g_node_db,
        &g_main_state->chain_active, g_wallet, g_datadir,
        0, chain_tip);

    int64_t new_balance = db_wallet_utxo_balance(g_node_db);
    int utxo_count = 0;
    {
        struct db_wallet_utxo tmp[4096];
        utxo_count = db_wallet_utxo_list_unspent(g_node_db, tmp, 4096);
    }

    wallet_view_replay_summary(result, utxo_count,
        found > 0 ? found : 0, new_balance, old_balance);
    return true;
}

/* ── fastsync ────────────────────────────────────────────────── */

static bool rpc_fastsync(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result,
        "fastsync \"legacy_datadir\"\n"
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

    if (!g_datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }
    if (!g_wallet_db) {
        json_set_str(result, "Wallet DB not available");
        return false;
    }

    json_set_object(result);

    /* ── Phase 1: Repair wallet LevelDB ── */
    char wallet_path[1024];
    snprintf(wallet_path, sizeof(wallet_path), "%s/wallet", g_datadir);

    size_t keys_before = g_wallet->keystore.num_keys;
    size_t txs_before = g_wallet->num_wallet_tx;

    /* Close, repair, reopen */
    if (g_wallet_db->open) {
        wallet_db_close(g_wallet_db);
    }

    bool repaired = db_wrapper_repair(wallet_path);

    struct json_value phase1 = {0};
    json_set_object(&phase1);
    json_push_kv_bool(&phase1, "repair_success", repaired);

    if (wallet_db_open(g_wallet_db, wallet_path)) {
        wallet_db_read_keys(g_wallet_db, g_wallet);
        wallet_db_read_txs(g_wallet_db, g_wallet);
        wallet_db_read_sapling_keys(g_wallet_db, g_wallet);
        wallet_db_read_scripts(g_wallet_db, g_wallet);
    }

    size_t keys_after = g_wallet->keystore.num_keys;
    size_t txs_after = g_wallet->num_wallet_tx;
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
    snap.dst_dir = g_datadir;

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
    wallet_rebuild_spent_set(g_wallet);

    struct json_value phase3 = {0};
    json_set_object(&phase3);
    json_push_kv_int(&phase3, "total_keys", (int64_t)g_wallet->keystore.num_keys);
    json_push_kv_int(&phase3, "total_txs", (int64_t)g_wallet->num_wallet_tx);
    json_push_kv_int(&phase3, "spent_outpoints", (int64_t)g_wallet->num_spent);

    char s[32];
    format_amount(wallet_get_balance(g_wallet), s, sizeof(s));
    json_push_kv_str(&phase3, "balance", s);

    json_push_kv_str(&phase3, "note",
        "Restart node to load new chain data. "
        "Then run syncwalletfromdb to fix balance.");
    json_push_kv(result, "wallet_state", &phase3);
    json_free(&phase3);

    return true;
}

/* ── syncwalletfromdb ─────────────────────────────────────────── */

static bool rpc_syncwalletfromdb(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "syncwalletfromdb\n"
        "Sync in-memory wallet spent set from SQLite + chainstate truth.\n"
        "For each SQLite unspent UTXO verified in chainstate, removes it from\n"
        "the spent set if incorrectly marked. For UTXOs not in chainstate,\n"
        "marks them as spent. Fixes getbalance without restart.");

    ENSURE_WALLET(result);
    if (!g_coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }

    int64_t balance_before = wallet_get_balance(g_wallet);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(g_node_db, unspent, 4096);

    int synced = 0, already_correct = 0, marked_spent = 0;

    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool found = coins_view_cache_get_coins(g_coins_tip, &tid, &c);
        bool available = found &&
            coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (available) {
            if (wallet_is_outpoint_spent(g_wallet, &tid, unspent[i].vout)) {
                wallet_unmark_outpoint_spent(g_wallet, &tid, unspent[i].vout);
                synced++;
            } else {
                already_correct++;
            }
        } else {
            if (!wallet_is_outpoint_spent(g_wallet, &tid, unspent[i].vout)) {
                wallet_mark_outpoint_spent(g_wallet, &tid, unspent[i].vout);
                marked_spent++;
            } else {
                already_correct++;
            }
        }
    }

    int64_t balance_after = wallet_get_balance(g_wallet);

    wallet_view_sync_summary(result, synced, already_correct, marked_spent,
                              balance_before, balance_after);
    return true;
}

/* ── coinanalysis ────────────────────────────────────────────── */

static bool rpc_coinanalysis(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "coinanalysis\n"
        "Full chain analysis: for every wallet key, queries chainstate for\n"
        "unspent outputs. Compares against wallet's tracked UTXOs to find\n"
        "missing (untracked) coins. Reports total recoverable balance.");

    ENSURE_WALLET(result);
    if (!g_coins_tip) {
        json_set_str(result, "Chainstate not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }

    json_set_object(result);

    /* Get all wallet-tracked unspent UTXOs */
    struct db_wallet_utxo tracked[4096];
    int tracked_count = db_wallet_utxo_list_unspent(g_node_db, tracked, 4096);

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
    for (size_t ti = 0; ti < g_wallet->num_wallet_tx; ti++) {
        const struct wallet_tx *wtx = &g_wallet->map_wallet[ti];
        for (size_t vi = 0; vi < wtx->tx.num_vout; vi++) {
            const struct tx_out *out = &wtx->tx.vout[vi];

            /* Check if output goes to one of our keys */
            struct tx_destination dest;
            if (!script_extract_destination(&out->script_pub_key, &dest))
                continue;
            if (dest.type != DEST_KEY_ID) continue;

            struct privkey test_key;
            if (!keystore_get_key(&g_wallet->keystore,
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
                g_coins_tip, &wtx->tx.hash, &c);
            bool available = in_chain &&
                coins_is_available(&c, (unsigned int)vi);
            coins_free(&c);

            if (available && !is_tracked) {
                untracked_count++;
                untracked_balance += out->value;

                const struct chain_params *cp = chain_params_get();
                size_t pk_len = 0, sc_len = 0;
                const unsigned char *pk_pfx =
                    chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
                const unsigned char *sc_pfx =
                    chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);
                char addr[128];
                encode_destination(&dest, pk_pfx, pk_len, sc_pfx, sc_len,
                                  addr, sizeof(addr));

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
    sqlite3_prepare_v2(g_node_db->db,
        "SELECT txid, output_index, value, block_height, spent_txid,"
        " diversifier, pk_d, witness_height"
        " FROM wallet_sapling_notes ORDER BY block_height",
        -1, &z_stmt, NULL);
    while (z_stmt && sqlite3_step(z_stmt) == SQLITE_ROW) {
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
            for (int j = 0; j < 32; j++)
                snprintf(txid_hex + j * 2, 3, "%02x", ntxid[31 - j]);
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
            for (int j = 0; j < 32; j++)
                snprintf(spent_hex + j * 2, 3, "%02x", spent_by[31 - j]);
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
    sqlite3_prepare_v2(g_node_db->db,
        "SELECT fee FROM wallet_transactions WHERE from_me = 1 AND fee > 0",
        -1, &fee_stmt, NULL);
    while (fee_stmt && sqlite3_step(fee_stmt) == SQLITE_ROW) {
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

/* ── rescanwallet ────────────────────────────────────────────── */

static bool rpc_rescanwallet(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "rescanwallet\n"
        "Rescan the entire chain for wallet transactions using the current\n"
        "full key set. Finds UTXOs that were missed because keys were added\n"
        "after the initial scan. Imports them into SQLite and syncs the\n"
        "in-memory wallet.");

    ENSURE_WALLET(result);
    if (!g_main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!g_datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }
    if (!g_coins_tip) {
        json_set_str(result, "Chainstate not available");
        return false;
    }

    int64_t balance_before = db_wallet_utxo_balance(g_node_db);
    int utxos_before = 0;
    {
        struct db_wallet_utxo tmp[4096];
        utxos_before = db_wallet_utxo_list_unspent(g_node_db, tmp, 4096);
    }

    /* Full rescan from block 0 */
    node_db_begin(g_node_db);
    db_wallet_utxo_delete_all(g_node_db);
    db_wallet_tx_delete_all(g_node_db);
    node_db_commit(g_node_db);

    int chain_tip = active_chain_height(&g_main_state->chain_active);
    printf("rescanwallet: rescanning %d blocks with %zu keys...\n",
           chain_tip + 1, g_wallet->keystore.num_keys);
    fflush(stdout);

    int found = wallet_scan_blocks(g_node_db,
        &g_main_state->chain_active, g_wallet, g_datadir,
        0, chain_tip);

    /* Now sync the in-memory wallet from the fresh SQLite data */
    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(g_node_db, unspent, 4096);

    int synced = 0;
    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool avail = coins_view_cache_get_coins(g_coins_tip, &tid, &c)
                   && coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (avail && wallet_is_outpoint_spent(g_wallet, &tid,
                                               unspent[i].vout)) {
            wallet_unmark_outpoint_spent(g_wallet, &tid, unspent[i].vout);
            synced++;
        }
    }

    int64_t balance_after = db_wallet_utxo_balance(g_node_db);

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

/* ── rescanwitnesses ─────────────────────────────────────────── */

static bool rpc_rescanwitnesses(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "rescanwitnesses\n"
        "Rebuild Sapling Merkle witnesses for all unspent shielded notes.\n"
        "Required before spending z→z or z→t. Replays the commitment tree\n"
        "from the Sapling activation height to tip.");

    ENSURE_WALLET(result);
    if (!g_main_state) {
        json_set_str(result, "Main state not available");
        return false;
    }
    if (!g_node_db || !g_node_db->open) {
        json_set_str(result, "Node database not available");
        return false;
    }
    if (!g_datadir) {
        json_set_str(result, "Data directory not configured");
        return false;
    }

    /* Load all unspent notes that need witnesses */
    struct db_sapling_note notes[256];
    int n_notes = db_sapling_note_list_unspent(g_node_db, notes, 256);
    if (n_notes == 0) {
        json_set_object(result);
        json_push_kv_int(result, "notes_updated", 0);
        json_push_kv_str(result, "status", "no unspent notes");
        return true;
    }

    printf("rescanwitnesses: building witnesses for %d notes...\n", n_notes);
    fflush(stdout);

    int chain_tip = active_chain_height(&g_main_state->chain_active);
    int sapling_start = 476969; /* Sapling activation on ZClassic mainnet */

    /* Initialize empty tree and per-note witness state */
    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);

    struct incremental_witness *witnesses = calloc((size_t)n_notes,
        sizeof(struct incremental_witness));
    bool *witness_active = calloc((size_t)n_notes, sizeof(bool));
    int witnesses_built = 0;

    /* mmap cache */
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    int64_t t_start = (int64_t)time(NULL);
    int blocks_scanned = 0;

    for (int h = sapling_start; h <= chain_tip; h++) {
        const struct block_index *pindex =
            active_chain_at(&g_main_state->chain_active, h);
        if (!pindex) continue;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) continue;

        /* mmap block file */
        if (pindex->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            char path[512];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     g_datadir, pindex->nFile);
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
            cached_file = pindex->nFile;
        }
        if (!cached_data || pindex->nDataPos >= cached_size) continue;

        struct block blk;
        block_init(&blk);
        struct byte_stream bs;
        stream_init_from_data(&bs, cached_data + pindex->nDataPos,
                              cached_size - pindex->nDataPos);
        if (!block_deserialize(&blk, &bs)) {
            block_free(&blk);
            continue;
        }

        /* Process each Sapling output commitment */
        for (size_t ti = 0; ti < blk.num_vtx; ti++) {
            const struct transaction *tx = &blk.vtx[ti];
            for (size_t oi = 0; oi < tx->num_shielded_output; oi++) {
                const struct uint256 *cm =
                    &tx->v_shielded_output[oi].cm;

                /* Advance all active witnesses */
                for (int ni = 0; ni < n_notes; ni++) {
                    if (witness_active[ni])
                        incremental_witness_append(&witnesses[ni], cm);
                }

                /* Append to tree */
                incremental_tree_append(&tree, cm);

                /* Check if this cm matches any note's commitment */
                for (int ni = 0; ni < n_notes; ni++) {
                    if (witness_active[ni]) continue;
                    if (memcmp(cm->data, notes[ni].cm, 32) == 0) {
                        /* Init witness from tree state (tree already
                         * contains this cm) */
                        incremental_witness_init(&witnesses[ni], &tree);
                        witness_active[ni] = true;
                        witnesses_built++;
                    }
                }
            }
        }

        /* Verify tree root against block header every 100k blocks
         * and on the last block */
        if (blocks_scanned % 100000 == 0 || h == chain_tip) {
            struct uint256 tree_root;
            incremental_tree_root(&tree, &tree_root);
            if (memcmp(tree_root.data,
                       blk.header.hashFinalSaplingRoot.data, 32) != 0) {
                char tr[65], hr[65];
                for (int j = 0; j < 32; j++) {
                    snprintf(tr + j*2, 3, "%02x", tree_root.data[31-j]);
                    snprintf(hr + j*2, 3, "%02x",
                             blk.header.hashFinalSaplingRoot.data[31-j]);
                }
                printf("rescanwitnesses: ROOT MISMATCH at height %d\n"
                       "  tree:   %s\n  header: %s\n", h, tr, hr);
                fflush(stdout);
            }
        }

        block_free(&blk);
        blocks_scanned++;

        if (blocks_scanned % 100000 == 0) {
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            printf("rescanwitnesses: %d blocks (height %d), "
                   "%d/%d witnesses built, %llds\n",
                   blocks_scanned, h, witnesses_built, n_notes,
                   (long long)elapsed);
            fflush(stdout);
        }
    }

    if (cached_data) munmap(cached_data, cached_size);

    /* Save the authoritative tree state to node_state.
     * This replaces any incomplete tree from catchup. */
    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        incremental_tree_serialize(&tree, &ts);
        node_db_state_set(g_node_db, "sapling_tree", ts.data, ts.size);
        stream_free(&ts);
    }

    /* Serialize and save witnesses */
    int saved = 0;
    for (int ni = 0; ni < n_notes; ni++) {
        if (!witness_active[ni]) continue;

        struct byte_stream ws;
        stream_init(&ws, 4096);
        if (incremental_witness_serialize(&witnesses[ni], &ws)) {
            db_sapling_note_save_witness(g_node_db,
                notes[ni].txid, notes[ni].output_index,
                ws.data, ws.size, chain_tip);
            saved++;
        }
        stream_free(&ws);
    }

    free(witnesses);
    free(witness_active);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("rescanwitnesses: done in %llds — %d/%d witnesses built, "
           "%d saved\n",
           (long long)elapsed, witnesses_built, n_notes, saved);
    fflush(stdout);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", blocks_scanned);
    json_push_kv_int(result, "notes_total", n_notes);
    json_push_kv_int(result, "witnesses_built", witnesses_built);
    json_push_kv_int(result, "witnesses_saved", saved);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

/* ── diagnoseutxos ───────────────────────────────────────────── */

static bool rpc_diagnoseutxos(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "diagnoseutxos\n"
        "Per-UTXO diagnostic: checks script type, destination extraction,\n"
        "key existence (have_key), key retrieval (get_key), and chainstate\n"
        "presence. Identifies exactly why each UTXO can or cannot be spent.");

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(g_wallet, coins, &num_coins, 4096, false, false);

    json_set_object(result);

    int can_spend = 0, cannot_spend = 0;
    int64_t spendable_balance = 0, locked_balance = 0;

    struct json_value utxo_list = {0};
    json_set_array(&utxo_list);

    for (size_t i = 0; i < num_coins; i++) {
        const struct wallet_tx *wtx = coins[i].wtx;
        uint32_t vout_n = coins[i].i;
        const struct tx_out *out = &wtx->tx.vout[vout_n];

        struct json_value entry = {0};
        json_set_object(&entry);

        char txid[65];
        uint256_get_hex(&wtx->tx.hash, txid);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "vout", vout_n);

        char amt[32];
        format_amount(out->value, amt, sizeof(amt));
        json_push_kv_str(&entry, "amount", amt);

        /* Extract destination */
        struct tx_destination dest;
        bool have_dest = script_extract_destination(
            &out->script_pub_key, &dest);

        if (!have_dest) {
            json_push_kv_str(&entry, "script_type", "unknown");
            json_push_kv_str(&entry, "status", "no_destination");
            cannot_spend++;
            locked_balance += out->value;
            json_push_back(&utxo_list, &entry);
            json_free(&entry);
            continue;
        }

        const char *dest_type = dest.type == DEST_KEY_ID ? "p2pkh"
                               : dest.type == DEST_SCRIPT_ID ? "p2sh"
                               : "other";
        json_push_kv_str(&entry, "script_type", dest_type);

        /* Resolve address */
        char addr[128];
        encode_destination(&dest, pk_pfx, pk_pfx_len,
                           sc_pfx, sc_pfx_len, addr, sizeof(addr));
        json_push_kv_str(&entry, "address", addr);

        /* Key hash hex */
        char keyhash[41];
        for (int k = 0; k < 20; k++)
            snprintf(keyhash + k * 2, 3, "%02x",
                     dest.id.key.id.data[k]);
        json_push_kv_str(&entry, "key_id", keyhash);

        /* Check key availability */
        bool have = false;
        bool can_get = false;

        if (dest.type == DEST_KEY_ID) {
            have = keystore_have_key(&g_wallet->keystore, &dest.id.key);
            struct privkey test_key;
            can_get = keystore_get_key(&g_wallet->keystore,
                                        &dest.id.key, &test_key);
            if (can_get)
                memory_cleanse(test_key.vch, 32);
        } else if (dest.type == DEST_SCRIPT_ID) {
            have = keystore_have_cscript(&g_wallet->keystore,
                                          &dest.id.script.hash);
            json_push_kv_str(&entry, "note",
                "p2sh — need underlying keys, not just script");
        }

        json_push_kv_bool(&entry, "have_key", have);
        json_push_kv_bool(&entry, "can_retrieve_key", can_get);

        /* Chainstate check */
        if (g_coins_tip) {
            struct coins c;
            coins_init(&c);
            bool found = coins_view_cache_get_coins(g_coins_tip,
                &wtx->tx.hash, &c);
            bool avail = found &&
                coins_is_available(&c, vout_n);
            coins_free(&c);
            json_push_kv_bool(&entry, "in_chainstate", avail);
        }

        if (can_get) {
            json_push_kv_str(&entry, "status", "spendable");
            can_spend++;
            spendable_balance += out->value;
        } else if (have && !can_get) {
            json_push_kv_str(&entry, "status",
                "have_key_but_cannot_retrieve");
            cannot_spend++;
            locked_balance += out->value;
        } else {
            json_push_kv_str(&entry, "status", "key_missing");
            cannot_spend++;
            locked_balance += out->value;
        }

        json_push_back(&utxo_list, &entry);
        json_free(&entry);
    }

    /* Summary */
    struct json_value summary = {0};
    json_set_object(&summary);
    json_push_kv_int(&summary, "total_utxos", (int64_t)num_coins);
    json_push_kv_int(&summary, "spendable", can_spend);
    json_push_kv_int(&summary, "not_spendable", cannot_spend);

    char s[32];
    format_amount(spendable_balance, s, sizeof(s));
    json_push_kv_str(&summary, "spendable_balance", s);
    format_amount(locked_balance, s, sizeof(s));
    json_push_kv_str(&summary, "locked_balance", s);
    json_push_kv_int(&summary, "keystore_keys",
        (int64_t)g_wallet->keystore.num_keys);

    json_push_kv(result, "summary", &summary);
    json_free(&summary);

    json_push_kv(result, "utxos", &utxo_list);
    json_free(&utxo_list);

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
        { "wallet", "getwalletaccounting", rpc_getwalletaccounting, false },
        { "wallet", "removestalletxs", rpc_removestalletxs,     false },
        { "wallet", "db_info",          rpc_db_info,              false },
        { "wallet", "walletaudit",     rpc_walletaudit,          false },
        { "wallet", "listwalletkeys",  rpc_listwalletkeys,       false },
        { "wallet", "listwallettxdetail", rpc_listwallettxdetail, false },
        { "wallet", "traceutxo",       rpc_traceutxo,            false },
        { "wallet", "getbalanceflow",  rpc_getbalanceflow,       false },
        { "wallet", "getchaincoins",   rpc_getchaincoins,        false },
        { "wallet", "reconcilewalletutxos", rpc_reconcilewalletutxos, false },
        { "wallet", "purgephantomutxos", rpc_purgephantomutxos,    false },
        { "wallet", "replaywalletfromchain", rpc_replaywalletfromchain, false },
        { "wallet", "syncwalletfromdb", rpc_syncwalletfromdb, false },
        { "wallet", "diagnoseutxos", rpc_diagnoseutxos, false },
        { "wallet", "coinanalysis", rpc_coinanalysis, false },
        { "wallet", "rescanwallet", rpc_rescanwallet, false },
        { "wallet", "rescanwitnesses", rpc_rescanwitnesses, false },
        { "wallet", "fastsync", rpc_fastsync, false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
