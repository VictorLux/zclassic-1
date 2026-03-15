/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/blockchain_controller.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/undo.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "json/json.h"
#include "primitives/block.h"
#include "storage/coins_db.h"
#include "storage/dbwrapper.h"
#include "storage/disk_block_io.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "validation/update_coins.h"
#include "validation/connect_block.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

static struct main_state *g_main_state = NULL;
static struct tx_mempool *g_mempool = NULL;
static const char *g_datadir = NULL;
static struct coins_view_db *g_coins_db = NULL;
static struct coins_view_cache *g_coins_tip = NULL;

void rpc_blockchain_set_state(struct main_state *ms, struct tx_mempool *mp,
                               const char *datadir)
{
    g_main_state = ms;
    g_mempool = mp;
    g_datadir = datadir;
}

void rpc_blockchain_set_coins_db(struct coins_view_db *cvdb,
                                  struct coins_view_cache *coins_tip)
{
    g_coins_db = cvdb;
    g_coins_tip = coins_tip;
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
    RPC_HELP(help, result, "getblockcount\nReturns the number of blocks.");
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
    RPC_HELP(help, result, "getbestblockhash\nReturns the hash of the best block.");
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
    RPC_HELP(help, result, "getdifficulty\nReturns proof-of-work difficulty.");
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
    RPC_HELP(help, result, "getblockhash height\nReturns hash of block at height.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    int height = (int)rpc_require_int(&p, 0, "height");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
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
    RPC_HELP(help, result,
             "getblockheader \"hash\" ( verbose )\nReturns block header.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
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
    RPC_HELP(help, result,
             "getblock \"hash\" ( verbose )\nReturns block data.");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 2);
    const char *hash_str = rpc_require_str(&p, 0, "hash");
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }
    if (!g_main_state) {
        json_set_str(result, "Not initialized");
        return false;
    }
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
    RPC_HELP(help, result, "getblockchaininfo\nReturns blockchain state info.");
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
    struct json_value upgrades = {0};
    json_set_object(&upgrades);
    json_push_kv(result, "upgrades", &upgrades);
    json_free(&upgrades);

    return true;
}

static bool rpc_getmempoolinfo(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result, "getmempoolinfo\nReturns mempool state.");

    json_set_object(result);
    json_push_kv_int(result, "size",
                     g_mempool ? (int64_t)g_mempool->num_entries : 0);
    json_push_kv_int(result, "bytes",
                     g_mempool ? (int64_t)g_mempool->total_tx_size : 0);
    return true;
}

/* gettxoutsetinfo: UTXO set statistics matching legacy node output. */
static bool rpc_gettxoutsetinfo(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "gettxoutsetinfo\n"
        "\nReturns statistics about the UTXO set.\n");

    if (!g_coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush in-memory UTXO cache to LevelDB for accurate totals */
    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    int tip_height = active_chain_height(&g_main_state->chain_active);
    struct block_index *tip = active_chain_tip(&g_main_state->chain_active);

    int64_t total_amount = 0;
    int64_t num_txs = 0;
    int64_t num_txouts = 0;

    struct db_iterator it;
    db_iter_init(&it, &g_coins_db->db);
    char prefix = 'c';
    db_iter_seek(&it, &prefix, 1);

    while (db_iter_valid(&it)) {
        size_t keylen = 0;
        const char *key = db_iter_key(&it, &keylen);
        if (!key || keylen != 33 || key[0] != 'c')
            break;

        struct uint256 txid;
        memcpy(txid.data, key + 1, 32);

        struct coins c;
        coins_init(&c);
        if (coins_view_db_get_coins(g_coins_db, &txid, &c)) {
            num_txs++;
            for (size_t i = 0; i < c.num_vout; i++) {
                if (!tx_out_is_null(&c.vout[i])) {
                    total_amount += c.vout[i].value;
                    num_txouts++;
                }
            }
        }
        coins_free(&c);
        db_iter_next(&it);
    }
    db_iter_free(&it);

    json_set_object(result);
    json_push_kv_int(result, "height", tip_height);
    if (tip && tip->phashBlock) {
        char hex[65];
        uint256_get_hex(tip->phashBlock, hex);
        json_push_kv_str(result, "bestblock", hex);
    }
    json_push_kv_int(result, "transactions", num_txs);
    json_push_kv_int(result, "txouts", num_txouts);

    char amt[32];
    snprintf(amt, sizeof(amt), "%lld.%08lld",
             (long long)(total_amount / 100000000),
             (long long)(total_amount % 100000000));
    json_push_kv_str(result, "total_amount", amt);

    return true;
}

