/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/wallet_shielded_controller.h"
#include "controllers/wallet_helpers.h"
#include "controllers/strong_params.h"
#include "views/format_helpers.h"
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
#include "core/serialize.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/txmempool.h"
#include "wallet/wallet_sqlite.h"
#include "net/connman.h"
#include "sapling/sapling.h"
#include "sapling/fr.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling_prover.h"
#include "consensus/upgrades.h"
#include "services/wallet_backup_service.h"
#include "models/database.h"
#include "models/block.h"
#include "models/utxo.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_scan.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct wallet_rpc_context *wallet_ctx(void)
{
    return wallet_rpc_context_current();
}

static bool wallet_ctx_db_ready(const struct wallet_rpc_context *ctx)
{
    return ctx->node_db && ctx->node_db->open;
}

static bool rpc_z_getnewaddress(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "z_getnewaddress\n"
        "\nReturns a new Sapling shielded address.\n"
        "\nResult:\n"
        "\"address\"  (string) The new z-address\n");

    ENSURE_WALLET(result);

    uint8_t diversifier[11];
    uint8_t pk_d[32];
    if (!sapling_keystore_new_address(&ctx->wallet->sapling_keys,
                                       diversifier, pk_d)) {
        json_set_str(result, "Failed to generate Sapling address");
        LOG_FAIL("wallet_shielded", "sapling_keystore_new_address failed");
    }

    const struct chain_params *cp = chain_params_get();
    char addr[128];
    if (!sapling_encode_payment_address(diversifier, pk_d,
            cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS],
            addr, sizeof(addr))) {
        json_set_str(result, "Failed to encode address");
        LOG_FAIL("wallet_shielded", "sapling_encode_payment_address failed for HRP=%s",
                 cp->bech32HRPs[BECH32_SAPLING_PAYMENT_ADDRESS]);
    }

    /* Persist sapling keys to wallet DB. Errors here are fatal to the
     * RPC: the in-memory key is derivable from the seed + child_index
     * but if either row fails to land on disk we are shipping a
     * z-address the user cannot recover after restart. Report and
     * let the persistence health block flag the daemon. */
    if (ctx->wallet_db) {
        struct sapling_keystore *sks = &ctx->wallet->sapling_keys;
        if (sks->has_seed) {
            if (!wallet_sqlite_write_sapling_seed(ctx->wallet_db, sks->seed)) {
                json_set_str(result,
                    "Error: failed to persist Sapling seed. Address NOT saved. "
                    "Check getwalletinfo.persistence and node.log.");
                LOG_FAIL("wallet_shielded", "z_getnewaddress: sapling_seed flush failed");
            }
        }
        if (sks->num_keys > 0) {
            if (!wallet_sqlite_write_sapling_key(ctx->wallet_db,
                    sks->keys[sks->num_keys - 1].child_index,
                    &sks->keys[sks->num_keys - 1])) {
                json_set_str(result,
                    "Error: failed to persist Sapling key. Address NOT saved. "
                    "Check getwalletinfo.persistence and node.log.");
                LOG_FAIL("wallet_shielded", "z_getnewaddress: sapling_key flush failed");
            }
        }
        wallet_backup_service_on_key_change();
    }

    json_set_str(result, addr);
    return true;
}

