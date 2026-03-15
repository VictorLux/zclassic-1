/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_helpers.h"
#include "wallet/wallet.h"
#include "chain/chainparams.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "validation/main_state.h"
#include "models/database.h"
#include "models/wallet_tx.h"
#include "core/serialize.h"
#include <stdio.h>
#include <string.h>

/* Shared wallet state */
struct wallet *g_wallet = NULL;
struct main_state *g_main_state = NULL;
const char *g_datadir = NULL;
struct wallet_db *g_wallet_db = NULL;
struct tx_mempool *g_mempool = NULL;
struct connman *g_connman_ptr = NULL;
struct node_db *g_node_db = NULL;
struct coins_view_cache *g_coins_tip = NULL;

void format_amount(int64_t satoshis, char *out, size_t out_size)
{
    bool neg = satoshis < 0;
    int64_t abs_val = neg ? -satoshis : satoshis;
    int64_t whole = abs_val / 100000000;
    int64_t frac = abs_val % 100000000;
    snprintf(out, out_size, "%s%lld.%08lld",
             neg ? "-" : "",
             (long long)whole, (long long)frac);
}

int64_t parse_amount(const struct json_value *v)
{
    if (!v) return -1;

    if (v->type == JSON_INT) {
        int64_t val = json_get_int(v);
        return val * 100000000;
    }

    const char *str = NULL;
    char tmp[64];
    if (v->type == JSON_STR) {
        str = json_get_str(v);
    } else if (v->type == JSON_REAL) {
        snprintf(tmp, sizeof(tmp), "%.8f", json_get_real(v));
        str = tmp;
    }
    if (!str) return -1;

    const char *p = str;
    while (*p == ' ') p++;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }

    int64_t whole = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        p++;
    }

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
    while (frac_digits < 8) {
        frac *= 10;
        frac_digits++;
    }

    int64_t satoshis = whole * 100000000 + frac;
    return neg ? -satoshis : satoshis;
}

int wallet_history_count(void)
{
    int mem_count = g_wallet ? (int)g_wallet->num_wallet_tx : 0;
    int db_count = (g_node_db && g_node_db->open) ? db_wallet_tx_count(g_node_db) : 0;
    return db_count > mem_count ? db_count : mem_count;
}

bool wallet_history_db_ready(void)
{
    if (!g_wallet || !g_node_db || !g_node_db->open)
        return false;

    int db_count = db_wallet_tx_count(g_node_db);
    if (db_count >= (int)g_wallet->num_wallet_tx)
        return true;

    return g_wallet->num_wallet_tx >= MAX_WALLET_TX;
}

bool wallet_db_tx_deserialize(const struct db_wallet_tx *dbtx,
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

int wallet_db_tx_confirmations(const struct db_wallet_tx *dbtx)
{
    if (!dbtx || !dbtx->has_block || !g_main_state)
        return 0;

    int tip_height = active_chain_height(&g_main_state->chain_active);
    if (tip_height < dbtx->block_height)
        return 0;

    return tip_height - dbtx->block_height + 1;
}

void append_one_entry(struct json_value *result,
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

bool wallet_append_tx_entry(const struct transaction *tx,
                            bool from_me, int64_t fee,
                            int confirmations, int64_t time_received,
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