/* HODL Wave: UTXO age distribution across the entire UTXO set.
 * Uses coins_view_db_get_coins for correct deserialization. */
static bool rpc_gethodlwave(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "gethodlwave\n"
        "\nScans the entire UTXO set and reports value distribution by age.\n"
        "Inspired by Unchained Capital's Bitcoin HODL Waves analysis.\n"
        "\nBuckets: <1d, 1d-1w, 1w-1m, 1-3m, 3-6m, 6-12m, 1-2y, 2-3y, 3-5y, >5y\n"
        "\nResult: { buckets: [{label, value, pct, utxo_count}], summary }\n");

    if (!g_coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush in-memory UTXO cache to LevelDB for accurate totals */
    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    int tip_height = active_chain_height(&g_main_state->chain_active);

    /* ZClassic: ~75 second block time */
    #define BLOCKS_PER_DAY    1152
    #define BLOCKS_PER_WEEK   (BLOCKS_PER_DAY * 7)
    #define BLOCKS_PER_MONTH  (BLOCKS_PER_DAY * 30)
    #define NUM_HODL_BUCKETS  10

    struct {
        const char *label;
        int max_age_blocks;  /* 0 = unlimited */
    } buckets[NUM_HODL_BUCKETS] = {
        { "< 1 day",    BLOCKS_PER_DAY },
        { "1d - 1w",    BLOCKS_PER_WEEK },
        { "1w - 1m",    BLOCKS_PER_MONTH },
        { "1 - 3m",     BLOCKS_PER_MONTH * 3 },
        { "3 - 6m",     BLOCKS_PER_MONTH * 6 },
        { "6 - 12m",    BLOCKS_PER_MONTH * 12 },
        { "1 - 2y",     BLOCKS_PER_MONTH * 24 },
        { "2 - 3y",     BLOCKS_PER_MONTH * 36 },
        { "3 - 5y",     BLOCKS_PER_MONTH * 60 },
        { "> 5y",       0 },
    };

    int64_t bucket_values[NUM_HODL_BUCKETS] = {0};
    int64_t bucket_counts[NUM_HODL_BUCKETS] = {0};
    int64_t total_value = 0;
    int64_t total_utxos = 0;
    int64_t total_txs = 0;
    int64_t skipped = 0;

    struct db_iterator it;
    db_iter_init(&it, &g_coins_db->db);
    char prefix = 'c';
    db_iter_seek(&it, &prefix, 1);

    while (db_iter_valid(&it)) {
        size_t keylen = 0;
        const char *key = db_iter_key(&it, &keylen);
        if (!key || keylen != 33 || key[0] != 'c')
            break;

        struct uint256 txid;
        memcpy(txid.data, key + 1, 32);

        struct coins c;
        coins_init(&c);
        if (!coins_view_db_get_coins(g_coins_db, &txid, &c)) {
            skipped++;
            coins_free(&c);
            db_iter_next(&it);
            continue;
        }

        int height = c.height;
        if (height < 0 || height > tip_height + 100) {
            skipped++;
            coins_free(&c);
            db_iter_next(&it);
            continue;
        }

        int age = tip_height - height;
        int bucket_idx = NUM_HODL_BUCKETS - 1;
        for (int b = 0; b < NUM_HODL_BUCKETS - 1; b++) {
            if (age < buckets[b].max_age_blocks) {
                bucket_idx = b;
                break;
            }
        }

        for (size_t i = 0; i < c.num_vout; i++) {
            if (!tx_out_is_null(&c.vout[i])) {
                bucket_values[bucket_idx] += c.vout[i].value;
                bucket_counts[bucket_idx]++;
            }
        }

        coins_free(&c);
        total_txs++;
        db_iter_next(&it);
    }
    db_iter_free(&it);

    for (int b = 0; b < NUM_HODL_BUCKETS; b++) {
        total_value += bucket_values[b];
        total_utxos += bucket_counts[b];
    }

    json_set_object(result);
    json_push_kv_int(result, "tip_height", tip_height);
    json_push_kv_int(result, "total_transactions", total_txs);
    json_push_kv_int(result, "total_utxos", total_utxos);
    if (skipped > 0)
        json_push_kv_int(result, "skipped_entries", skipped);

    char amt[32];
    snprintf(amt, sizeof(amt), "%lld.%08lld",
             (long long)(total_value / 100000000),
             (long long)(total_value % 100000000));
    json_push_kv_str(result, "total_value", amt);

    struct json_value arr = {0};
    json_set_array(&arr);

    for (int b = 0; b < NUM_HODL_BUCKETS; b++) {
        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "label", buckets[b].label);

        snprintf(amt, sizeof(amt), "%lld.%08lld",
                 (long long)(bucket_values[b] / 100000000),
                 (long long)(bucket_values[b] % 100000000));
        json_push_kv_str(&entry, "value", amt);

        double pct = total_value > 0
                   ? (double)bucket_values[b] / (double)total_value * 100.0
                   : 0.0;
        json_push_kv_real(&entry, "percent", pct);
        json_push_kv_int(&entry, "utxo_count", bucket_counts[b]);

        int bar_len = (int)(pct / 2.0);
        if (bar_len > 40) bar_len = 40;
        char bar[42];
        for (int i = 0; i < bar_len; i++) bar[i] = '#';
        bar[bar_len] = '\0';
        json_push_kv_str(&entry, "bar", bar);

        json_push_back(&arr, &entry);
        json_free(&entry);
    }

    json_push_kv(result, "buckets", &arr);
    json_free(&arr);

    #undef BLOCKS_PER_DAY
    #undef BLOCKS_PER_WEEK
    #undef BLOCKS_PER_MONTH
    #undef NUM_HODL_BUCKETS

    return true;
}