static bool rpc_z_listaddresses(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    (void)params;
    RPC_HELP(help, result,
        "z_listaddresses\n"
        "\nReturns all Sapling z-addresses in the wallet.\n");

    ENSURE_WALLET(result);

    json_set_array(result);
    const struct chain_params *cp = chain_params_get();

    for (size_t i = 0; i < ctx->wallet->sapling_keys.num_keys; i++) {
        if (!ctx->wallet->sapling_keys.keys[i].used) continue;
        char addr[128];
        if (sapling_encode_payment_address(
                ctx->wallet->sapling_keys.keys[i].diversifier,
                ctx->wallet->sapling_keys.keys[i].pk_d,
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

static bool rpc_z_getbalance(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "z_getbalance \"address\" ( minconf )\n"
        "\nReturns the balance for a taddr or zaddr.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int minconf = (int)rpc_permit_int(&p, 1, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_getbalance: invalid params"); }

    ENSURE_WALLET(result);

    /* Check if Sapling address */
    uint8_t z_d[11], z_pkd[32];
    if (sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        int64_t balance = 0;
        bool found_in_memory = false;
        for (size_t i = 0; i < ctx->wallet->num_sapling_notes; i++) {
            const struct sapling_received_note *n = &ctx->wallet->sapling_notes[i];
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
        if (!found_in_memory && ctx->node_db) {
            const struct sapling_key_entry *ske =
                sapling_keystore_find_by_address(&ctx->wallet->sapling_keys, z_d, z_pkd);
            if (ske)
                balance = db_sapling_note_balance_for_ivk(ctx->node_db, ske->ivk);
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
        LOG_FAIL("wallet_shielded", "z_getbalance: decode_destination failed for addr=%s", addr_str);
    }

    int64_t balance = 0;
    struct coin_entry coins[4096];
    size_t num_coins = 0;
    wallet_available_coins(ctx->wallet, coins, &num_coins, 4096,
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
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "z_listunspent ( minconf maxconf )\n"
        "\nReturns list of unspent shielded notes.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    int minconf = (int)rpc_permit_int(&p, 0, "minconf", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_listunspent: invalid params"); }

    ENSURE_WALLET(result);

    json_set_array(result);

    /* Always read from SQLite (authoritative source for shielded notes) */
    if (ctx->node_db) {
        struct db_sapling_note db_notes[256];
        int count = db_sapling_note_list_unspent(ctx->node_db, db_notes, 256);
        int chain_h = ctx->wallet->best_block_height;
        if (chain_h == 0 && ctx->main_state)
            chain_h = active_chain_height(&ctx->main_state->chain_active);
        if (chain_h == 0 && wallet_ctx_db_ready(ctx)) {
            sqlite3_stmt *hs = NULL;
            sqlite3_prepare_v2(ctx->node_db->db,
                "SELECT MAX(height) FROM blocks", -1, &hs, NULL);
            if (hs && AR_STEP_ROW_READONLY(hs) == SQLITE_ROW)
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

            /* Memo — show if non-empty */
            bool has_memo = false;
            for (size_t j = 0; j < n->memo_len && j < 512; j++) {
                if (n->memo[j] != 0 && n->memo[j] != 0xf6) {
                    has_memo = true;
                    break;
                }
            }
            if (has_memo) {
                if (n->memo[0] >= 0x20 && n->memo[0] < 0x7f) {
                    size_t len = 0;
                    while (len < n->memo_len && len < 512 &&
                           n->memo[len] != 0 && n->memo[len] != 0xf6)
                        len++;
                    char memo_str[513];
                    memcpy(memo_str, n->memo, len);
                    memo_str[len] = '\0';
                    json_push_kv_str(&entry, "memo", memo_str);
                } else {
                    char hex[1025];
                    size_t last = 0;
                    for (size_t j = 0; j < n->memo_len && j < 512; j++)
                        if (n->memo[j]) last = j;
                    for (size_t j = 0; j <= last; j++)
                        snprintf(hex + j * 2, 3, "%02x", n->memo[j]);
                    hex[(last + 1) * 2] = '\0';
                    json_push_kv_str(&entry, "memo_hex", hex);
                }
            }

            json_push_back(result, &entry);
        }
    }
    return true;
}

/* z_sendmany: send from transparent address to one or more Sapling/transparent recipients */
static bool rpc_z_sendmany(const struct json_value *params, bool help,
                             struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result,
        "z_sendmany \"fromaddress\" [{\"address\":\"...\",\"amount\":...,\"memo\":\"...\"},...]\n"
        "\nSend from a transparent or shielded address to multiple recipients.\n"
        "Supports t→t, t→z, z→z, and z→t transactions.\n");

    if (json_size(params) < 2) {
        json_set_str(result, "Expected at least 2 parameter(s)");
        LOG_FAIL("wallet_shielded", "z_sendmany: expected 2+ params, got %zu", json_size(params));
    }

    ENSURE_WALLET(result);

    const char *from_addr = json_get_str(json_at(params, 0));
    const struct json_value *recipients = json_at(params, 1);
    if (!from_addr || !recipients || recipients->type != JSON_ARR || json_size(recipients) == 0) {
        json_set_str(result, "Invalid parameters");
        LOG_FAIL("wallet_shielded", "z_sendmany: invalid from_addr or recipients array");
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
            LOG_FAIL("wallet_shielded", "z_sendmany: cannot decode shielded from address %s", from_addr);
        }
        from_z_key = sapling_keystore_find_by_address(
            &ctx->wallet->sapling_keys, from_z_diversifier, from_z_pk_d);
        if (!from_z_key) {
            json_set_str(result, "Shielded from address not in wallet");
            LOG_FAIL("wallet_shielded", "z_sendmany: shielded from addr not found in keystore");
        }
        memset(&from_dest, 0, sizeof(from_dest));
    } else if (!decode_destination(from_addr, pk_pfx, pk_pfx_len, sc_pfx, sc_pfx_len, &from_dest)) {
        json_set_str(result, "Invalid from address");
        LOG_FAIL("wallet_shielded", "z_sendmany: decode_destination failed for from=%s", from_addr);
    }

    /* Parse recipients */
    size_t num_recip = json_size(recipients);
    if (num_recip > 50) {
        json_set_str(result, "Too many recipients");
        LOG_FAIL("wallet_shielded", "z_sendmany: too many recipients (%zu > 50)", num_recip);
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
            LOG_FAIL("wallet_shielded", "z_sendmany: recipient %zu is not a JSON object", i);
        }
        const char *addr = json_get_str(json_get(r, "address"));
        int64_t amount = parse_amount(json_get(r, "amount"));
        if (!addr || amount <= 0) {
            json_set_str(result, "Invalid recipient address or amount");
            LOG_FAIL("wallet_shielded", "z_sendmany: recipient %zu has invalid addr or amount=%lld", i, (long long)amount);
        }
        total_amount += amount;

        if (strncmp(addr, "zs1", 3) == 0) {
            /* Sapling shielded output */
            if (!sapling_decode_payment_address(addr,
                    z_diversifiers[num_z_out], z_pk_ds[num_z_out])) {
                json_set_str(result, "Invalid Sapling address");
                LOG_FAIL("wallet_shielded", "z_sendmany: cannot decode sapling recipient addr=%s", addr);
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
                LOG_FAIL("wallet_shielded", "z_sendmany: cannot decode transparent recipient addr=%s", addr);
            }
            t_amounts[num_t_out] = amount;
            num_t_out++;
        }
    }

    /* ── Shielded spend path (z→z, z→t) ──────────────────────────── */
    if (from_is_shielded) {
        int64_t fee = ctx->wallet->default_fee;

        /* Select unspent notes for the from z-address */
        struct db_sapling_note notes[256];
        int num_notes = db_sapling_note_list_unspent_for_ivk(
            ctx->node_db, from_z_key->ivk, notes, 256);
        if (num_notes <= 0) {
            json_set_str(result, "No unspent shielded notes for this address");
            LOG_FAIL("wallet_shielded", "z_sendmany: no unspent notes for from z-address");
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
            LOG_FAIL("wallet_shielded", "z_sendmany: insufficient shielded funds: have=%lld need=%lld",
                     (long long)notes_total, (long long)(total_amount + fee));
        }

        /* Witnesses are maintained incrementally by connect_block and bulk_blocks.
         * Load them directly — they are already at chain tip. */
        uint8_t anchor[32];
        struct incremental_witness *witnesses = zcl_calloc(num_sel_notes,
            sizeof(struct incremental_witness), "sapling_witnesses");
        if (!witnesses) {
            json_set_str(result, "Out of memory");
            LOG_FAIL("wallet_shielded", "z_sendmany: calloc witnesses failed for %zu notes", num_sel_notes);
        }
        struct incremental_merkle_tree dummy_tree;
        sapling_tree_init(&dummy_tree);

        int witness_height = 0;
        (void)witness_height;
        for (size_t i = 0; i < num_sel_notes; i++) {
            uint8_t *wblob = NULL;
            size_t wlen = 0;
            int wheight = 0;
            if (!db_sapling_note_load_witness(ctx->node_db,
                    selected_notes[i].txid, selected_notes[i].output_index,
                    &wblob, &wlen, &wheight) || !wblob) {
                free(witnesses);
                json_set_str(result, "Witness not available (run rescanwitnesses)");
                LOG_FAIL("wallet_shielded", "z_sendmany: witness not available for note %zu", i);
            }
            if (i == 0) witness_height = wheight;
            struct byte_stream ws;
            stream_init_from_data(&ws, wblob, wlen);
            if (!incremental_witness_deserialize(&witnesses[i], &ws,
                    SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
                    dummy_tree.combine, dummy_tree.uncommitted)) {
                free(wblob); free(witnesses);
                json_set_str(result, "Failed to deserialize witness");
                LOG_FAIL("wallet_shielded", "z_sendmany: witness deserialize failed for note %zu", i);
            }
            free(wblob);
        }

        /* Anchor = witness root at chain tip.
         * All witnesses share the same tree state, so use witness[0]'s root. */
        {
            struct uint256 w0root;
            incremental_witness_root(&witnesses[0], &w0root);
            memcpy(anchor, w0root.data, 32);
        }

        /* Verify ALL witness roots match the anchor */
        for (size_t vi = 1; vi < num_sel_notes; vi++) {
            struct uint256 wroot;
            incremental_witness_root(&witnesses[vi], &wroot);
            if (memcmp(wroot.data, anchor, 32) != 0) {
                char ah[65], wh[65];
                uint256_get_hex((const struct uint256 *)anchor, ah);
                uint256_get_hex(&wroot, wh);
                fprintf(stderr, "z_sendmany: witness %zu root differs from witness 0 "
                    "w0=%s w%zu=%s\n", vi, ah, vi, wh);
                free(witnesses);
                json_set_str(result, "Witness roots inconsistent (run rescanwitnesses)");
                return false;
            }
        }

        /* Build transaction */
        struct wallet_tx wtx;
        memset(&wtx, 0, sizeof(wtx));
        transaction_init(&wtx.tx);

        int height = ctx->wallet->best_block_height;
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
                LOG_FAIL("wallet_shielded", "z_sendmany: transaction_alloc failed for %zu t-outputs (shielded path)", total_t_out_shielded);
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
        void *proving_ctx = zclassic_sapling_proving_ctx_init();
        if (!proving_ctx) {
            free(witnesses);
            transaction_free(&wtx.tx);
            json_set_str(result, "Failed to init proving context");
            LOG_FAIL("wallet_shielded", "z_sendmany: sapling_proving_ctx_init failed (shielded spend path)");
        }

        /* Build spend descriptions */
        wtx.tx.v_shielded_spend = zcl_calloc(num_sel_notes, sizeof(struct spend_description), "shielded_spends");
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
            wtx.tx.v_shielded_output = zcl_calloc(total_z_outs,
                sizeof(struct output_description), "shielded_outputs");
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
                /* rsk = ask + ar in Jubjub scalar field (Fs, NOT Fr).
                 * ask and ar are Fs scalars — using Fr addition would
                 * produce a wrong result since Fr.p != Fs.p. */
                uint8_t rsk[32];
                struct fs ask_fs, ar_fs, rsk_fs;
                fs_from_bytes(&ask_fs, from_z_key->xsk.expsk.ask);
                fs_from_bytes(&ar_fs, spend_ars[i]);
                fs_add(&rsk_fs, &ask_fs, &ar_fs);
                fs_to_bytes(rsk, &rsk_fs);
                memory_cleanse(&ask_fs, sizeof(ask_fs));
                memory_cleanse(&ar_fs, sizeof(ar_fs));
                memory_cleanse(&rsk_fs, sizeof(rsk_fs));

                if (!redjubjub_sign(rsk, sighash.data, 32,
                                    wtx.tx.v_shielded_spend[i].spend_auth_sig,
                                    5 /* GEN_SPENDING_KEY */))
                    spend_err = "Spend auth signature failed";
                memory_cleanse(rsk, 32);
            }

            if (!spend_err &&
                !zclassic_sapling_binding_sig(proving_ctx,
                    wtx.tx.value_balance, sighash.data, wtx.tx.binding_sig))
                spend_err = "Binding signature failed";
        }

