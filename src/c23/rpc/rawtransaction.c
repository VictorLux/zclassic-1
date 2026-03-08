/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/rawtransaction.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "encoding/utilstrencodings.h"
#include "keys/key_io.h"
#include "json/json.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "validation/check_transaction.h"
#include "validation/chainstate.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static const char *g_datadir = NULL;

void rpc_rawtx_set_state(struct main_state *ms, struct tx_mempool *mp,
                          struct coins_view_cache *coins_tip,
                          const char *datadir)
{
    g_ms = ms;
    g_mp = mp;
    g_coins_tip = coins_tip;
    g_datadir = datadir;
}

static bool rpc_getrawtransaction(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "getrawtransaction \"txid\" ( verbose )\n"
            "Return the raw transaction data.\n"
            "Arguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "2. verbose   (numeric, optional, default=0) "
            "If 0, return hex; if 1, return JSON object");
        return true;
    }

    const struct json_value *txid_val = json_at(params, 0);
    if (!txid_val || txid_val->type != JSON_STR) {
        json_set_str(result, "Invalid txid");
        return false;
    }

    struct uint256 hash;
    if (!parse_hash_str(json_get_str(txid_val), &hash)) {
        json_set_str(result, "Invalid txid format");
        return false;
    }

    int verbose = 0;
    if (json_size(params) >= 2) {
        const struct json_value *v = json_at(params, 1);
        if (v) verbose = (int)json_get_int(v);
    }

    struct transaction tx;
    transaction_init(&tx);
    struct uint256 hash_block;
    uint256_set_null(&hash_block);
    bool found = false;

    if (g_mp && tx_mempool_lookup(g_mp, &hash, &tx)) {
        found = true;
    }

    if (!found) {
        transaction_free(&tx);
        json_set_str(result, "Transaction not found");
        return false;
    }

    if (verbose == 0) {
        char *hex = malloc(2 * 1024 * 1024);
        if (!hex) {
            transaction_free(&tx);
            return false;
        }
        size_t hex_len = encode_hex_tx(&tx, hex, 2 * 1024 * 1024);
        hex[hex_len] = '\0';
        json_set_str(result, hex);
        free(hex);
    } else {
        tx_to_json(&tx, &hash_block, result);
    }

    transaction_free(&tx);
    return true;
}

static bool rpc_decoderawtransaction(const struct json_value *params, bool help,
                                      struct json_value *result)
{
    if (help || json_size(params) != 1) {
        json_set_str(result,
            "decoderawtransaction \"hexstring\"\n"
            "Return a JSON object representing the serialized transaction.\n"
            "Arguments:\n"
            "1. \"hexstring\" (string, required) The transaction hex string");
        return true;
    }

    const struct json_value *hex_val = json_at(params, 0);
    if (!hex_val || hex_val->type != JSON_STR) {
        json_set_str(result, "Invalid hex string");
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);
    if (!decode_hex_tx(&tx, json_get_str(hex_val))) {
        transaction_free(&tx);
        json_set_str(result, "TX decode failed");
        return false;
    }

    struct uint256 null_hash;
    uint256_set_null(&null_hash);
    tx_to_json(&tx, &null_hash, result);
    transaction_free(&tx);
    return true;
}

static bool rpc_sendrawtransaction(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
            "Submits raw transaction to local node and network.\n"
            "Arguments:\n"
            "1. \"hexstring\" (string, required) The hex string of the raw tx\n"
            "2. allowhighfees (boolean, optional, default=false)");
        return true;
    }

    const struct json_value *hex_val = json_at(params, 0);
    if (!hex_val || hex_val->type != JSON_STR) {
        json_set_str(result, "Invalid hex string");
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);
    if (!decode_hex_tx(&tx, json_get_str(hex_val))) {
        transaction_free(&tx);
        json_set_str(result, "TX decode failed");
        return false;
    }

    transaction_compute_hash(&tx);
    struct uint256 hash = tx.hash;

    if (g_mp && tx_mempool_exists(g_mp, &hash)) {
        char hex[65];
        uint256_get_hex(&hash, hex);
        json_set_str(result, hex);
        transaction_free(&tx);
        return true;
    }

    struct validation_state state;
    validation_state_init(&state);

    if (!check_transaction(&tx, &state)) {
        char msg[512];
        format_state_message(&state, msg, sizeof(msg));
        json_set_str(result, msg);
        transaction_free(&tx);
        return false;
    }

    if (g_mp) {
        int tip_height = active_chain_height(&g_ms->chain_active);
        uint32_t branch_id = consensus_current_epoch_branch_id(
            tip_height + 1, &chain_params_get()->consensus);

        struct mempool_entry entry;
        mempool_entry_init(&entry, &tx, 0, (int64_t)time(NULL), 0.0,
                           (unsigned int)(tip_height + 1),
                           tx_mempool_has_no_inputs_of(g_mp, &tx),
                           false, branch_id);

        if (!tx_mempool_add_unchecked(g_mp, &hash, &entry)) {
            mempool_entry_free(&entry);
            json_set_str(result, "Failed to add to mempool");
            transaction_free(&tx);
            return false;
        }
    }

    char hex[65];
    uint256_get_hex(&hash, hex);
    json_set_str(result, hex);
    transaction_free(&tx);
    return true;
}