/* ── Color mapping for heatmap ──────────────────────────────────── */

static void plasma_color(double t, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* 0=black, 0.15=dark blue, 0.35=purple, 0.55=red, 0.75=orange,
     * 0.9=yellow, 1.0=white */
    if (t <= 0.0) { *r = 0; *g = 0; *b = 0; return; }
    if (t >= 1.0) { *r = 255; *g = 255; *b = 255; return; }
    if (t < 0.15) {
        double s = t / 0.15;
        *r = 0; *g = 0; *b = (uint8_t)(s * 140);
    } else if (t < 0.35) {
        double s = (t - 0.15) / 0.20;
        *r = (uint8_t)(s * 160); *g = 0; *b = (uint8_t)(140 + s * 60);
    } else if (t < 0.55) {
        double s = (t - 0.35) / 0.20;
        *r = (uint8_t)(160 + s * 95); *g = (uint8_t)(s * 40); *b = (uint8_t)(200 - s * 200);
    } else if (t < 0.75) {
        double s = (t - 0.55) / 0.20;
        *r = 255; *g = (uint8_t)(40 + s * 180); *b = 0;
    } else if (t < 0.92) {
        double s = (t - 0.75) / 0.17;
        *r = 255; *g = (uint8_t)(220 + s * 35); *b = (uint8_t)(s * 100);
    } else {
        double s = (t - 0.92) / 0.08;
        *r = 255; *g = 255; *b = (uint8_t)(100 + s * 155);
    }
}