shielded_cleanup:
        zclassic_sapling_proving_ctx_free(proving_ctx);
        memory_cleanse(spend_ars, sizeof(spend_ars));

        if (spend_err) {
            free(witnesses);
            transaction_free(&wtx.tx);
            json_set_str(result, spend_err);
            LOG_FAIL("wallet_shielded", "z_sendmany: shielded spend failed: %s", spend_err);
        }

        free(witnesses);

        /* Broadcast */
        transaction_compute_hash(&wtx.tx);

        if (ctx->mempool) {
            struct mempool_entry me;
            mempool_entry_init(&me, &wtx.tx, fee, (int64_t)time(NULL),
                               0.0, (unsigned int)height, true, false, 0);
            tx_mempool_add_unchecked(ctx->mempool, &wtx.tx.hash, &me);
        }

        char txid_hex[65];
        uint256_get_hex(&wtx.tx.hash, txid_hex);
        json_set_str(result, txid_hex);

        transaction_free(&wtx.tx);
        return true;
    }

    /* ── Transparent spend path (t→t, t→z) ─────────────────────── */

    /* Select coins from SQLite model layer — filter to from address */
    int64_t fee = ctx->wallet->default_fee;
    int tip = active_chain_height(&ctx->main_state->chain_active);
    struct db_wallet_utxo db_utxos[256];
    int n_utxos = 0;

    if (wallet_ctx_db_ready(ctx))
        n_utxos = db_wallet_utxo_select_coins(ctx->node_db,
            total_amount + fee, tip, db_utxos, 256);

    /* Filter to coins matching the from address */
    struct db_wallet_utxo db_selected[256];
    size_t num_selected = 0;
    int64_t selected_value = 0;

    for (int i = 0; i < n_utxos; i++) {
        if (!db_utxos[i].script || db_utxos[i].script_len == 0)
            continue;
        struct script sc;
        script_init(&sc);
        memcpy(sc.data, db_utxos[i].script, db_utxos[i].script_len);
        sc.size = db_utxos[i].script_len;
        struct tx_destination coin_dest;
        if (!script_extract_destination(&sc, &coin_dest))
            continue;
        bool match = false;
        if (coin_dest.type == from_dest.type) {
            if (coin_dest.type == DEST_KEY_ID)
                match = (memcmp(coin_dest.id.key.id.data,
                                from_dest.id.key.id.data, 20) == 0);
            else if (coin_dest.type == DEST_SCRIPT_ID)
                match = (memcmp(coin_dest.id.script.hash.data,
                                from_dest.id.script.hash.data, 20) == 0);
        }
        if (match) {
            db_selected[num_selected] = db_utxos[i];
            db_utxos[i].script = NULL; /* prevent double-free */
            selected_value += db_utxos[i].value;
            num_selected++;
            if (selected_value >= total_amount + fee)
                break;
        }
    }
    for (int i = 0; i < n_utxos; i++)
        db_wallet_utxo_free(&db_utxos[i]);

    if (selected_value < total_amount + fee) {
        for (size_t i = 0; i < num_selected; i++)
            db_wallet_utxo_free(&db_selected[i]);
        json_set_str(result, "Insufficient funds from specified address");
        LOG_FAIL("wallet_shielded", "z_sendmany: insufficient transparent funds: have=%lld need=%lld",
                 (long long)selected_value, (long long)(total_amount + fee));
    }

    /* Build transaction */
    struct wallet_tx wtx;
    memset(&wtx, 0, sizeof(wtx));
    transaction_init(&wtx.tx);

    int height = tip;
    wtx.tx.overwintered = true;
    wtx.tx.version = SAPLING_TX_VERSION;
    wtx.tx.version_group_id = SAPLING_VERSION_GROUP_ID;
    wtx.tx.expiry_height = (uint32_t)(height + 20);

    /* Transparent outputs: recipients + change */
    int64_t change = selected_value - total_amount - fee;
    size_t total_t_out = num_t_out + (change > 0 ? 1 : 0);

    if (!transaction_alloc(&wtx.tx, num_selected, total_t_out)) {
        json_set_str(result, "Transaction allocation failed");
        LOG_FAIL("wallet_shielded", "z_sendmany: transaction_alloc failed for %zu inputs, %zu outputs",
                 num_selected, total_t_out);
    }

    /* Fill transparent outputs */
    for (size_t i = 0; i < num_t_out; i++) {
        struct script dest_script;
        script_for_destination(&dest_script, &t_dests[i]);
        wtx.tx.vout[i].value = t_amounts[i];
        wtx.tx.vout[i].script_pub_key = dest_script;
    }

    /* Change output — send back to the FROM address (no keypool needed) */
    if (change > 0) {
        struct pubkey change_pk;
        bool got_key = wallet_get_key_from_pool(ctx->wallet, &change_pk);
        if (!got_key) {
            /* Keypool exhausted — use the from_address as change address */
            struct tx_destination change_dest = from_dest;
            struct script change_script;
            script_for_destination(&change_script, &change_dest);
            wtx.tx.vout[num_t_out].value = change;
            wtx.tx.vout[num_t_out].script_pub_key = change_script;
        } else {
            struct key_id change_kid = pubkey_get_id(&change_pk);
            struct tx_destination change_dest;
            change_dest.type = DEST_KEY_ID;
            change_dest.id.key = change_kid;
            struct script change_script;
            script_for_destination(&change_script, &change_dest);
            wtx.tx.vout[num_t_out].value = change;
            wtx.tx.vout[num_t_out].script_pub_key = change_script;
        }
    }

    /* value_balance = -(sum of shielded outputs) for shielding (negative = transparent→shielded) */
    int64_t shielded_total = 0;
    for (size_t i = 0; i < num_z_out; i++)
        shielded_total += z_amounts[i];
    wtx.tx.value_balance = -shielded_total;

    /* Build Sapling output descriptions */
    if (num_z_out > 0) {
        wtx.tx.v_shielded_output = zcl_calloc(num_z_out, sizeof(struct output_description), "shielded_outputs");
        if (!wtx.tx.v_shielded_output) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Allocation failed");
            LOG_FAIL("wallet_shielded", "z_sendmany: calloc shielded_output failed for %zu outputs", num_z_out);
        }
        wtx.tx.num_shielded_output = num_z_out;

        /* Get OVK from sapling keystore */
        uint8_t ovk[32];
        if (ctx->wallet->sapling_keys.num_keys > 0)
            memcpy(ovk, ctx->wallet->sapling_keys.keys[0].xfvk.fvk.ovk, 32);
        else
            GetRandBytes(ovk, 32);

        /* Use Sapling proving context for output proofs + binding sig */
        extern void *zclassic_sapling_proving_ctx_init(void);
        extern bool zclassic_sapling_binding_sig(
            const void *ctx, int64_t valueBalance,
            const unsigned char *sighash, unsigned char *result_out);
        extern void zclassic_sapling_proving_ctx_free(void *);

        void *proving_ctx = zclassic_sapling_proving_ctx_init();
        if (!proving_ctx) {
            transaction_free(&wtx.tx);
            json_set_str(result, "Failed to init proving context");
            LOG_FAIL("wallet_shielded", "z_sendmany: sapling_proving_ctx_init failed (transparent spend path)");
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
                zclassic_sapling_proving_ctx_free(proving_ctx);
                transaction_free(&wtx.tx);
                json_set_str(result, "Failed to build Sapling output");
                LOG_FAIL("wallet_shielded", "z_sendmany: sapling_build_output failed for output %zu", i);
            }
        }

        /* Fill transparent inputs from SQLite model (needed for sighash) */
        for (size_t i = 0; i < num_selected; i++) {
            memcpy(wtx.tx.vin[i].prevout.hash.data, db_selected[i].txid, 32);
            wtx.tx.vin[i].prevout.n = db_selected[i].vout;
            wtx.tx.vin[i].sequence = UINT32_MAX - 1;
        }

        /* Sign transparent inputs */
        zcl_mutex_lock(&ctx->wallet->cs);
        for (size_t i = 0; i < num_selected; i++) {
            /* Reconstruct prevout script from SQLite data */
            struct script prev_script;
            script_init(&prev_script);
            if (db_selected[i].script && db_selected[i].script_len > 0) {
                memcpy(prev_script.data, db_selected[i].script,
                       db_selected[i].script_len);
                prev_script.size = db_selected[i].script_len;
            }

            struct tx_destination prev_dest;
            if (!script_extract_destination(&prev_script, &prev_dest)) {
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Cannot determine input destination");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: script_extract_destination failed for input %zu (t->z path)", i);
            }

            struct privkey skey;
            if (!keystore_get_key(&ctx->wallet->keystore, &prev_dest.id.key, &skey)) {
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Private key not available");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: private key not found for input %zu (t->z path)", i);
            }

            struct pubkey spk;
            privkey_get_pubkey(&skey, &spk);

            uint32_t branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);
            struct sighash_type ht;
            ht.raw = SIGHASH_ALL;
            struct precomputed_tx_data txdata;
            precompute_tx_data(&wtx.tx, &txdata);

            struct uint256 sighash;
            if (!signature_hash(&prev_script, &wtx.tx,
                                (unsigned int)i, ht, db_selected[i].value,
                                branch_id, &txdata, &sighash)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Sighash computation failed");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: signature_hash failed for input %zu (t->z path)", i);
            }

            unsigned char sig[SIGNATURE_SIZE + 1];
            size_t siglen = 0;
            if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Signing failed");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: privkey_sign failed for input %zu (t->z path)", i);
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
        zcl_mutex_unlock(&ctx->wallet->cs);

        /* Compute binding signature using Sapling proving context */
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

        if (!zclassic_sapling_binding_sig(proving_ctx,
                                               wtx.tx.value_balance,
                                               binding_sighash.data,
                                               wtx.tx.binding_sig)) {
            zclassic_sapling_proving_ctx_free(proving_ctx);
            transaction_free(&wtx.tx);
            json_set_str(result, "Binding signature failed");
            LOG_FAIL("wallet_shielded", "z_sendmany: binding_sig failed, value_balance=%lld", (long long)wtx.tx.value_balance);
        }
        zclassic_sapling_proving_ctx_free(proving_ctx);
    } else {
        /* No shielded outputs — just transparent */
        for (size_t i = 0; i < num_selected; i++) {
            memcpy(wtx.tx.vin[i].prevout.hash.data, db_selected[i].txid, 32);
            wtx.tx.vin[i].prevout.n = db_selected[i].vout;
            wtx.tx.vin[i].sequence = UINT32_MAX - 1;
        }

        zcl_mutex_lock(&ctx->wallet->cs);
        for (size_t i = 0; i < num_selected; i++) {
            struct script prev_script;
            script_init(&prev_script);
            if (db_selected[i].script && db_selected[i].script_len > 0) {
                memcpy(prev_script.data, db_selected[i].script,
                       db_selected[i].script_len);
                prev_script.size = db_selected[i].script_len;
            }

            struct tx_destination prev_dest;
            if (!script_extract_destination(&prev_script, &prev_dest)) {
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Cannot determine input destination");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: script_extract_destination failed for input %zu (t->t path)", i);
            }

            struct privkey skey;
            if (!keystore_get_key(&ctx->wallet->keystore, &prev_dest.id.key, &skey)) {
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Private key not available");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: private key not found for input %zu (t->t path)", i);
            }

            struct pubkey spk;
            privkey_get_pubkey(&skey, &spk);
            uint32_t branch_id = consensus_current_epoch_branch_id(height + 1, &cp->consensus);
            struct sighash_type ht;
            ht.raw = SIGHASH_ALL;
            struct precomputed_tx_data txdata;
            precompute_tx_data(&wtx.tx, &txdata);

            struct uint256 sighash;
            signature_hash(&prev_script, &wtx.tx,
                           (unsigned int)i, ht, db_selected[i].value,
                           branch_id, &txdata, &sighash);

            unsigned char sig[SIGNATURE_SIZE + 1];
            size_t siglen = 0;
            if (!privkey_sign(&skey, &sighash, sig, &siglen)) {
                memory_cleanse(skey.vch, 32);
                zcl_mutex_unlock(&ctx->wallet->cs);
                transaction_free(&wtx.tx);
                json_set_str(result, "Signing failed");
                for (size_t j = 0; j < num_selected; j++)
                    db_wallet_utxo_free(&db_selected[j]);
                LOG_FAIL("wallet_shielded", "z_sendmany: privkey_sign failed for input %zu (t->t path)", i);
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
        zcl_mutex_unlock(&ctx->wallet->cs);
    }

    /* Free SQLite UTXO data — no longer needed after building inputs */
    for (size_t i = 0; i < num_selected; i++)
        db_wallet_utxo_free(&db_selected[i]);

    transaction_compute_hash(&wtx.tx);
    wtx.time_received = GetTime();
    wtx.from_me = true;
    wtx.used = true;

    if (!wallet_commit_transaction(ctx->wallet, &wtx, ctx->mempool)) {
        json_set_str(result, "Error committing transaction");
        transaction_free(&wtx.tx);
        LOG_FAIL("wallet_shielded", "z_sendmany: wallet_commit_transaction failed");
    }

    if (wallet_ctx_db_ready(ctx))
        node_db_sync_wallet_tx(ctx->node_db, &wtx.tx, ctx->wallet, 0);

    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx.tx.hash);

    if (ctx->wallet_db) {
        struct zcl_result fr = wallet_sqlite_flush_r(ctx->wallet_db, ctx->wallet);
        if (!fr.ok) {
            LOG_FAIL("wallet", "z_sendmany: post-broadcast flush failed "
                                "(code=%d): %s", fr.code, fr.message);
        }
    }

    char txid[65];
    uint256_get_hex(&wtx.tx.hash, txid);
    json_set_str(result, txid);
    return true;
}

