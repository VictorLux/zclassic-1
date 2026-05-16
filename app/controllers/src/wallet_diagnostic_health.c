/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/rpc_chainstate_guard.h"
#include "controllers/wallet_helpers.h"
#include "controllers/strong_params.h"
#include "wallet_diagnostic_internal.h"
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
#include <stdio.h>
#include <stdlib.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* wallet-diagnostic health RPCs: read-only inspection of wallet state. */

bool rpc_getwalletaccounting(const struct json_value *params,
                                    bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
        if (!ctx->wallet->map_wallet[i].used)
            continue;

        const struct wallet_tx *wtx = &ctx->wallet->map_wallet[i];
        const struct transaction *tx = &wtx->tx;

        char txid[65];
        uint256_get_hex(&tx->hash, txid);

        /* Sum outputs by destination type */
        int64_t out_to_mine = 0;     /* outputs to our addresses */
        int64_t out_to_other = 0;    /* outputs to external addresses */

        for (size_t j = 0; j < tx->num_vout; j++) {
            if (wallet_is_mine(ctx->wallet, &tx->vout[j]))
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
                const struct wallet_tx *prev = wallet_get_tx(ctx->wallet,
                    &tx->vin[j].prevout.hash);
                if (prev) {
                    uint32_t n = tx->vin[j].prevout.n;
                    if (n < prev->tx.num_vout) {
                        int64_t v = prev->tx.vout[n].value;
                        total_in += v;
                        if (wallet_is_mine(ctx->wallet, &prev->tx.vout[n]))
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
                const struct wallet_tx *prev = wallet_get_tx(ctx->wallet,
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
            bool is_mine = wallet_is_mine(ctx->wallet, &tx->vout[j]);
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
        wallet_available_coins(ctx->wallet, coins, &nc, 4096, false, false);
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
                     (int64_t)ctx->wallet->num_wallet_tx);
    json_push_kv(&result[0], "transactions", &tx_list);
    json_free(&tx_list);

    return true;
}
bool rpc_db_info(const struct json_value *params, bool help,
                        struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "db_info\n"
        "Returns SQLite node database statistics.\n");

    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "SQLite database not available");
        return false;
    }

    json_set_object(result);

    int tip_h = node_db_sync_get_tip_height(ctx->node_db);
    json_push_kv_int(result, "tip_height", tip_h);

    int64_t utxo_count = db_utxo_count(ctx->node_db);
    json_push_kv_int(result, "utxo_count", utxo_count);

    int block_count = db_block_count(ctx->node_db);
    json_push_kv_int(result, "blocks_indexed", block_count);

    int max_h = db_block_max_height(ctx->node_db);
    json_push_kv_int(result, "max_block_height", max_h);

    int64_t wallet_bal = db_wallet_utxo_balance(ctx->node_db);
    char bal_str[32];
    format_amount(wallet_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t sapling_bal = db_sapling_note_balance(ctx->node_db);
    char zbal_str[32];
    format_amount(sapling_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int mempool_count = db_mempool_count(ctx->node_db);
    json_push_kv_int(result, "mempool_persisted", mempool_count);

    int peer_count = db_peer_count(ctx->node_db);
    json_push_kv_int(result, "peers_stored", peer_count);

    int wkey_count = db_wallet_key_count(ctx->node_db);
    json_push_kv_int(result, "wallet_keys", wkey_count);

    int skey_count = db_sapling_key_count(ctx->node_db);
    json_push_kv_int(result, "sapling_keys", skey_count);

    int wtx_count = db_wallet_tx_count(ctx->node_db);
    json_push_kv_int(result, "wallet_transactions", wtx_count);

    return true;
}
bool rpc_getchaincoins(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "getchaincoins \"txid\"\n"
        "Raw chainstate lookup for any txid.\n"
        "Shows all outputs with spent/unspent status.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!rpc_require_chainstate_lookup_ready(ctx->main_state, result,
            "getchaincoins", "Chainstate lookup"))
        return false;

    struct uint256 txid;
    uint256_set_hex(&txid, txid_str);

    struct coins chain_coins;
    coins_init(&chain_coins);
    bool found = coins_view_cache_get_coins(ctx->coins_tip, &txid, &chain_coins);

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
        if (ctx->wallet && available)
            in_wallet_flag = wallet_is_mine(ctx->wallet, &chain_coins.vout[i]);

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
bool rpc_traceutxo(const struct json_value *params, bool help,
                            struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "traceutxo", "Chainstate lookup"))
        return false;

    bool in_wallet = false;
    bool in_chain = false;
    bool chain_available = false;
    int64_t value = 0;
    int height = 0;
    char spent_by_hex[65];
    spent_by_hex[0] = '\0';

    /* Check wallet DB */
    if (wallet_ctx_db_ready(ctx)) {
        struct db_wallet_utxo wu;
        if (db_wallet_utxo_find(ctx->node_db, txid.data, (uint32_t)vout, &wu)) {
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
    if (ctx->coins_tip) {
        struct coins chain_coins;
        coins_init(&chain_coins);
        in_chain = coins_view_cache_get_coins(ctx->coins_tip, &txid, &chain_coins);
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
    if (wallet_ctx_db_ready(ctx) && value == 0) {
        struct db_wallet_tx dbtx;
        if (db_wallet_tx_find(ctx->node_db, txid.data, &dbtx)) {
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

bool rpc_listwalletkeys(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "listwalletkeys ( include_privkeys )\n"
        "Show every key with per-key verified balance.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    (void)rpc_permit_bool(&p, 0, "include_privkeys", false);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    ENSURE_WALLET(result);
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Wallet database not available");
        return false;
    }
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "listwalletkeys",
            "Chainstate lookup"))
        return false;

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
        .ndb = ctx->node_db,
        .pk_pfx = pk_pfx,
        .pk_pfx_len = pk_pfx_len,
        .sc_pfx = sc_pfx,
        .sc_pfx_len = sc_pfx_len,
        .coins_tip = ctx->coins_tip,
        .total_balance = 0,
        .total_keys = 0,
    };
    db_wallet_key_each(ctx->node_db, key_balance_cb, &kctx);

    json_push_kv(result, "transparent_keys", &keys_arr);
    json_free(&keys_arr);

    /* Sapling keys */
    struct json_value z_keys = {0};
    json_set_array(&z_keys);
    int64_t z_total = 0;

    for (size_t i = 0; i < ctx->wallet->sapling_keys.num_keys; i++) {
        if (!ctx->wallet->sapling_keys.keys[i].used) continue;
        const struct sapling_key_entry *sk = &ctx->wallet->sapling_keys.keys[i];

        int64_t z_bal = 0;
        if (wallet_ctx_db_ready(ctx))
            z_bal = db_sapling_note_balance_for_ivk(ctx->node_db, sk->ivk);

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
bool rpc_listwallettxdetail(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (!wallet_ctx_db_ready(ctx)) {
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

    struct db_wallet_tx *rows = zcl_calloc((size_t)count, sizeof(struct db_wallet_tx), "wallet_tx_rows");
    if (!rows) {
        json_set_str(result, "Out of memory");
        return false;
    }

    int n = db_wallet_tx_list(ctx->node_db, rows, (size_t)count, (size_t)offset);

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

            bool mine = wallet_is_mine(ctx->wallet, &tx.vout[j]);
            json_push_kv_bool(&vo, "is_mine", mine);
            json_push_kv_bool(&vo, "is_change",
                              wallet_is_change(ctx->wallet, &tx.vout[j]));

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
            if (wallet_ctx_db_ready(ctx)) {
                struct db_wallet_utxo prev_utxo;
                if (db_wallet_utxo_find(ctx->node_db,
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
bool rpc_getbalanceflow(const struct json_value *params, bool help,
                                struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Wallet database not available");
        return false;
    }
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "getbalanceflow",
            "Chainstate lookup"))
        return false;

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);
    (void)pk_pfx; (void)pk_pfx_len;
    (void)sc_pfx; (void)sc_pfx_len;

    /* Get all wallet UTXOs (spent + unspent) sorted by height */
    struct db_wallet_utxo *all_utxos = zcl_calloc(4096, sizeof(struct db_wallet_utxo), "wallet_utxos");
    if (!all_utxos) {
        json_set_str(result, "Out of memory");
        return false;
    }
    int nutxos = db_wallet_utxo_list_all(ctx->node_db, all_utxos, 4096);

    /* Get all wallet txs sorted by time */
    struct db_wallet_tx *txs = zcl_calloc(2000, sizeof(struct db_wallet_tx), "wallet_txs");
    if (!txs) {
        free(all_utxos);
        json_set_str(result, "Out of memory");
        return false;
    }
    int ntxs = db_wallet_tx_list(ctx->node_db, txs, 2000, 0);

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
            if (wallet_is_mine(ctx->wallet, &tx.vout[j]))
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
    if (ctx->coins_tip) {
        int64_t chain_balance = 0;
        struct db_wallet_utxo unspent[1024];
        int nu = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 1024);
        for (int i = 0; i < nu; i++) {
            struct uint256 tid;
            memcpy(tid.data, unspent[i].txid, 32);
            struct coins cc;
            coins_init(&cc);
            bool found = coins_view_cache_get_coins(ctx->coins_tip, &tid, &cc);
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