/* HODL wave age band colors (warm=young, cool=old) */
static const uint8_t hodl_colors[10][3] = {
    {255, 50,  30},   /* < 1 day    — bright red */
    {255, 120, 20},   /* 1d - 1w    — orange */
    {255, 200, 30},   /* 1w - 1m    — yellow */
    {180, 230, 40},   /* 1 - 3m     — yellow-green */
    {80,  210, 60},   /* 3 - 6m     — green */
    {30,  190, 150},  /* 6 - 12m    — teal */
    {30,  140, 220},  /* 1 - 2y     — blue */
    {60,  80,  200},  /* 2 - 3y     — indigo */
    {100, 50,  180},  /* 3 - 5y     — purple */
    {80,  30,  120},  /* > 5y       — dark purple */
};

/* gethodlwaveimage: Scan UTXO set, generate a PPM heatmap + HODL wave bar,
 * save to datadir/hodlwave.ppm. */
static bool rpc_gethodlwaveimage(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "gethodlwaveimage\n"
        "\nGenerates a HODL wave heatmap image from the current UTXO set.\n"
        "\nThe image shows:\n"
        "  Top: UTXO creation heatmap (X=block height, Y=value log scale)\n"
        "  Bottom: HODL wave age distribution bar\n"
        "\nSaves to <datadir>/hodlwave.ppm\n"
        "\nResult: { file, width, height, total_utxos, total_value }\n");

    if (!g_coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    int tip_height = active_chain_height(&g_main_state->chain_active);
    if (tip_height <= 0) {
        json_set_str(result, "No blocks");
        return false;
    }

    /* Image dimensions */
    const int IMG_W = 1920;
    const int IMG_H = 1080;
    const int HEATMAP_H = 860;   /* top portion: creation heatmap */
    const int WAVE_H = 160;      /* bottom portion: HODL wave bar */
    const int GAP_H = IMG_H - HEATMAP_H - WAVE_H; /* separator */

    /* Grid: height bins (columns) × value bins (rows) */
    int blocks_per_col = (tip_height + IMG_W - 1) / IMG_W;
    if (blocks_per_col < 1) blocks_per_col = 1;

    /* Value bins: log10 scale from 1 satoshi (0) to 100000 ZCL (13)
     * 1 sat = 10^0, 1 ZCL = 10^8, 100000 ZCL = 10^13 */
    const double LOG_MIN = 0.0;
    const double LOG_MAX = 13.0;

    /* Allocate grid: grid[row][col] = count of UTXOs */
    int64_t *grid = calloc((size_t)(HEATMAP_H * IMG_W), sizeof(int64_t));
    if (!grid) {
        json_set_str(result, "Out of memory");
        return false;
    }

    /* HODL wave buckets (same 10 as gethodlwave) */
    #define BLOCKS_PER_DAY_IMG    1152
    #define BLOCKS_PER_WEEK_IMG   (BLOCKS_PER_DAY_IMG * 7)
    #define BLOCKS_PER_MONTH_IMG  (BLOCKS_PER_DAY_IMG * 30)
    #define NUM_HODL_BUCKETS_IMG  10

    int hodl_max_age[NUM_HODL_BUCKETS_IMG] = {
        BLOCKS_PER_DAY_IMG,
        BLOCKS_PER_WEEK_IMG,
        BLOCKS_PER_MONTH_IMG,
        BLOCKS_PER_MONTH_IMG * 3,
        BLOCKS_PER_MONTH_IMG * 6,
        BLOCKS_PER_MONTH_IMG * 12,
        BLOCKS_PER_MONTH_IMG * 24,
        BLOCKS_PER_MONTH_IMG * 36,
        BLOCKS_PER_MONTH_IMG * 60,
        0  /* unlimited */
    };

    int64_t hodl_values[NUM_HODL_BUCKETS_IMG] = {0};
    int64_t total_value = 0;
    int64_t total_utxos = 0;

    /* Scan UTXO set */
    struct db_iterator it;
    db_iter_init(&it, &g_coins_db->db);
    char prefix = 'c';
    db_iter_seek(&it, &prefix, 1);

    while (db_iter_valid(&it)) {
        size_t keylen = 0;
        const char *key = db_iter_key(&it, &keylen);
        if (!key || keylen != 33 || key[0] != 'c')
            break;

        struct uint256 txid;
        memcpy(txid.data, key + 1, 32);

        struct coins c;
        coins_init(&c);
        if (coins_view_db_get_coins(g_coins_db, &txid, &c)) {
            int height = c.height;
            if (height < 0) height = 0;
            if (height > tip_height) height = tip_height;
            int age = tip_height - height;

            /* HODL bucket */
            int bucket = NUM_HODL_BUCKETS_IMG - 1;
            for (int b = 0; b < NUM_HODL_BUCKETS_IMG - 1; b++) {
                if (age < hodl_max_age[b]) { bucket = b; break; }
            }

            /* Heatmap column from creation height */
            int col = height / blocks_per_col;
            if (col >= IMG_W) col = IMG_W - 1;

            for (size_t i = 0; i < c.num_vout; i++) {
                if (!tx_out_is_null(&c.vout[i]) && c.vout[i].value > 0) {
                    double logval = log10((double)c.vout[i].value);
                    if (logval < LOG_MIN) logval = LOG_MIN;
                    if (logval > LOG_MAX) logval = LOG_MAX;
                    /* Map to row (bottom=small, top=large) */
                    int row = HEATMAP_H - 1 -
                        (int)((logval - LOG_MIN) / (LOG_MAX - LOG_MIN)
                              * (HEATMAP_H - 1));
                    if (row < 0) row = 0;
                    if (row >= HEATMAP_H) row = HEATMAP_H - 1;
                    grid[row * IMG_W + col]++;

                    hodl_values[bucket] += c.vout[i].value;
                    total_value += c.vout[i].value;
                    total_utxos++;
                }
            }
        }
        coins_free(&c);
        db_iter_next(&it);
    }
    db_iter_free(&it);

    /* Find max grid value for normalization */
    int64_t grid_max = 0;
    for (int i = 0; i < HEATMAP_H * IMG_W; i++)
        if (grid[i] > grid_max) grid_max = grid[i];

    /* Allocate pixel buffer */
    uint8_t *pixels = calloc((size_t)(IMG_W * IMG_H * 3), 1);
    if (!pixels) {
        free(grid);
        json_set_str(result, "Out of memory for image");
        return false;
    }

    /* Render heatmap (top portion) */
    for (int row = 0; row < HEATMAP_H; row++) {
        for (int col = 0; col < IMG_W; col++) {
            int64_t count = grid[row * IMG_W + col];
            if (count > 0 && grid_max > 0) {
                double t = log1p((double)count) / log1p((double)grid_max);
                uint8_t r, g, b;
                plasma_color(t, &r, &g, &b);
                int idx = (row * IMG_W + col) * 3;
                pixels[idx] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
            }
            /* else: stays black */
        }
    }

    /* Render separator gap (dark gray line) */
    for (int row = HEATMAP_H; row < HEATMAP_H + GAP_H; row++) {
        for (int col = 0; col < IMG_W; col++) {
            int idx = (row * IMG_W + col) * 3;
            pixels[idx] = 20;
            pixels[idx + 1] = 20;
            pixels[idx + 2] = 20;
        }
    }

    /* Render HODL wave bar (bottom portion) — stacked horizontal bands */
    if (total_value > 0) {
        int x_start = 0;
        for (int b = NUM_HODL_BUCKETS_IMG - 1; b >= 0; b--) {
            int band_width = (int)((double)hodl_values[b]
                                   / (double)total_value * IMG_W);
            if (b == 0) band_width = IMG_W - x_start; /* fill remainder */

            for (int col = x_start; col < x_start + band_width && col < IMG_W;
                 col++) {
                for (int row = HEATMAP_H + GAP_H; row < IMG_H; row++) {
                    int idx = (row * IMG_W + col) * 3;
                    pixels[idx]     = hodl_colors[b][0];
                    pixels[idx + 1] = hodl_colors[b][1];
                    pixels[idx + 2] = hodl_colors[b][2];
                }
            }
            x_start += band_width;
        }
    }

    /* Draw consensus activation height markers on the heatmap */
    int markers[] = { 476969, 585318, 585322, 707000 };
    for (int m = 0; m < 4; m++) {
        int col = markers[m] / blocks_per_col;
        if (col >= 0 && col < IMG_W) {
            for (int row = 0; row < HEATMAP_H; row++) {
                int idx = (row * IMG_W + col) * 3;
                /* Cyan marker line */
                pixels[idx]     = (uint8_t)(pixels[idx] / 2 + 30);
                pixels[idx + 1] = (uint8_t)(pixels[idx+1] / 2 + 200);
                pixels[idx + 2] = (uint8_t)(pixels[idx+2] / 2 + 200);
            }
        }
    }

    free(grid);

    /* Write PPM file */
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/hodlwave.ppm",
             g_datadir ? g_datadir : ".");

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        free(pixels);
        json_set_str(result, "Failed to open output file");
        return false;
    }

    fprintf(f, "P6\n%d %d\n255\n", IMG_W, IMG_H);
    fwrite(pixels, 3, (size_t)(IMG_W * IMG_H), f);
    fclose(f);
    free(pixels);

    /* Return result */
    json_set_object(result);
    json_push_kv_str(result, "file", filepath);
    json_push_kv_int(result, "width", IMG_W);
    json_push_kv_int(result, "height", IMG_H);
    json_push_kv_int(result, "total_utxos", total_utxos);
    json_push_kv_int(result, "blocks_per_column", blocks_per_col);

    char amt[32];
    snprintf(amt, sizeof(amt), "%lld.%08lld",
             (long long)(total_value / 100000000),
             (long long)(total_value % 100000000));
    json_push_kv_str(result, "total_value", amt);

    /* Also include the HODL wave percentages */
    const char *labels[] = {
        "< 1 day", "1d - 1w", "1w - 1m", "1 - 3m", "3 - 6m",
        "6 - 12m", "1 - 2y", "2 - 3y", "3 - 5y", "> 5y"
    };
    struct json_value wave = {0};
    json_set_array(&wave);
    for (int b = 0; b < NUM_HODL_BUCKETS_IMG; b++) {
        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "label", labels[b]);
        double pct = total_value > 0
            ? (double)hodl_values[b] / (double)total_value * 100.0 : 0.0;
        json_push_kv_real(&entry, "percent", pct);

        snprintf(amt, sizeof(amt), "%lld.%08lld",
                 (long long)(hodl_values[b] / 100000000),
                 (long long)(hodl_values[b] % 100000000));
        json_push_kv_str(&entry, "value", amt);
        json_push_back(&wave, &entry);
        json_free(&entry);
    }
    json_push_kv(result, "hodl_wave", &wave);
    json_free(&wave);

    #undef BLOCKS_PER_DAY_IMG
    #undef BLOCKS_PER_WEEK_IMG
    #undef BLOCKS_PER_MONTH_IMG
    #undef NUM_HODL_BUCKETS_IMG

    return true;
}