static bool rpc_z_gettotalbalance(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    (void)rpc_permit_int(&p, 0, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_gettotalbalance: invalid params"); }

    ENSURE_WALLET(result);

    /* Transparent balance from SQLite model layer */
    int64_t t_balance = wallet_ctx_db_ready(ctx)
        ? db_wallet_utxo_balance(ctx->node_db)
        : wallet_get_balance(ctx->wallet);

    /* Shielded balance: always from SQLite (authoritative source) */
    int64_t z_balance = 0;
    if (ctx->node_db)
        z_balance = db_sapling_note_balance(ctx->node_db);

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
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "z_listreceivedbyaddress \"address\" ( minconf )\n"
        "\nReturn a list of amounts received by a zaddr.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *addr_str = rpc_require_str(&p, 0, "address");
    int minconf = (int)rpc_permit_int(&p, 1, "minconf", 1);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_listreceivedbyaddress: invalid params"); }

    ENSURE_WALLET(result);

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Not a valid Sapling address");
        LOG_FAIL("wallet_shielded", "z_listreceivedbyaddress: invalid sapling address %s", addr_str);
    }

    json_set_array(result);
    for (size_t i = 0; i < ctx->wallet->num_sapling_notes; i++) {
        const struct sapling_received_note *n = &ctx->wallet->sapling_notes[i];
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
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_exportkey: invalid params"); }

    ENSURE_WALLET(result);

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Invalid Sapling address");
        LOG_FAIL("wallet_shielded", "z_exportkey: invalid sapling address %s", addr_str);
    }

    const struct sapling_key_entry *ke =
        sapling_keystore_find_by_address(&ctx->wallet->sapling_keys, z_d, z_pkd);
    if (!ke) {
        json_set_str(result, "Wallet does not hold spending key for this z-address");
        LOG_FAIL("wallet_shielded", "z_exportkey: spending key not found for address %s", addr_str);
    }

    const struct chain_params *cp = chain_params_get();
    char encoded[512];
    if (!sapling_encode_extended_spending_key(&ke->xsk,
            cp->bech32HRPs[BECH32_SAPLING_EXTENDED_SPEND_KEY],
            encoded, sizeof(encoded))) {
        json_set_str(result, "Failed to encode spending key");
        LOG_FAIL("wallet_shielded", "z_exportkey: sapling_encode_extended_spending_key failed");
    }

    json_set_str(result, encoded);
    memory_cleanse(encoded, sizeof(encoded));
    return true;
}

