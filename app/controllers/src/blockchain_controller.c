/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/blockchain_controller.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "json/json.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include <string.h>
#include <math.h>

static struct main_state *g_main_state = NULL;
static struct tx_mempool *g_mempool = NULL;
static const char *g_datadir = NULL;

void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir)
{
    g_main_state = ms;
    g_mempool = mp;
    g_datadir = datadir;
}

static double get_difficulty(const struct block_index *bi)
{
    if (!bi)
        return 1.0;
    int shift = (int)((bi->nBits >> 24) & 0xff) - 29;
    double diff = (double)0x0000ffff / (double)(bi->nBits & 0x00ffffff);
    while (shift < 0) {
        diff *= 256.0;
        shift++;
    }
    while (shift > 0) {
        diff /= 256.0;
        shift--;
    }
    return diff;
}

static bool rpc_getblockcount(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result, "getblockcount\nReturns the number of blocks.");
        return true;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
    json_set_int(result, active_chain_height(&g_main_state->chain_active));
    return true;
}

static bool rpc_getbestblockhash(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result, "getbestblockhash\nReturns the hash of the best block.");
        return true;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
    struct block_index *tip = active_chain_tip(&g_main_state->chain_active);
    if (!tip || !tip->phashBlock) {
        json_set_str(result, "No tip");
        return false;
    }
    char hex[65];
    uint256_get_hex(tip->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

static bool rpc_getdifficulty(const struct json_value *params, bool help,
                               struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result, "getdifficulty\nReturns proof-of-work difficulty.");
        return true;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
    struct block_index *tip = active_chain_tip(&g_main_state->chain_active);
    json_set_real(result, get_difficulty(tip));
    return true;
}

static bool rpc_getblockhash(const struct json_value *params, bool help,
                              struct json_value *result)
{
    if (help || json_size(params) != 1) {
        json_set_str(result, "getblockhash height\nReturns hash of block at height.");
        return help;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
    int height = (int)json_get_int(json_at(params, 0));
    struct block_index *bi = active_chain_at(&g_main_state->chain_active, height);
    if (!bi || !bi->phashBlock) {
        json_set_str(result, "Block height out of range");
        return false;
    }
    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_set_str(result, hex);
    return true;
}

static void block_header_to_json(const struct block_index *bi,
                                  struct json_value *result)
{
    json_set_object(result);
    if (!bi || !bi->phashBlock)
        return;

    char hex[65];
    uint256_get_hex(bi->phashBlock, hex);
    json_push_kv_str(result, "hash", hex);
    json_push_kv_int(result, "confirmations", 1);
    json_push_kv_int(result, "height", bi->nHeight);
    json_push_kv_int(result, "version", bi->nVersion);

    uint256_get_hex(&bi->hashMerkleRoot, hex);
    json_push_kv_str(result, "merkleroot", hex);

    json_push_kv_int(result, "time", (int64_t)bi->nTime);
    json_push_kv_int(result, "nonce", 0);

    char bits_hex[9];
    snprintf(bits_hex, sizeof(bits_hex), "%08x", bi->nBits);
    json_push_kv_str(result, "bits", bits_hex);

    json_push_kv_real(result, "difficulty", get_difficulty(bi));

    if (bi->pprev && bi->pprev->phashBlock) {
        uint256_get_hex(bi->pprev->phashBlock, hex);
        json_push_kv_str(result, "previousblockhash", hex);
    }
}

static bool rpc_getblockheader(const struct json_value *params, bool help,
                                struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result, "getblockheader \"hash\" ( verbose )\nReturns block header.");
        return help;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }

    const char *hash_str = json_get_str(json_at(params, 0));
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&g_main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        return false;
    }

    block_header_to_json(bi, result);
    return true;
}

static bool rpc_getblock(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help || json_size(params) < 1) {
        json_set_str(result, "getblock \"hash\" ( verbose )\nReturns block data.");
        return help;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }

    const char *hash_str = json_get_str(json_at(params, 0));
    struct uint256 hash;
    uint256_set_hex(&hash, hash_str);

    struct block_index *bi = block_map_find(&g_main_state->map_block_index, &hash);
    if (!bi) {
        json_set_str(result, "Block not found");
        return false;
    }

    block_header_to_json(bi, result);

    json_push_kv_int(result, "size", 0);
    json_push_kv_int(result, "tx", (int64_t)bi->nTx);

    return true;
}

static bool rpc_getblockchaininfo(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result, "getblockchaininfo\nReturns blockchain state info.");
        return true;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }

    const struct chain_params *cp = chain_params_get();

    json_set_object(result);
    json_push_kv_str(result, "chain", cp->strNetworkID);

    struct block_index *tip = active_chain_tip(&g_main_state->chain_active);
    json_push_kv_int(result, "blocks", tip ? tip->nHeight : 0);
    json_push_kv_int(result, "headers", tip ? tip->nHeight : 0);

    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        json_push_kv_str(result, "bestblockhash", hex);
    }

    json_push_kv_real(result, "difficulty", get_difficulty(tip));
    json_push_kv_real(result, "verificationprogress", 1.0);

    /* Upgrades */
    struct json_value upgrades;
    json_set_object(&upgrades);
    json_push_kv(result, "upgrades", &upgrades);
    json_free(&upgrades);

    return true;
}

static bool rpc_getmempoolinfo(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    if (help) {
        json_set_str(result, "getmempoolinfo\nReturns mempool state.");
        return true;
    }

    json_set_object(result);
    json_push_kv_int(result, "size",
                     g_mempool ? (int64_t)g_mempool->num_entries : 0);
    json_push_kv_int(result, "bytes",
                     g_mempool ? (int64_t)g_mempool->total_tx_size : 0);
    return true;
}

void register_blockchain_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "getblockcount",     rpc_getblockcount,     true },
        { "blockchain", "getbestblockhash",  rpc_getbestblockhash,  true },
        { "blockchain", "getdifficulty",     rpc_getdifficulty,     true },
        { "blockchain", "getblockhash",      rpc_getblockhash,      true },
        { "blockchain", "getblockheader",    rpc_getblockheader,    true },
        { "blockchain", "getblock",          rpc_getblock,          true },
        { "blockchain", "getblockchaininfo", rpc_getblockchaininfo, true },
        { "blockchain", "getmempoolinfo",    rpc_getmempoolinfo,    true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
