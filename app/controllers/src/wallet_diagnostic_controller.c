/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_diagnostic_controller.h"
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

static bool wallet_ctx_db_ready(const struct wallet_rpc_context *ctx)
{
    return ctx->node_db && ctx->node_db->open;
}

static bool rpc_scanblockfiles(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "scanblockfiles\n"
        "\nScan all block files on disk for wallet transactions.\n"
        "Faster than rescanblockchain — reads raw block files sequentially.\n"
        "Updates the spent-outpoint index for accurate balances.\n");

    ENSURE_WALLET(result);

    const char *dir = ctx->datadir ? ctx->datadir : "/home/bob/.zclassic-c23";
    int found = wallet_scan_blockfiles(ctx->wallet, dir);

    /* Also persist wallet updates */
    if (ctx->wallet_db)
        wallet_sqlite_flush(ctx->wallet_db, ctx->wallet);

    json_set_object(result);
    json_push_kv_int(result, "wallet_outputs_found", found);
    json_push_kv_int(result, "spent_outpoints", (int64_t)ctx->wallet->num_spent);

    /* Report corrected balance */
    int64_t balance = wallet_get_balance(ctx->wallet);
    char tbal[32], zbal[32];
    format_amount(balance, tbal, sizeof(tbal));
    json_push_kv_real(result, "transparent_balance", strtod(tbal, NULL));

    int64_t z_balance = wallet_get_sapling_balance(ctx->wallet);
    format_amount(z_balance, zbal, sizeof(zbal));
    json_push_kv_real(result, "shielded_balance", strtod(zbal, NULL));

    return true;
}

static bool rpc_reindexdb(const struct json_value *params, bool help,
                          struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "reindexdb\n"
        "Wipes wallet_utxos, wallet_transactions, and wallet_sapling_notes,\n"
        "then re-scans all blocks from disk to rebuild them.\n"
        "Returns the corrected balance.\n");

    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "SQLite database not available");
        return false;
    }
    if (!ctx->wallet) {
        json_set_str(result, "Wallet not loaded");
        return false;
    }
    if (!ctx->main_state) {
        json_set_str(result, "Chain state not available");
        return false;
    }

    int chain_tip = active_chain_height(&ctx->main_state->chain_active);
    printf("reindexdb: fast wallet scan of %d blocks...\n", chain_tip + 1);
    fflush(stdout);

    int found = wallet_scan_blocks(ctx->node_db,
        &ctx->main_state->chain_active, ctx->wallet, ctx->datadir,
        0, chain_tip);

    node_db_sync_wallet_keys(ctx->node_db, ctx->wallet);

    json_set_object(result);
    json_push_kv_int(result, "blocks_scanned", chain_tip + 1);
    json_push_kv_int(result, "wallet_transactions", found);

    int64_t t_bal = db_wallet_utxo_balance(ctx->node_db);
    char bal_str[32];
    format_amount(t_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t z_bal = db_sapling_note_balance(ctx->node_db);
    char zbal_str[32];
    format_amount(z_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int64_t total = t_bal + z_bal;
    char tot_str[32];
    format_amount(total, tot_str, sizeof(tot_str));
    json_push_kv_str(result, "total_balance", tot_str);

    struct db_wallet_utxo utxos[256];
    int utxo_count = db_wallet_utxo_list_unspent(ctx->node_db, utxos, 256);
    json_push_kv_int(result, "unspent_utxos", utxo_count);

    printf("reindexdb: complete — balance %s ZCL (%d UTXOs)\n",
           tot_str, utxo_count);
    fflush(stdout);

    return true;
}

static bool rpc_importlegacy(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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

    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "SQLite database not available");
        return false;
    }
    if (!ctx->wallet) {
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

    int found = legacy_import(legacy_dir, ctx->node_db, ctx->wallet, true);
    if (found < 0) {
        json_set_str(result,
            "Import failed — is the legacy node stopped?");
        return false;
    }

    json_set_object(result);
    json_push_kv_int(result, "wallet_transactions", found);

    int64_t t_bal = db_wallet_utxo_balance(ctx->node_db);
    char bal_str[32];
    format_amount(t_bal, bal_str, sizeof(bal_str));
    json_push_kv_str(result, "wallet_t_balance", bal_str);

    int64_t z_bal = db_sapling_note_balance(ctx->node_db);
    char zbal_str[32];
    format_amount(z_bal, zbal_str, sizeof(zbal_str));
    json_push_kv_str(result, "wallet_z_balance", zbal_str);

    int64_t total = t_bal + z_bal;
    char tot_str[32];
    format_amount(total, tot_str, sizeof(tot_str));
    json_push_kv_str(result, "total_balance", tot_str);

    struct db_wallet_utxo utxos[256];
    int utxo_count = db_wallet_utxo_list_unspent(ctx->node_db, utxos, 256);
    json_push_kv_int(result, "unspent_utxos", utxo_count);

    return true;
}

