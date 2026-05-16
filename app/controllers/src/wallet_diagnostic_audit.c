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

/* wallet-diagnostic audit RPCs: cross-reference UTXO + tx state, no mutations. */

bool rpc_walletaudit(const struct json_value *params, bool help,
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
    if (!rpc_require_chainstate_lookup_ready(ctx->main_state, result,
            "walletaudit", "Chainstate lookup"))
        return false;

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
bool rpc_diagnoseutxos(const struct json_value *params, bool help,
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
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "diagnoseutxos",
            "Chainstate lookup"))
        return false;

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
bool rpc_walletledger(const struct json_value *params, bool help,
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
    struct db_wallet_utxo *all_utxos = zcl_calloc(4096, sizeof(*all_utxos), "diag_utxos");
    int nutxos = all_utxos ? db_wallet_utxo_list_all(ctx->node_db, all_utxos, 4096) : 0;

    struct db_sapling_note *all_notes = zcl_calloc(1024, sizeof(*all_notes), "diag_notes");
    int nnotes = all_notes ? db_sapling_note_list_all(ctx->node_db, all_notes, 1024) : 0;

    struct db_wallet_tx *all_txs = zcl_calloc(2000, sizeof(*all_txs), "diag_txs");
    int ntxs = all_txs ? db_wallet_tx_list(ctx->node_db, all_txs, 2000, 0) : 0;

    /* Build a height-sorted list of unique txids */
    struct {
        uint8_t txid[32];
        int height;
        bool from_me;
        int64_t fee;
    } *events = zcl_calloc(4096, sizeof(*events), "diag_events");
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