static bool rpc_z_importkey(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_importkey: invalid params"); }

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
        LOG_FAIL("wallet_shielded", "z_importkey: failed to decode spending key");
    }

    /* Import into keystore */
    if (!sapling_keystore_import_xsk(&ctx->wallet->sapling_keys, &xsk)) {
        memory_cleanse(&xsk, sizeof(xsk));
        if (ignore_existing) {
            json_set_null(result);
            return true;
        }
        json_set_str(result, "Key already exists in wallet");
        LOG_FAIL("wallet_shielded", "z_importkey: key already exists in wallet");
    }
    memory_cleanse(&xsk, sizeof(xsk));

    /* Persist to wallet DB. A failed write on imported material leaves
     * the spending key in memory but not on disk — user cannot recover
     * after restart. Return the error so they know. */
    if (ctx->wallet_db) {
        struct sapling_keystore *sks = &ctx->wallet->sapling_keys;
        if (sks->has_seed) {
            if (!wallet_sqlite_write_sapling_seed(ctx->wallet_db, sks->seed)) {
                json_set_str(result,
                    "Error: failed to persist Sapling seed. Key NOT imported. "
                    "Check getwalletinfo.persistence and node.log.");
                LOG_FAIL("wallet_shielded", "z_importkey: sapling_seed flush failed");
            }
        }
        if (!wallet_sqlite_write_sapling_key(ctx->wallet_db,
                sks->keys[sks->num_keys - 1].child_index,
                &sks->keys[sks->num_keys - 1])) {
            json_set_str(result,
                "Error: failed to persist Sapling key. Key NOT imported. "
                "Check getwalletinfo.persistence and node.log.");
            LOG_FAIL("wallet_shielded", "z_importkey: sapling_key flush failed");
        }
        wallet_backup_service_on_key_change();
    }

    if (do_rescan && ctx->main_state) {
        wallet_rescan(ctx->wallet, &ctx->main_state->chain_active,
                      start_height, -1, ctx->datadir);
    }

    json_set_null(result);
    return true;
}

