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
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "wallet/wallet_db.h"
#include "net/connman.h"
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

static struct wallet *g_wallet = NULL;
static struct main_state *g_main_state = NULL;
static const char *g_datadir = NULL;
static struct wallet_db *g_wallet_db = NULL;
static struct tx_mempool *g_mempool = NULL;
static struct connman *g_connman_ptr = NULL;

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

    int64_t balance = wallet_get_balance(g_wallet);
    json_set_real(result, (double)balance / 100000000.0);
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

    int64_t balance = wallet_get_unconfirmed_balance(g_wallet);
    json_set_real(result, (double)balance / 100000000.0);
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

    json_set_object(result);
    json_push_kv_real(result, "balance",
                      (double)wallet_get_balance(g_wallet) / 100000000.0);
    json_push_kv_real(result, "unconfirmed_balance",
                      (double)wallet_get_unconfirmed_balance(g_wallet) / 100000000.0);
    json_push_kv_real(result, "immature_balance",
                      (double)wallet_get_immature_balance(g_wallet) / 100000000.0);
    json_push_kv_int(result, "txcount", (int64_t)g_wallet->num_wallet_tx);
    json_push_kv_int(result, "keypoolsize", (int64_t)g_wallet->key_pool_size);
    json_push_kv_real(result, "paytxfee",
                      (double)g_wallet->default_fee / 100000000.0);
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

        json_push_kv_real(&entry, "amount",
                          (double)out->value / 100000000.0);
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

    const struct json_value *addr_val = json_at(params, 0);
    const struct json_value *amt_val = json_at(params, 1);
    if (!addr_val || !amt_val) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    const char *addr_str = json_get_str(addr_val);
    double amt_dbl = json_get_real(amt_val);
    int64_t amount = (int64_t)(amt_dbl * 100000000.0 + 0.5);

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

    int count = 10;
    int skip = 0;
    if (json_size(params) >= 1)
        count = (int)json_get_int(json_at(params, 0));
    if (json_size(params) >= 2)
        skip = (int)json_get_int(json_at(params, 1));
    (void)skip;

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    json_set_array(result);
    int added = 0;

    for (size_t i = 0; i < MAX_WALLET_TX && added < count; i++) {
        if (!g_wallet->map_wallet[i].used)
            continue;
        const struct wallet_tx *wtx = &g_wallet->map_wallet[i];

        struct json_value entry;
        json_set_object(&entry);

        char txid[65];
        uint256_get_hex(&wtx->tx.hash, txid);
        json_push_kv_str(&entry, "txid", txid);
        json_push_kv_int(&entry, "confirmations", (int64_t)wtx->confirms);
        json_push_kv_int(&entry, "time", wtx->time_received);
        json_push_kv_int(&entry, "timereceived", wtx->time_received);

        if (wtx->from_me) {
            json_push_kv_str(&entry, "category", "send");
            int64_t value = transaction_get_value_out(&wtx->tx);
            json_push_kv_real(&entry, "amount",
                              -(double)value / 100000000.0);
        } else {
            int64_t credit = 0;
            for (size_t j = 0; j < wtx->tx.num_vout; j++) {
                if (wallet_is_mine(g_wallet, &wtx->tx.vout[j]))
                    credit += wtx->tx.vout[j].value;
            }
            json_push_kv_str(&entry, "category",
                             wtx->confirms > 0 ? "receive" : "immature");
            json_push_kv_real(&entry, "amount",
                              (double)credit / 100000000.0);

            if (wtx->tx.num_vout > 0) {
                struct tx_destination dest;
                if (script_extract_destination(
                        &wtx->tx.vout[0].script_pub_key, &dest)) {
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

    const char *txid_str = json_get_str(json_at(params, 0));
    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

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
    json_push_kv_real(result, "amount", (double)net / 100000000.0);
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

        double amt = json_get_real(val);
        if (val->type == JSON_INT)
            amt = (double)json_get_int(val);
        values[n] = (int64_t)(amt * 100000000.0 + 0.5);
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
        { "wallet", "sendmany",             rpc_sendmany,             false },
        { "wallet", "createmultisig",      rpc_createmultisig,       false },
        { "wallet", "z_getnewaddress",     rpc_z_getnewaddress,      false },
        { "wallet", "z_listaddresses",     rpc_z_listaddresses,      false },
        { "wallet", "addmultisigaddress", rpc_addmultisigaddress,   false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