static bool rpc_getwalletaccounting(const struct json_value *params,
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

static bool rpc_db_info(const struct json_value *params, bool help,
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

static bool rpc_removestalletxs(const struct json_value *params,
                                 bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
        if (!ctx->wallet->map_wallet[i].used)
            continue;
        struct wallet_tx *wtx = &ctx->wallet->map_wallet[i];
        if (wtx->confirms > 0)
            continue; /* skip confirmed txs */
        if (!wtx->from_me)
            continue; /* skip received unconfirmed */

        /* Check if any input's prevout is spent on-chain
         * (i.e., NOT in the current UTXO set) */
        bool any_input_spent = false;
        if (ctx->coins_tip) {
            for (size_t j = 0; j < wtx->tx.num_vin; j++) {
                struct coins c;
                coins_init(&c);
                bool found = coins_view_cache_get_coins(ctx->coins_tip,
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
            if (wallet_is_mine(ctx->wallet, &wtx->tx.vout[j]))
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
        ctx->wallet->num_wallet_tx--;
        removed++;
        recovered += locked_val;
    }

    /* Phase 2: Rebuild spent set from remaining wallet txs.
     * Do NOT call wallet_verify_utxos here — the C23 chainstate may be
     * incomplete and would incorrectly prune valid UTXOs that exist
     * on-chain but are missing from our coins cache. */
    if (removed > 0) {
        wallet_rebuild_spent_set(ctx->wallet);
    }

    /* Compute new balance */
    int64_t new_balance = wallet_get_balance(ctx->wallet);
    char bal_str[32];
    format_amount(new_balance, bal_str, sizeof(bal_str));

    json_push_kv_int(result, "removed", removed);
    char rec_str[32];
    format_amount(recovered, rec_str, sizeof(rec_str));
    json_push_kv_real(result, "recovered_value", strtod(rec_str, NULL));
    json_push_kv_real(result, "new_balance", strtod(bal_str, NULL));
    json_push_kv_int(result, "wallet_tx_count",
                     (int64_t)ctx->wallet->num_wallet_tx);
    json_push_kv(&result[0], "removed_txs", &removed_list);
    json_free(&removed_list);

    return true;
}

static bool rpc_walletaudit(const struct json_value *params, bool help,
                             struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "walletaudit\n"
        "Definitive wallet balance audit.\n"
        "Verifies every wallet UTXO against the chainstate coins DB.\n"
        "Reports: verified balance, phantom UTXOs, spent-on-chain outputs,\n"
        "and per-address breakdown with chain-verified balances.");

    ENSURE_WALLET(result);

    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    int tip_height = ctx->main_state
        ? active_chain_height(&ctx->main_state->chain_active) : 0;

    json_set_object(result);
    json_push_kv_int(result, "chain_height", tip_height);

    /* Phase 1: Get all wallet UTXOs and verify each against chainstate */
    struct coin_entry wallet_coins[4096];
    size_t num_wallet_coins = 0;
    wallet_available_coins(ctx->wallet, wallet_coins, &num_wallet_coins,
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
        bool in_chain = coins_view_cache_get_coins(ctx->coins_tip,
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
    if (wallet_ctx_db_ready(ctx)) {
        z_balance = db_sapling_note_balance(ctx->node_db);
        struct db_sapling_note db_notes[256];
        z_unspent = db_sapling_note_list_unspent(ctx->node_db, db_notes, 256);
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

    int64_t wallet_reports = wallet_get_balance(ctx->wallet);
    format_amount(wallet_reports, s, sizeof(s));
    json_push_kv_str(&summary, "getbalance_reports", s);

    if (wallet_ctx_db_ready(ctx)) {
        int64_t sqlite_balance = db_wallet_utxo_balance(ctx->node_db);
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

static bool rpc_getchaincoins(const struct json_value *params, bool help,
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

static bool rpc_traceutxo(const struct json_value *params, bool help,
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

static bool rpc_listwalletkeys(const struct json_value *params, bool help,
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

static bool rpc_listwallettxdetail(const struct json_value *params, bool help,
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

    struct db_wallet_tx *rows = calloc((size_t)count, sizeof(struct db_wallet_tx));
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

static bool rpc_getbalanceflow(const struct json_value *params, bool help,
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
    int nutxos = db_wallet_utxo_list_all(ctx->node_db, all_utxos, 4096);

    /* Get all wallet txs sorted by time */
    struct db_wallet_tx *txs = calloc(2000, sizeof(struct db_wallet_tx));
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

static bool rpc_reconcilewalletutxos(const struct json_value *params,
                                      bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "reconcilewalletutxos ( fix )\n"
        "Verify every wallet UTXO against chainstate.\n"
        "Classifies each as: verified, phantom, spent_on_chain, value_mismatch.\n"
        "If fix=true, marks phantoms and spent-on-chain as spent in both\n"
        "in-memory wallet and SQLite.\n"
        "\nArguments:\n"
        "1. fix    (bool, optional, default=false) Fix mismatches\n");

    ENSURE_WALLET(result);
    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
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

    int64_t balance_before = db_wallet_utxo_balance(ctx->node_db);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 4096);

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
        bool found = coins_view_cache_get_coins(ctx->coins_tip, &tid, &c);
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
            db_wallet_utxo_mark_spent(ctx->node_db, unspent[i].txid,
                                       unspent[i].vout,
                                       RECONCILE_SENTINEL, 0);
            wallet_mark_outpoint_spent(ctx->wallet, &tid, unspent[i].vout);
            json_push_kv_bool(&entry, "fixed", true);
            fixed++;
        }

        json_push_back(&details, &entry);
        json_free(&entry);
    }

    int64_t balance_after = fix ? db_wallet_utxo_balance(ctx->node_db)
                                : balance_before;

    if (fix) {
        node_db_state_set_int(ctx->node_db, "last_reconcile_height",
            ctx->main_state
                ? active_chain_height(&ctx->main_state->chain_active) : 0);
    }

    wallet_view_reconcile_summary(result, verified, phantom,
        spent_on_chain, mismatched, fixed, balance_before, balance_after);
    json_push_kv(result, "issues", &details);
    json_free(&details);
    return true;
}

static bool rpc_purgephantomutxos(const struct json_value *params,
                                   bool help, struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "purgephantomutxos confirm ( dryrun )\n"
        "Delete phantom UTXOs from the wallet SQLite database.\n"
        "Phantoms are wallet UTXOs not present in chainstate.\n"
        "\nArguments:\n"
        "1. confirm  (bool, required) Must be true to proceed\n"
        "2. dryrun   (bool, optional, default=false) Report without deleting\n");

    ENSURE_WALLET(result);
    if (!ctx->coins_tip) {
        json_set_str(result, "Chainstate (coins DB) not available");
        return false;
    }
    if (!wallet_ctx_db_ready(ctx)) {
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

    int64_t balance_before = db_wallet_utxo_balance(ctx->node_db);

    struct db_wallet_utxo unspent[4096];
    int count = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 4096);

    int utxos_deleted = 0;
    int txs_deleted = 0;
    int64_t amount_purged = 0;

    node_db_begin(ctx->node_db);

    for (int i = 0; i < count; i++) {
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);

        struct coins c;
        coins_init(&c);
        bool found = coins_view_cache_get_coins(ctx->coins_tip, &tid, &c);
        bool available = found && coins_is_available(&c, unspent[i].vout);
        coins_free(&c);

        if (available)
            continue;

        amount_purged += unspent[i].value;

        if (!dryrun) {
            db_wallet_utxo_delete(ctx->node_db, unspent[i].txid,
                                   unspent[i].vout);
            wallet_mark_outpoint_spent(ctx->wallet, &tid, unspent[i].vout);
        }
        utxos_deleted++;

        if (!dryrun) {
            int remaining = db_wallet_utxo_count_for_tx(ctx->node_db,
                                                         unspent[i].txid);
            if (remaining == 0) {
                db_wallet_tx_delete(ctx->node_db, unspent[i].txid);
                txs_deleted++;
            }
        }
    }

    if (!dryrun) {
        node_db_commit(ctx->node_db);
        wallet_rebuild_spent_set(ctx->wallet);
    } else {
        node_db_rollback(ctx->node_db);
    }

    int64_t balance_after = dryrun ? balance_before
                                   : db_wallet_utxo_balance(ctx->node_db);

    wallet_view_purge_summary(result, utxos_deleted, txs_deleted,
        amount_purged, balance_before, balance_after);
    json_push_kv_bool(result, "dryrun", dryrun);
    return true;
}


static bool rpc_diagnoseutxos(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    wallet_available_coins(ctx->wallet, coins, &num_coins, 4096, false, false);

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
            have = keystore_have_key(&ctx->wallet->keystore, &dest.id.key);
            struct privkey test_key;
            can_get = keystore_get_key(&ctx->wallet->keystore,
                                        &dest.id.key, &test_key);
            if (can_get)
                memory_cleanse(test_key.vch, 32);
        } else if (dest.type == DEST_SCRIPT_ID) {
            have = keystore_have_cscript(&ctx->wallet->keystore,
                                          &dest.id.script.hash);
            json_push_kv_str(&entry, "note",
                "p2sh — need underlying keys, not just script");
        }

        json_push_kv_bool(&entry, "have_key", have);
        json_push_kv_bool(&entry, "can_retrieve_key", can_get);

        /* Chainstate check */
        if (ctx->coins_tip) {
            struct coins c;
            coins_init(&c);
            bool found = coins_view_cache_get_coins(ctx->coins_tip,
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
        (int64_t)ctx->wallet->keystore.num_keys);

    json_push_kv(result, "summary", &summary);
    json_free(&summary);

    json_push_kv(result, "utxos", &utxo_list);
    json_free(&utxo_list);

    return true;
}


/* walletledger: Unified double-entry ledger for transparent + shielded pools.
 * Each transaction shows debits/credits in both pools with running balances.
 * Queries SQLite directly for complete accounting without deserialization. */
static bool rpc_walletledger(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "walletledger ( min_height max_height )\n"
        "\nUnified transparent + shielded ledger with double-entry accounting.\n"
        "Shows every fund movement in both pools chronologically.\n"
        "Each entry: debits, credits, fee, running balances per pool.\n"
        "\nArguments:\n"
        "1. min_height  (int, optional) Filter from this height\n"
        "2. max_height  (int, optional) Filter up to this height\n"
        "\nResult:\n"
        "  entries[]: Chronological list of wallet events\n"
        "  current_holdings: Per-address/note breakdown of current funds\n"
        "  accounting: Complete fund flow summary\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 2);
    int min_h = (int)rpc_permit_int(&p, 0, "min_height", 0);
    int max_h = (int)rpc_permit_int(&p, 1, "max_height", 999999999);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!wallet_ctx_db_ready(ctx)) {
        json_set_str(result, "Wallet database not available");
        return false;
    }

    ENSURE_WALLET(result);

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    /* Load all data from SQLite */
    struct db_wallet_utxo *all_utxos = calloc(4096, sizeof(*all_utxos));
    int nutxos = all_utxos ? db_wallet_utxo_list_all(ctx->node_db, all_utxos, 4096) : 0;

    struct db_sapling_note *all_notes = calloc(1024, sizeof(*all_notes));
    int nnotes = all_notes ? db_sapling_note_list_all(ctx->node_db, all_notes, 1024) : 0;

    struct db_wallet_tx *all_txs = calloc(2000, sizeof(*all_txs));
    int ntxs = all_txs ? db_wallet_tx_list(ctx->node_db, all_txs, 2000, 0) : 0;

    /* Build a height-sorted list of unique txids */
    struct {
        uint8_t txid[32];
        int height;
        bool from_me;
        int64_t fee;
    } *events = calloc(4096, sizeof(*events));
    int nevents = 0;

    /* Collect events from wallet transactions */
    for (int i = 0; i < ntxs; i++) {
        if (all_txs[i].block_height < min_h || all_txs[i].block_height > max_h)
            continue;
        bool dup = false;
        for (int j = 0; j < nevents; j++) {
            if (memcmp(events[j].txid, all_txs[i].txid, 32) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup && nevents < 4096) {
            memcpy(events[nevents].txid, all_txs[i].txid, 32);
            events[nevents].height = all_txs[i].block_height;
            events[nevents].from_me = all_txs[i].from_me;
            events[nevents].fee = all_txs[i].fee;
            nevents++;
        }
    }

    /* Sort by height ascending */
    for (int i = 0; i < nevents - 1; i++) {
        for (int j = i + 1; j < nevents; j++) {
            if (events[j].height < events[i].height) {
                typeof(events[0]) tmp = events[i];
                events[i] = events[j];
                events[j] = tmp;
            }
        }
    }

    json_set_object(result);

    struct json_value entries = {0};
    json_set_array(&entries);

    int64_t t_balance = 0, z_balance = 0;
    int64_t total_t_received = 0, total_t_sent = 0;
    int64_t total_z_received = 0, total_z_sent = 0;
    int64_t total_shielded = 0, total_unshielded = 0;
    int64_t total_fees = 0;
    int64_t total_z_fees = 0;

    for (int e = 0; e < nevents; e++) {
        /* For this tx, find: T credits, T debits, Z credits, Z debits */
        int64_t t_credit = 0, t_debit = 0;
        int64_t z_credit = 0, z_debit = 0;
        int t_credit_count = 0, t_debit_count = 0;
        int z_credit_count = 0, z_debit_count = 0;

        /* T credits: UTXOs created by this tx */
        for (int u = 0; u < nutxos; u++) {
            if (memcmp(all_utxos[u].txid, events[e].txid, 32) == 0) {
                t_credit += all_utxos[u].value;
                t_credit_count++;
            }
        }

        /* T debits: UTXOs spent by this tx */
        for (int u = 0; u < nutxos; u++) {
            if (all_utxos[u].is_spent &&
                memcmp(all_utxos[u].spent_txid, events[e].txid, 32) == 0) {
                t_debit += all_utxos[u].value;
                t_debit_count++;
            }
        }

        /* Z credits: notes created by this tx (received into wallet) */
        for (int n = 0; n < nnotes; n++) {
            if (memcmp(all_notes[n].txid, events[e].txid, 32) == 0) {
                z_credit += all_notes[n].value;
                z_credit_count++;
            }
        }

        /* Z debits: notes spent by this tx */
        for (int n = 0; n < nnotes; n++) {
            if (all_notes[n].is_spent &&
                memcmp(all_notes[n].spent_txid, events[e].txid, 32) == 0) {
                z_debit += all_notes[n].value;
                z_debit_count++;
            }
        }

        /* Fee */
        int64_t fee = events[e].from_me ? events[e].fee : 0;
        if (fee < 0) fee = -fee;

        /* Classify the transaction type */
        const char *type;
        if (t_debit > 0 && z_credit > 0 && t_credit == 0 && z_debit == 0)
            type = "shield";     /* t → z (pure shielding) */
        else if (t_debit > 0 && z_credit > 0 && t_credit > 0)
            type = "shield";     /* t → z with change */
        else if (z_debit > 0 && t_credit > 0 && z_credit == 0)
            type = "unshield";   /* z → t */
        else if (z_debit > 0 && t_credit > 0 && z_credit > 0)
            type = "unshield";   /* z → t with z-change */
        else if (z_debit > 0 && z_credit > 0 && t_credit == 0 && t_debit == 0)
            type = "z_transfer"; /* z → z */
        else if (z_debit > 0 && t_debit == 0 && t_credit == 0 && z_credit == 0)
            type = "z_send";     /* z → external */
        else if (t_debit > 0 && t_credit > 0 && z_credit == 0 && z_debit == 0)
            type = events[e].from_me ? "send" : "receive";
        else if (t_debit == 0 && t_credit > 0 && z_credit == 0 && z_debit == 0)
            type = "receive";    /* external → t */
        else if (t_debit == 0 && t_credit == 0 && z_credit > 0 && z_debit == 0)
            type = "z_receive";  /* external → z */
        else if (t_debit > 0 && t_credit == 0 && z_credit == 0 && z_debit == 0)
            type = "send";       /* t → external */
        else
            type = "mixed";

        /* Update running balances */
        int64_t t_net = t_credit - t_debit;
        int64_t z_net = z_credit - z_debit;
        t_balance += t_net;
        z_balance += z_net;

        /* Track accounting totals (net flows, not gross) */
        if (!events[e].from_me && t_credit > 0)
            total_t_received += t_credit;  /* external deposits */
        if (!events[e].from_me && z_credit > 0)
            total_z_received += z_credit;  /* external z-deposits */
        if (events[e].from_me) {
            /* For our sends: external_out = debit - credit - shielded */
            int64_t t_external = t_debit - t_credit - fee;
            if (z_credit > 0) t_external -= z_credit; /* value went to z-pool */
            if (t_external > 0) total_t_sent += t_external;
            /* z outflow: z_debit - z_credit (change) */
            int64_t z_external = z_debit - z_credit;
            if (t_credit > 0 && z_debit > 0) z_external -= 0; /* unshield */
            if (z_external > 0) total_z_sent += z_external;
        }
        if (t_debit > 0 && z_credit > 0) total_shielded += z_credit;
        if (z_debit > 0 && t_credit > 0 && !events[e].from_me)
            total_unshielded += t_credit;
        else if (z_debit > 0 && t_credit > 0 && events[e].from_me && t_debit == 0)
            total_unshielded += t_credit;
        total_fees += fee;

        /* Build entry */
        char txid_hex[65];
        struct uint256 tid;
        memcpy(tid.data, events[e].txid, 32);
        uint256_get_hex(&tid, txid_hex);

        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_int(&entry, "height", events[e].height);
        json_push_kv_str(&entry, "txid", txid_hex);
        json_push_kv_str(&entry, "type", type);

        char s[32];
        if (t_debit > 0) {
            format_amount(t_debit, s, sizeof(s));
            json_push_kv_str(&entry, "t_debit", s);
        }
        if (t_credit > 0) {
            format_amount(t_credit, s, sizeof(s));
            json_push_kv_str(&entry, "t_credit", s);
        }
        if (z_debit > 0) {
            format_amount(z_debit, s, sizeof(s));
            json_push_kv_str(&entry, "z_debit", s);
        }
        if (z_credit > 0) {
            format_amount(z_credit, s, sizeof(s));
            json_push_kv_str(&entry, "z_credit", s);
        }
        if (fee > 0) {
            format_amount(fee, s, sizeof(s));
            json_push_kv_str(&entry, "fee", s);
        }

        format_amount(t_balance, s, sizeof(s));
        json_push_kv_str(&entry, "t_balance", s);
        format_amount(z_balance, s, sizeof(s));
        json_push_kv_str(&entry, "z_balance", s);
        format_amount(t_balance + z_balance, s, sizeof(s));
        json_push_kv_str(&entry, "total_balance", s);

        json_push_back(&entries, &entry);
        json_free(&entry);
    }

    json_push_kv(result, "entries", &entries);
    json_free(&entries);

    /* Current holdings: per-address breakdown */
    struct json_value holdings = {0};
    json_set_object(&holdings);

    struct json_value t_holdings = {0};
    json_set_array(&t_holdings);

    struct db_wallet_utxo unspent[1024];
    int nu = db_wallet_utxo_list_unspent(ctx->node_db, unspent, 1024);
    int64_t verified_t = 0;

    for (int i = 0; i < nu; i++) {
        struct json_value h = {0};
        json_set_object(&h);

        char txid_hex[65];
        struct uint256 tid;
        memcpy(tid.data, unspent[i].txid, 32);
        uint256_get_hex(&tid, txid_hex);
        json_push_kv_str(&h, "txid", txid_hex);
        json_push_kv_int(&h, "vout", unspent[i].vout);

        /* Resolve address from script */
        char addr[128] = {0};
        if (unspent[i].script && unspent[i].script_len > 0) {
            struct script scr;
            script_init(&scr);
            size_t cplen = unspent[i].script_len < MAX_SCRIPT_SIZE
                         ? unspent[i].script_len : MAX_SCRIPT_SIZE;
            memcpy(scr.data, unspent[i].script, cplen);
            scr.size = cplen;
            struct tx_destination dest;
            if (script_extract_destination(&scr, &dest))
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));
        }
        if (addr[0]) json_push_kv_str(&h, "address", addr);

        char s[32];
        format_amount(unspent[i].value, s, sizeof(s));
        json_push_kv_str(&h, "amount", s);
        json_push_kv_int(&h, "height", unspent[i].height);

        verified_t += unspent[i].value;

        json_push_back(&t_holdings, &h);
        json_free(&h);
        db_wallet_utxo_free(&unspent[i]);
    }

    json_push_kv(&holdings, "transparent", &t_holdings);
    json_free(&t_holdings);

    /* Shielded holdings */
    struct json_value z_holdings = {0};
    json_set_array(&z_holdings);

    struct db_sapling_note unspent_notes[256];
    int nn = db_sapling_note_list_unspent(ctx->node_db, unspent_notes, 256);
    int64_t verified_z = 0;

    for (int i = 0; i < nn; i++) {
        struct json_value h = {0};
        json_set_object(&h);

        char txid_hex[65];
        struct uint256 tid;
        memcpy(tid.data, unspent_notes[i].txid, 32);
        uint256_get_hex(&tid, txid_hex);
        json_push_kv_str(&h, "txid", txid_hex);
        json_push_kv_int(&h, "output_index", unspent_notes[i].output_index);

        /* Derive z-address from diversifier + pk_d */
        char zaddr[128] = {0};
        sapling_encode_payment_address(
            unspent_notes[i].diversifier, unspent_notes[i].pk_d,
            "zs", zaddr, sizeof(zaddr));
        if (zaddr[0]) json_push_kv_str(&h, "address", zaddr);

        char s[32];
        format_amount(unspent_notes[i].value, s, sizeof(s));
        json_push_kv_str(&h, "amount", s);
        json_push_kv_int(&h, "height", unspent_notes[i].block_height);

        /* Decode memo if non-empty */
        if (unspent_notes[i].memo_len > 0 && unspent_notes[i].memo[0] != 0xf6) {
            size_t mlen = unspent_notes[i].memo_len;
            while (mlen > 0 && unspent_notes[i].memo[mlen - 1] == 0) mlen--;
            if (mlen > 0) {
                char memo_str[513];
                size_t copy_len = mlen < 512 ? mlen : 512;
                memcpy(memo_str, unspent_notes[i].memo, copy_len);
                memo_str[copy_len] = '\0';
                json_push_kv_str(&h, "memo", memo_str);
            }
        }

        verified_z += unspent_notes[i].value;

        json_push_back(&z_holdings, &h);
        json_free(&h);
        db_sapling_note_free(&unspent_notes[i]);
    }

    json_push_kv(&holdings, "shielded", &z_holdings);
    json_free(&z_holdings);

    json_push_kv(result, "current_holdings", &holdings);
    json_free(&holdings);

    /* Accounting summary */
    struct json_value acct = {0};
    json_set_object(&acct);
    char s[32];

    format_amount(total_t_received, s, sizeof(s));
    json_push_kv_str(&acct, "total_t_received", s);

    format_amount(total_z_received, s, sizeof(s));
    json_push_kv_str(&acct, "total_z_received", s);

    format_amount(total_shielded, s, sizeof(s));
    json_push_kv_str(&acct, "total_shielded_t_to_z", s);

    format_amount(total_unshielded, s, sizeof(s));
    json_push_kv_str(&acct, "total_unshielded_z_to_t", s);

    format_amount(total_fees + total_z_fees, s, sizeof(s));
    json_push_kv_str(&acct, "total_fees", s);

    format_amount(verified_t, s, sizeof(s));
    json_push_kv_str(&acct, "current_t_balance", s);

    format_amount(verified_z, s, sizeof(s));
    json_push_kv_str(&acct, "current_z_balance", s);

    format_amount(verified_t + verified_z, s, sizeof(s));
    json_push_kv_str(&acct, "current_total", s);

    /* Verify: received - sent - fees should equal current balance */
    int64_t expected = total_t_received + total_z_received
                     - total_t_sent - total_z_sent
                     - (total_fees + total_z_fees);
    int64_t actual = verified_t + verified_z;
    int64_t unaccounted = expected - actual;
    if (unaccounted < 0) unaccounted = -unaccounted;
    format_amount(unaccounted, s, sizeof(s));
    json_push_kv_str(&acct, "unaccounted", s);

    json_push_kv_int(&acct, "transparent_utxos", nu);
    json_push_kv_int(&acct, "shielded_notes", nn);
    json_push_kv_int(&acct, "ledger_entries", nevents);

    json_push_kv(result, "accounting", &acct);
    json_free(&acct);

    /* Cleanup */
    for (int i = 0; i < nutxos; i++) db_wallet_utxo_free(&all_utxos[i]);
    for (int i = 0; i < nnotes; i++) db_sapling_note_free(&all_notes[i]);
    free(all_utxos);
    free(all_notes);
    for (int i = 0; i < ntxs; i++) db_wallet_tx_free(&all_txs[i]);
    free(all_txs);
    free(events);

    return true;
}

void register_wallet_diagnostic_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "wallet", "scanblockfiles",      rpc_scanblockfiles,       false },
        { "wallet", "reindexdb",           rpc_reindexdb,            false },
        { "wallet", "importlegacy",        rpc_importlegacy,         false },
        { "wallet", "getwalletaccounting", rpc_getwalletaccounting,  false },
        { "wallet", "db_info",             rpc_db_info,              false },
        { "wallet", "removestalletxs",     rpc_removestalletxs,      false },
        { "wallet", "walletaudit",         rpc_walletaudit,          false },
        { "wallet", "getchaincoins",       rpc_getchaincoins,        false },
        { "wallet", "traceutxo",           rpc_traceutxo,            false },
        { "wallet", "listwalletkeys",      rpc_listwalletkeys,       false },
        { "wallet", "listwallettxdetail",  rpc_listwallettxdetail,   false },
        { "wallet", "getbalanceflow",      rpc_getbalanceflow,       false },
        { "wallet", "reconcilewalletutxos", rpc_reconcilewalletutxos, false },
        { "wallet", "purgephantomutxos",   rpc_purgephantomutxos,    false },
        { "wallet", "diagnoseutxos",       rpc_diagnoseutxos,        false },
        { "wallet", "walletledger",        rpc_walletledger,         false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