/* reindexchainstate: Wipe and rebuild the UTXO set by replaying all blocks.
 * Fixes any corrupt coins entries from prior serialization bugs. */
static bool rpc_reindexchainstate(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "reindexchainstate\n"
        "\nWipes the chainstate (UTXO database) and rebuilds it by replaying\n"
        "all blocks from genesis to chain tip. This fixes any corrupt coins\n"
        "entries from prior serialization bugs.\n"
        "\nWARNING: This operation takes a long time (minutes to hours).\n"
        "The node will not process new blocks or transactions during reindex.\n");

    if (!g_coins_db || !g_coins_tip || !g_main_state || !g_datadir) {
        json_set_str(result, "Node not fully initialized");
        return false;
    }

    int tip_height = active_chain_height(&g_main_state->chain_active);
    if (tip_height < 0) {
        json_set_str(result, "No active chain");
        return false;
    }

    printf("reindexchainstate: rebuilding UTXO set for %d blocks...\n",
           tip_height + 1);
    fflush(stdout);

    /* Step 1: Flush and free the in-memory cache */
    coins_view_cache_flush(g_coins_tip);
    coins_view_cache_free(g_coins_tip);

    /* Step 2: Close and reopen coins DB with wipe=true */
    coins_view_db_close(g_coins_db);

    char coins_path[1024];
    snprintf(coins_path, sizeof(coins_path), "%s/chainstate", g_datadir);
    if (!coins_view_db_open(g_coins_db, coins_path,
                            450 << 20, false, true)) {
        json_set_str(result, "Failed to reopen coins database");
        return false;
    }

    /* Step 3: Reinitialize coins cache */
    coins_view_cache_init(g_coins_tip, &g_coins_db->view);

    int64_t t_start = (int64_t)time(NULL);
    int errors = 0;

    /* Step 4: Replay all blocks */
    for (int h = 0; h <= tip_height; h++) {
        struct block_index *pindex = active_chain_at(
            &g_main_state->chain_active, h);
        if (!pindex) {
            printf("reindexchainstate: missing block_index at height %d\n", h);
            errors++;
            continue;
        }

        struct block blk;
        if (!read_block_from_disk_index(&blk, pindex, g_datadir)) {
            printf("reindexchainstate: failed to read block at height %d\n", h);
            errors++;
            continue;
        }

        /* Genesis block: just set best block */
        if (h == 0) {
            struct uint256 block_hash;
            block_header_get_hash(&blk.header, &block_hash);
            coins_view_cache_set_best_block(g_coins_tip, &block_hash);
            block_free(&blk);
            if (h % 10000 == 0) {
                printf("  height %d/%d\n", h, tip_height);
                fflush(stdout);
            }
            continue;
        }

        /* Apply each transaction to the UTXO set */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            update_coins(&blk.vtx[i], g_coins_tip, pindex->nHeight);
        }

        /* Set best block hash */
        struct uint256 block_hash;
        block_header_get_hash(&blk.header, &block_hash);
        coins_view_cache_set_best_block(g_coins_tip, &block_hash);

        block_free(&blk);

        /* Periodic flush every 10000 blocks */
        if (h % 10000 == 0) {
            coins_view_cache_flush(g_coins_tip);
            int64_t elapsed = (int64_t)time(NULL) - t_start;
            double rate = elapsed > 0 ? (double)h / (double)elapsed : 0;
            int eta = rate > 0 ? (int)((tip_height - h) / rate) : 0;
            printf("  height %d/%d (%.0f blk/s, ETA %dm%ds)\n",
                   h, tip_height, rate, eta / 60, eta % 60);
            fflush(stdout);
        }
    }

    /* Step 5: Final flush */
    coins_view_cache_flush(g_coins_tip);

    int64_t elapsed = (int64_t)time(NULL) - t_start;
    printf("reindexchainstate: complete in %lldm%llds (%d errors)\n",
           (long long)(elapsed / 60), (long long)(elapsed % 60), errors);
    fflush(stdout);

    /* Report results */
    json_set_object(result);
    json_push_kv_int(result, "height", tip_height);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    json_push_kv_int(result, "errors", errors);
    json_push_kv_str(result, "status", errors == 0 ? "success" : "completed with errors");

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
        { "blockchain", "gettxoutsetinfo",      rpc_gettxoutsetinfo,      true },
        { "blockchain", "gethodlwave",          rpc_gethodlwave,          true },
        { "blockchain", "gethodlwaveimage",    rpc_gethodlwaveimage,    true },
        { "blockchain", "reindexchainstate",    rpc_reindexchainstate,    false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
