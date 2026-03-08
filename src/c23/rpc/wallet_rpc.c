/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/wallet_rpc.h"
#include "wallet/wallet.h"
#include "chain/chainparams.h"
#include "encoding/utilmoneystr.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "validation/txmempool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wallet *g_wallet = NULL;

void rpc_wallet_set_state(struct wallet *w)
{
    g_wallet = w;
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

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);

    transaction_free(&wtx.tx);
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
            "importprivkey \"privkey\"\n"
            "Adds a private key to your wallet.");
        return true;
    }

    const char *wif = json_get_str(json_at(params, 0));

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

    memory_cleanse(key.vch, 32);
    json_set_null(result);
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
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