static bool rpc_z_exportviewingkey(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
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
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_exportviewingkey: invalid params"); }

    ENSURE_WALLET(result);

    uint8_t z_d[11], z_pkd[32];
    if (!sapling_decode_payment_address(addr_str, z_d, z_pkd)) {
        json_set_str(result, "Invalid Sapling address");
        LOG_FAIL("wallet_shielded", "z_exportviewingkey: invalid sapling address %s", addr_str);
    }

    const struct sapling_key_entry *ke =
        sapling_keystore_find_by_address(&ctx->wallet->sapling_keys, z_d, z_pkd);
    if (!ke) {
        json_set_str(result,
            "Wallet does not hold key for this z-address");
        LOG_FAIL("wallet_shielded", "z_exportviewingkey: key not found for address %s", addr_str);
    }

    const struct chain_params *cp = chain_params_get();
    char encoded[512];
    if (!sapling_encode_extended_full_viewing_key(&ke->xfvk,
            cp->bech32HRPs[BECH32_SAPLING_FULL_VIEWING_KEY],
            encoded, sizeof(encoded))) {
        json_set_str(result, "Failed to encode viewing key");
        LOG_FAIL("wallet_shielded", "z_exportviewingkey: sapling_encode_extended_full_viewing_key failed");
    }

    json_set_str(result, encoded);
    return true;
}