static bool rpc_createrawtransaction(const struct json_value *params, bool help,
                                      struct json_value *result)
{
    if (help || json_size(params) < 2) {
        json_set_str(result,
            "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] "
            "{\"address\":amount,...}\n"
            "Create a transaction spending the given inputs.\n"
            "Arguments:\n"
            "1. \"inputs\"  (array, required) JSON array of inputs\n"
            "2. \"outputs\" (object, required) JSON object of outputs");
        return true;
    }

    const struct json_value *inputs = json_at(params, 0);
    const struct json_value *outputs = json_at(params, 1);

    if (!inputs || inputs->type != JSON_ARR ||
        !outputs || outputs->type != JSON_OBJ) {
        json_set_str(result, "Invalid parameters");
        return false;
    }

    struct transaction tx;
    transaction_init(&tx);

    int tip_height = g_ms ?
        active_chain_height(&g_ms->chain_active) : 0;
    const struct consensus_params *cp = &chain_params_get()->consensus;
    int epoch = consensus_current_epoch(tip_height + 1, cp);

    if (epoch >= (int)UPGRADE_SAPLING) {
        tx.version = SAPLING_TX_VERSION;
        tx.version_group_id = SAPLING_VERSION_GROUP_ID;
    } else if (epoch >= (int)UPGRADE_OVERWINTER) {
        tx.version = OVERWINTER_TX_VERSION;
        tx.version_group_id = OVERWINTER_VERSION_GROUP_ID;
    } else {
        tx.version = 1;
    }

    for (size_t i = 0; i < json_size(inputs); i++) {
        const struct json_value *inp = json_at(inputs, i);
        if (!inp || inp->type != JSON_OBJ) continue;

        const struct json_value *txid_v = json_get(inp, "txid");
        const struct json_value *vout_v = json_get(inp, "vout");
        if (!txid_v || !vout_v) continue;

        struct tx_in vin;
        tx_in_init(&vin);
        parse_hash_str(json_get_str(txid_v), &vin.prevout.hash);
        vin.prevout.n = (uint32_t)json_get_int(vout_v);

        const struct json_value *seq_v = json_get(inp, "sequence");
        if (seq_v) vin.sequence = (uint32_t)json_get_int(seq_v);

        size_t new_count = tx.num_vin + 1;
        struct tx_in *new_vin = realloc(tx.vin, new_count * sizeof(struct tx_in));
        if (!new_vin) { transaction_free(&tx); return false; }
        tx.vin = new_vin;
        tx.vin[tx.num_vin] = vin;
        tx.num_vin = new_count;
    }

    for (size_t i = 0; i < json_size(outputs); i++) {
        if (!outputs->keys || !outputs->keys[i]) continue;
        const char *addr = outputs->keys[i];
        const struct json_value *amt_v = &outputs->children[i];

        struct tx_out vout;
        tx_out_set_null(&vout);

        int64_t amount = 0;
        if (amt_v->type == JSON_REAL)
            amount = (int64_t)(json_get_real(amt_v) * 100000000.0);
        else if (amt_v->type == JSON_INT)
            amount = json_get_int(amt_v) * 100000000;
        vout.value = amount;

        const struct chain_params *cp2 = chain_params_get();
        size_t pk_len, sc_len;
        const unsigned char *pk_pfx = chain_params_base58_prefix(
            cp2, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *sc_pfx = chain_params_base58_prefix(
            cp2, B58_SCRIPT_ADDRESS, &sc_len);
        struct tx_destination dest;
        if (decode_destination(addr, pk_pfx, pk_len, sc_pfx, sc_len, &dest)) {
            script_for_destination(&vout.script_pub_key, &dest);
        }

        size_t new_count = tx.num_vout + 1;
        struct tx_out *new_vout = realloc(tx.vout,
                                          new_count * sizeof(struct tx_out));
        if (!new_vout) { transaction_free(&tx); return false; }
        tx.vout = new_vout;
        tx.vout[tx.num_vout] = vout;
        tx.num_vout = new_count;
    }

    char *hex = malloc(2 * 1024 * 1024);
    if (!hex) { transaction_free(&tx); return false; }
    size_t hex_len = encode_hex_tx(&tx, hex, 2 * 1024 * 1024);
    hex[hex_len] = '\0';
    json_set_str(result, hex);
    free(hex);
    transaction_free(&tx);
    return true;
}

void register_rawtransaction_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "rawtransactions", "getrawtransaction",
          rpc_getrawtransaction, true },
        { "rawtransactions", "decoderawtransaction",
          rpc_decoderawtransaction, true },
        { "rawtransactions", "sendrawtransaction",
          rpc_sendrawtransaction, false },
        { "rawtransactions", "createrawtransaction",
          rpc_createrawtransaction, false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
