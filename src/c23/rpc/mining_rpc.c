/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/mining_rpc.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "core/core_io.h"
#include "core/serialize.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "mining/miner.h"
#include "primitives/block.h"
#include "script/script.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include <stdlib.h>
#include <string.h>

static struct main_state *g_ms = NULL;
static struct tx_mempool *g_mp = NULL;
static struct coins_view_cache *g_coins = NULL;
static const char *g_datadir = NULL;

void rpc_mining_set_state(struct main_state *ms, struct tx_mempool *mp,
                           struct coins_view_cache *coins_tip,
                           const char *datadir)
{
    g_ms = ms;
    g_mp = mp;
    g_coins = coins_tip;
    g_datadir = datadir;
}

static bool rpc_getmininginfo(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result,
            "getmininginfo\n"
            "Returns mining-related information.");
        return true;
    }

    const struct chain_params *cp = chain_params_get();
    struct block_index *tip = active_chain_tip(&g_ms->chain_active);

    json_set_object(result);
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    json_push_kv_int(result, "currentblocksize", (int64_t)g_ms->nLastBlockSize);
    json_push_kv_int(result, "currentblocktx", (int64_t)g_ms->nLastBlockTx);

    double difficulty = 0.0;
    if (tip) {
        int shift = (tip->nBits >> 24) & 0xff;
        double diff = (double)(0x0000ff & (tip->nBits >> 16));
        while (shift < 29) { diff *= 256.0; shift++; }
        while (shift > 29) { diff /= 256.0; shift--; }
        if (diff != 0.0)
            difficulty = (double)0x00ffff / diff;
    }
    json_push_kv_real(result, "difficulty", difficulty);

    json_push_kv_str(result, "chain", cp->strNetworkID);
    json_push_kv_bool(result, "generate", false);

    return true;
}

static bool rpc_generate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "generate numblocks\n"
            "Mine blocks immediately (regtest only).\n"
            "Arguments:\n"
            "1. numblocks (numeric, required) How many blocks to generate");
        return true;
    }

    int64_t num_blocks = json_get_int(json_at(params, 0));
    if (num_blocks <= 0 || num_blocks > 1000) {
        json_set_str(result, "Invalid number of blocks");
        return false;
    }

    const struct chain_params *cp = chain_params_get();

    struct script coinbase_script;
    coinbase_script.size = 0;

    json_set_array(result);

    for (int64_t i = 0; i < num_blocks; i++) {
        struct block_template *tmpl = create_new_block(
            &coinbase_script, g_ms, g_coins, g_mp, cp);
        if (!tmpl) break;

        struct block_index *tip = active_chain_tip(&g_ms->chain_active);
        unsigned int extra_nonce = 0;
        increment_extra_nonce(&tmpl->block, tip, &extra_nonce);

        if (process_block_found(&tmpl->block, g_ms, g_coins, cp, g_datadir)) {
            struct uint256 hash;
            block_get_hash(&tmpl->block, &hash);
            char hex[65];
            uint256_get_hex(&hash, hex);
            struct json_value v;
            json_set_str(&v, hex);
            json_push_back(result, &v);
            json_free(&v);
        }

        block_template_free(tmpl);
        free(tmpl);
    }

    return true;
}

static bool rpc_submitblock(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result,
            "submitblock \"hexdata\"\n"
            "Attempts to submit new block to network.\n"
            "Arguments:\n"
            "1. \"hexdata\" (string, required) The hex-encoded block data");
        return true;
    }

    const struct json_value *hex_val = json_at(params, 0);
    if (!hex_val || hex_val->type != JSON_STR) {
        json_set_str(result, "Invalid hex data");
        return false;
    }

    const char *hex = json_get_str(hex_val);
    size_t hex_len = strlen(hex);
    size_t bin_len = hex_len / 2;
    unsigned char *bin = malloc(bin_len);
    if (!bin) return false;

    size_t parsed = ParseHex(hex, bin, bin_len);
    if (parsed == 0) {
        free(bin);
        json_set_str(result, "Block decode failed");
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, bin, parsed);

    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, &s)) {
        block_free(&blk);
        stream_free(&s);
        free(bin);
        json_set_str(result, "Block decode failed");
        return false;
    }
    stream_free(&s);
    free(bin);

    const struct chain_params *cp = chain_params_get();
    struct validation_state state;
    validation_state_init(&state);

    bool ok = process_new_block(&state, g_ms, g_coins, cp, &blk,
                                 true, g_datadir);
    block_free(&blk);

    if (!ok) {
        char msg[512];
        format_state_message(&state, msg, sizeof(msg));
        if (msg[0])
            json_set_str(result, msg);
        else
            json_set_str(result, "rejected");
        return false;
    }

    json_set_null(result);
    return true;
}

void register_mining_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "mining", "getmininginfo", rpc_getmininginfo, true },
        { "mining", "generate",      rpc_generate,      true },
        { "mining", "submitblock",   rpc_submitblock,    true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