static bool rpc_z_getmemo(const struct json_value *params, bool help,
                            struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "z_getmemo \"txid\" ( outindex )\n"
        "\nReturns the memo attached to a shielded note.\n"
        "\nArguments:\n"
        "1. \"txid\"      (string, required) Transaction ID\n"
        "2. outindex    (numeric, optional, default=0) Output index\n"
        "\nResult:\n"
        "{\n"
        "  \"txid\": \"hex\",\n"
        "  \"outindex\": n,\n"
        "  \"memo\": \"text or hex\",\n"
        "  \"memo_hex\": \"raw hex\",\n"
        "  \"memo_bytes\": n\n"
        "}\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *txid_str = rpc_require_str(&p, 0, "txid");
    int outindex = (int)rpc_permit_int(&p, 1, "outindex", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); LOG_FAIL("wallet_shielded", "z_getmemo: invalid params"); }

    ENSURE_WALLET(result);

    if (!ctx->node_db) {
        json_set_str(result, "Database not available");
        LOG_FAIL("wallet_shielded", "z_getmemo: node_db is NULL");
    }

    uint8_t txid[32];
    if (!zcl_is_hex_string(txid_str, 64)) {
        json_set_str(result, "Invalid txid (expected 64-char hex)");
        LOG_FAIL("wallet_shielded", "z_getmemo: txid not 64 hex chars: '%s'",
                 txid_str ? txid_str : "(null)");
    }
    for (int i = 0; i < 32; i++) {
        unsigned int b;
        sscanf(txid_str + (31 - i) * 2, "%2x", &b);
        txid[i] = (uint8_t)b;
    }

    struct db_sapling_note notes[16];
    int count = db_wallet_tx_notes(ctx->node_db, txid, notes, 16);
    if (count <= 0) {
        json_set_str(result, "No shielded notes found for this txid");
        LOG_FAIL("wallet_shielded", "z_getmemo: no notes found for txid=%s", txid_str);
    }

    struct db_sapling_note *found = NULL;
    for (int i = 0; i < count; i++) {
        if ((int)notes[i].output_index == outindex) {
            found = &notes[i];
            break;
        }
    }
    if (!found) {
        json_set_str(result, "No note at specified output index");
        LOG_FAIL("wallet_shielded", "z_getmemo: no note at outindex=%d for txid=%s", outindex, txid_str);
    }

    json_set_object(result);
    json_push_kv_str(result, "txid", txid_str);
    json_push_kv_int(result, "outindex", outindex);

    /* Find meaningful memo bytes (strip trailing 0xf6 padding and zeroes) */
    size_t memo_end = 0;
    bool has_content = false;
    for (size_t j = 0; j < found->memo_len && j < 512; j++) {
        if (found->memo[j] != 0 && found->memo[j] != 0xf6) {
            memo_end = j + 1;
            has_content = true;
        }
    }

    if (has_content) {
        /* Text representation if printable */
        if (found->memo[0] >= 0x20 && found->memo[0] < 0x7f) {
            size_t len = 0;
            while (len < memo_end && found->memo[len] >= 0x20)
                len++;
            char memo_str[513];
            memcpy(memo_str, found->memo, len);
            memo_str[len] = '\0';
            json_push_kv_str(result, "memo", memo_str);
        }

        /* Always include hex */
        char hex[1025];
        for (size_t j = 0; j < memo_end; j++)
            snprintf(hex + j * 2, 3, "%02x", found->memo[j]);
        hex[memo_end * 2] = '\0';
        json_push_kv_str(result, "memo_hex", hex);
        json_push_kv_int(result, "memo_bytes", (int64_t)memo_end);
    } else {
        json_push_kv_str(result, "memo", "(empty)");
        json_push_kv_int(result, "memo_bytes", 0);
    }

    return true;
}

/* z_listallnotes: list all shielded notes (spent + unspent) with memos */
static bool rpc_z_listallnotes(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    struct wallet_rpc_context *ctx = wallet_ctx();
    RPC_HELP(help, result, "z_listallnotes\n"
        "\nReturns all shielded notes (spent and unspent) with memos.\n");

    (void)params;
    ENSURE_WALLET(result);

    if (!ctx->node_db) {
        json_set_str(result, "Database not available");
        LOG_FAIL("wallet_shielded", "z_listallnotes: node_db is NULL");
    }

    struct db_sapling_note notes[512];
    int count = db_sapling_note_list_all(ctx->node_db, notes, 512);

    int chain_h = 0;
    if (ctx->main_state)
        chain_h = active_chain_height(&ctx->main_state->chain_active);

    json_set_array(result);
    for (int i = 0; i < count; i++) {
        struct db_sapling_note *n = &notes[i];

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

        int confirms = chain_h > 0 ? chain_h - n->block_height + 1 : 0;
        json_push_kv_int(&entry, "confirmations", (int64_t)confirms);
        json_push_kv_int(&entry, "block_height", (int64_t)n->block_height);
        json_push_kv_bool(&entry, "spent", n->is_spent);

        if (n->is_spent) {
            char spent_hex[65];
            for (int j = 0; j < 32; j++)
                snprintf(spent_hex + j * 2, 3, "%02x", n->spent_txid[31 - j]);
            json_push_kv_str(&entry, "spent_by", spent_hex);
        }

        /* Memo */
        bool has_memo = false;
        size_t memo_end = 0;
        for (size_t j = 0; j < n->memo_len && j < 512; j++) {
            if (n->memo[j] != 0 && n->memo[j] != 0xf6) {
                memo_end = j + 1;
                has_memo = true;
            }
        }
        if (has_memo) {
            if (n->memo[0] >= 0x20 && n->memo[0] < 0x7f) {
                size_t len = 0;
                while (len < memo_end && n->memo[len] >= 0x20)
                    len++;
                char memo_str[513];
                memcpy(memo_str, n->memo, len);
                memo_str[len] = '\0';
                json_push_kv_str(&entry, "memo", memo_str);
            } else {
                char hex[1025];
                for (size_t j = 0; j < memo_end; j++)
                    snprintf(hex + j * 2, 3, "%02x", n->memo[j]);
                hex[memo_end * 2] = '\0';
                json_push_kv_str(&entry, "memo_hex", hex);
            }
        }

        json_push_back(result, &entry);
        json_free(&entry);
    }
    return true;
}

void register_wallet_shielded_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "wallet", "z_getnewaddress",     rpc_z_getnewaddress,      false },
        { "wallet", "z_listaddresses",     rpc_z_listaddresses,      false },
        { "wallet", "z_sendmany",          rpc_z_sendmany,           false },
        { "wallet", "z_getbalance",        rpc_z_getbalance,         false },
        { "wallet", "z_gettotalbalance",   rpc_z_gettotalbalance,    false },
        { "wallet", "z_listunspent",       rpc_z_listunspent,        false },
        { "wallet", "z_listreceivedbyaddress", rpc_z_listreceivedbyaddress, false },
        { "wallet", "z_exportkey",         rpc_z_exportkey,          false },
        { "wallet", "z_importkey",         rpc_z_importkey,          false },
        { "wallet", "z_exportviewingkey",  rpc_z_exportviewingkey,   false },
        { "wallet", "z_getmemo",           rpc_z_getmemo,            false },
        { "wallet", "z_listallnotes",      rpc_z_listallnotes,       false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
