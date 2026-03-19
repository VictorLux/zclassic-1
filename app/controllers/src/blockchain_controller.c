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
#include "util/png_writer.h"
#include "storage/block_index_db.h"
#include "zslp/slp.h"
#include "models/block.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "controllers/sync_controller.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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

    /* ZClassic block times: 150s before Buttercup (707000), 75s after.
     * Age is computed in seconds for accurate bucket assignment. */
    #define BUTTERCUP_HEIGHT  707000
    #define PRE_SPACING       150
    #define POST_SPACING      75
    #define NUM_HODL_BUCKETS  10

    /* Compute age in seconds for a UTXO created at 'height' */
    #define UTXO_AGE_SECONDS(height, tip) ( \
        ((height) < BUTTERCUP_HEIGHT) \
            ? (int64_t)(BUTTERCUP_HEIGHT - (height)) * PRE_SPACING \
              + (int64_t)((tip) - BUTTERCUP_HEIGHT) * POST_SPACING \
            : (int64_t)((tip) - (height)) * POST_SPACING)

    #define SECS_PER_DAY    86400
    #define SECS_PER_WEEK   604800
    #define SECS_PER_MONTH  2592000
    #define SECS_PER_YEAR   31557600

    struct {
        const char *label;
        int64_t max_age_seconds;  /* 0 = unlimited */
    } buckets[NUM_HODL_BUCKETS] = {
        { "< 1 day",    SECS_PER_DAY },
        { "1d - 1w",    SECS_PER_WEEK },
        { "1w - 1m",    SECS_PER_MONTH },
        { "1 - 3m",     SECS_PER_MONTH * 3 },
        { "3 - 6m",     SECS_PER_MONTH * 6 },
        { "6 - 12m",    SECS_PER_YEAR },
        { "1 - 2y",     SECS_PER_YEAR * 2 },
        { "2 - 3y",     SECS_PER_YEAR * 3 },
        { "3 - 5y",     SECS_PER_YEAR * 5 },
        { "> 5y",       0 },
    };

    int64_t bucket_values[NUM_HODL_BUCKETS] = {0};
    int64_t bucket_counts[NUM_HODL_BUCKETS] = {0};
    int64_t total_value = 0;
    int64_t total_utxos = 0;
    int64_t total_txs = 0;
    int64_t skipped = 0;
    int64_t over_1y_value = 0;
    int64_t over_1y_count = 0;

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

        int64_t age_secs = UTXO_AGE_SECONDS(height, tip_height);
        int bucket_idx = NUM_HODL_BUCKETS - 1;
        for (int b = 0; b < NUM_HODL_BUCKETS - 1; b++) {
            if (age_secs < buckets[b].max_age_seconds) {
                bucket_idx = b;
                break;
            }
        }

        for (size_t i = 0; i < c.num_vout; i++) {
            if (!tx_out_is_null(&c.vout[i])) {
                bucket_values[bucket_idx] += c.vout[i].value;
                bucket_counts[bucket_idx]++;
                if (age_secs >= SECS_PER_YEAR) {
                    over_1y_value += c.vout[i].value;
                    over_1y_count++;
                }
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
    double total_zcl = (double)total_value / 100000000.0;
    snprintf(amt, sizeof(amt), "%.8f", total_zcl);
    json_push_kv_str(result, "total_supply_zcl", amt);

    /* Build buckets with cumulative % (oldest-first accumulation).
     * "cumulative_pct" answers: what % of supply has been unmoved
     * for AT LEAST this long? Buckets go young→old, so accumulate
     * from the old end backwards. */
    int64_t cumul_value = 0;
    double cumul_pcts[NUM_HODL_BUCKETS];
    for (int b = NUM_HODL_BUCKETS - 1; b >= 0; b--) {
        cumul_value += bucket_values[b];
        cumul_pcts[b] = total_value > 0
            ? (double)cumul_value / (double)total_value * 100.0
            : 0.0;
    }

    struct json_value arr = {0};
    json_set_array(&arr);

    for (int b = 0; b < NUM_HODL_BUCKETS; b++) {
        struct json_value entry = {0};
        json_set_object(&entry);
        json_push_kv_str(&entry, "age", buckets[b].label);

        double bval = (double)bucket_values[b] / 100000000.0;
        snprintf(amt, sizeof(amt), "%.8f", bval);
        json_push_kv_str(&entry, "zcl", amt);

        json_push_kv_int(&entry, "utxos", bucket_counts[b]);

        double pct = total_value > 0
                   ? (double)bucket_values[b] / (double)total_value * 100.0
                   : 0.0;
        json_push_kv_real(&entry, "pct", pct);
        json_push_kv_real(&entry, "cumulative_pct_unmoved", cumul_pcts[b]);

        /* Bar shows cumulative % */
        int bar_len = (int)(cumul_pcts[b] / 2.5);
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

    #undef BUTTERCUP_HEIGHT
    #undef PRE_SPACING
    #undef POST_SPACING
    #undef UTXO_AGE_SECONDS
    #undef SECS_PER_DAY
    #undef SECS_PER_WEEK
    #undef SECS_PER_MONTH
    #undef SECS_PER_YEAR
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

    /* HODL wave buckets (same 10 as gethodlwave, time-based) */
    #define BUTTERCUP_HT_IMG  707000
    #define PRE_SP_IMG        150
    #define POST_SP_IMG       75
    #define SECS_DAY_IMG      86400
    #define SECS_WEEK_IMG     604800
    #define SECS_MONTH_IMG    2592000
    #define SECS_YEAR_IMG     31557600
    #define NUM_HODL_BUCKETS_IMG  10

    #define AGE_SECS_IMG(h, tip) ( \
        ((h) < BUTTERCUP_HT_IMG) \
            ? (int64_t)(BUTTERCUP_HT_IMG - (h)) * PRE_SP_IMG \
              + (int64_t)((tip) - BUTTERCUP_HT_IMG) * POST_SP_IMG \
            : (int64_t)((tip) - (h)) * POST_SP_IMG)

    int64_t hodl_max_age_secs[NUM_HODL_BUCKETS_IMG] = {
        SECS_DAY_IMG,
        SECS_WEEK_IMG,
        SECS_MONTH_IMG,
        SECS_MONTH_IMG * 3,
        SECS_MONTH_IMG * 6,
        SECS_YEAR_IMG,
        SECS_YEAR_IMG * 2,
        SECS_YEAR_IMG * 3,
        SECS_YEAR_IMG * 5,
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
            int64_t age_s = AGE_SECS_IMG(height, tip_height);

            /* HODL bucket */
            int bucket = NUM_HODL_BUCKETS_IMG - 1;
            for (int b = 0; b < NUM_HODL_BUCKETS_IMG - 1; b++) {
                if (age_s < hodl_max_age_secs[b]) { bucket = b; break; }
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

    /* Write PNG file */
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/hodlwave.png",
             g_datadir ? g_datadir : ".");

    if (!png_write_rgb(filepath, pixels, (uint32_t)IMG_W, (uint32_t)IMG_H)) {
        free(pixels);
        json_set_str(result, "Failed to write PNG file");
        return false;
    }
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

    #undef BUTTERCUP_HT_IMG
    #undef PRE_SP_IMG
    #undef POST_SP_IMG
    #undef SECS_DAY_IMG
    #undef SECS_WEEK_IMG
    #undef SECS_MONTH_IMG
    #undef SECS_YEAR_IMG
    #undef AGE_SECS_IMG
    #undef NUM_HODL_BUCKETS_IMG

    return true;
}

/* ── HODL wave timeline ────────────────────────────────────────── */

/* Convert block height to approximate Unix timestamp.
 * Pre-Buttercup (< 707000): 150s blocks.  Post: 75s blocks. */
static int64_t height_to_timestamp(int height)
{
    const int64_t GENESIS_TIME = 1478403829;
    const int BUTTERCUP_HT = 707000;
    if (height <= 0) return GENESIS_TIME;
    if (height < BUTTERCUP_HT)
        return GENESIS_TIME + (int64_t)height * 150;
    return GENESIS_TIME + (int64_t)BUTTERCUP_HT * 150
                        + (int64_t)(height - BUTTERCUP_HT) * 75;
}

/* gethodlwavetimeline: Time-series of surviving UTXO value by creation date.
 * Shows how much ZCL that was last moved on each day/month still sits unspent.
 * Also generates a PPM stacked-area chart to <datadir>/hodlwave_timeline.ppm.
 *
 * Params: [granularity]  —  "day" (default) or "month" */
static bool rpc_gethodlwavetimeline(const struct json_value *params, bool help,
                                     struct json_value *result)
{
    RPC_HELP(help, result,
        "gethodlwavetimeline ( \"granularity\" )\n"
        "\nTime-series of surviving UTXO value by creation date.\n"
        "Shows how much ZCL last moved on each day/month is still unspent.\n"
        "Generates a PPM chart at <datadir>/hodlwave_timeline.ppm\n"
        "\nArguments:\n"
        "  1. granularity   (string, optional, default=\"month\") \"day\" or \"month\"\n"
        "\nResult: { periods: [{date, zcl, utxos, cumulative_pct}], chart_file }\n");

    if (!g_coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Parse granularity */
    bool by_month = true;
    if (params && params->type == JSON_ARR && params->num_children > 0 &&
        params->children[0].type == JSON_STR) {
        if (strcmp(params->children[0].val.s, "day") == 0)
            by_month = false;
    }

    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    int tip_height = active_chain_height(&g_main_state->chain_active);
    int64_t tip_ts = height_to_timestamp(tip_height);

    /* Compute number of periods.
     * day = 86400s bins.  month = calendar month bins (use 30.44d avg). */
    int64_t genesis_ts = height_to_timestamp(0);
    int num_periods;
    if (by_month) {
        /* Months from genesis to tip */
        struct tm g_tm, t_tm;
        time_t g_t = (time_t)genesis_ts, t_t = (time_t)tip_ts;
        gmtime_r(&g_t, &g_tm);
        gmtime_r(&t_t, &t_tm);
        num_periods = (t_tm.tm_year - g_tm.tm_year) * 12
                    + (t_tm.tm_mon - g_tm.tm_mon) + 1;
    } else {
        num_periods = (int)((tip_ts - genesis_ts) / 86400) + 1;
    }
    if (num_periods < 1) num_periods = 1;
    if (num_periods > 10000) num_periods = 10000;

    /* Allocate per-period accumulators */
    int64_t *period_value = calloc((size_t)num_periods, sizeof(int64_t));
    int64_t *period_count = calloc((size_t)num_periods, sizeof(int64_t));
    if (!period_value || !period_count) {
        free(period_value); free(period_count);
        json_set_str(result, "Out of memory");
        return false;
    }

    int64_t total_value = 0;
    int64_t total_utxos = 0;

    /* Helper: timestamp → period index */
    struct tm genesis_tm;
    {
        time_t g_t = (time_t)genesis_ts;
        gmtime_r(&g_t, &genesis_tm);
    }

    /* Scan UTXO set */
    struct db_iterator it;
    db_iter_init(&it, &g_coins_db->db);
    char prefix = 'c';
    db_iter_seek(&it, &prefix, 1);

    while (db_iter_valid(&it)) {
        size_t keylen = 0;
        const char *key = db_iter_key(&it, &keylen);
        if (!key || keylen != 33 || key[0] != 'c') break;

        struct uint256 txid;
        memcpy(txid.data, key + 1, 32);

        struct coins c;
        coins_init(&c);
        if (!coins_view_db_get_coins(g_coins_db, &txid, &c)) {
            coins_free(&c); db_iter_next(&it); continue;
        }

        int height = c.height;
        if (height < 0 || height > tip_height + 100) {
            coins_free(&c); db_iter_next(&it); continue;
        }

        int64_t ts = height_to_timestamp(height);
        int pidx;
        if (by_month) {
            struct tm utm;
            time_t tt = (time_t)ts;
            gmtime_r(&tt, &utm);
            pidx = (utm.tm_year - genesis_tm.tm_year) * 12
                 + (utm.tm_mon - genesis_tm.tm_mon);
        } else {
            pidx = (int)((ts - genesis_ts) / 86400);
        }
        if (pidx < 0) pidx = 0;
        if (pidx >= num_periods) pidx = num_periods - 1;

        for (size_t i = 0; i < c.num_vout; i++) {
            if (!tx_out_is_null(&c.vout[i]) && c.vout[i].value > 0) {
                period_value[pidx] += c.vout[i].value;
                period_count[pidx]++;
            }
        }

        coins_free(&c);
        db_iter_next(&it);
    }
    db_iter_free(&it);

    for (int p = 0; p < num_periods; p++) {
        total_value += period_value[p];
        total_utxos += period_count[p];
    }

    /* ── Generate PPM chart ─────────────────────────────────── */
    const int IMG_W = 1920;
    const int IMG_H = 600;
    uint8_t *img = calloc((size_t)(IMG_W * IMG_H * 3), 1);
    if (!img) {
        free(period_value); free(period_count);
        json_set_str(result, "Out of memory for image");
        return false;
    }

    /* Background: dark gray */
    for (int i = 0; i < IMG_W * IMG_H * 3; i += 3) {
        img[i] = 20; img[i+1] = 20; img[i+2] = 25;
    }

    /* Find max period value for Y-axis scaling */
    int64_t max_val = 0;
    for (int p = 0; p < num_periods; p++)
        if (period_value[p] > max_val) max_val = period_value[p];

    /* Draw bars: X maps period, Y maps value */
    int margin_bottom = 40;
    int margin_top = 20;
    int plot_h = IMG_H - margin_bottom - margin_top;
    double x_scale = (double)IMG_W / (double)num_periods;

    for (int p = 0; p < num_periods; p++) {
        if (period_value[p] == 0) continue;

        int x0 = (int)(p * x_scale);
        int x1 = (int)((p + 1) * x_scale);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > IMG_W) x1 = IMG_W;

        double frac = max_val > 0
            ? (double)period_value[p] / (double)max_val : 0.0;
        int bar_h = (int)(frac * plot_h);
        if (bar_h < 1) bar_h = 1;

        /* Color: teal gradient by age (older = cooler) */
        double age_frac = 1.0 - (double)p / (double)num_periods;
        uint8_t cr = (uint8_t)(30 + age_frac * 50);
        uint8_t cg = (uint8_t)(140 + (1.0 - age_frac) * 80);
        uint8_t cb = (uint8_t)(180 + (1.0 - age_frac) * 40);

        for (int x = x0; x < x1; x++) {
            for (int dy = 0; dy < bar_h; dy++) {
                int y = IMG_H - margin_bottom - 1 - dy;
                if (y < margin_top || y >= IMG_H) continue;
                int off = (y * IMG_W + x) * 3;
                img[off] = cr; img[off+1] = cg; img[off+2] = cb;
            }
        }
    }

    /* Draw Y-axis labels: mark 25%, 50%, 75%, max */
    for (int pct = 25; pct <= 100; pct += 25) {
        int y = IMG_H - margin_bottom - (int)((pct / 100.0) * plot_h);
        if (y < margin_top || y >= IMG_H) continue;
        for (int x = 0; x < IMG_W; x += 4) {
            int off = (y * IMG_W + x) * 3;
            img[off] = 60; img[off+1] = 60; img[off+2] = 65;
        }
    }

    /* Draw consensus fork markers */
    int fork_heights[] = { 476969, 585318, 585322, 707000 };
    for (int f = 0; f < 4; f++) {
        int64_t fts = height_to_timestamp(fork_heights[f]);
        int fp;
        if (by_month) {
            struct tm ftm;
            time_t ft = (time_t)fts;
            gmtime_r(&ft, &ftm);
            fp = (ftm.tm_year - genesis_tm.tm_year) * 12
               + (ftm.tm_mon - genesis_tm.tm_mon);
        } else {
            fp = (int)((fts - genesis_ts) / 86400);
        }
        int fx = (int)(fp * x_scale);
        if (fx >= 0 && fx < IMG_W) {
            for (int y = margin_top; y < IMG_H - margin_bottom; y++) {
                int off = (y * IMG_W + fx) * 3;
                img[off] = 0; img[off+1] = 220; img[off+2] = 220;
            }
        }
    }

    /* Write PNG */
    char ppm_path[512];
    snprintf(ppm_path, sizeof(ppm_path), "%s/hodlwave_timeline.png",
             g_datadir ? g_datadir : ".");
    png_write_rgb(ppm_path, img, (uint32_t)IMG_W, (uint32_t)IMG_H);
    free(img);

    /* ── Build JSON response ────────────────────────────────── */
    json_set_object(result);
    json_push_kv_int(result, "tip_height", tip_height);
    json_push_kv_int(result, "total_utxos", total_utxos);
    json_push_kv_str(result, "granularity", by_month ? "month" : "day");
    json_push_kv_int(result, "num_periods", num_periods);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.8f", (double)total_value / 1e8);
    json_push_kv_str(result, "total_supply_zcl", buf);
    json_push_kv_str(result, "chart_file", ppm_path);

    struct json_value periods_arr = {0};
    json_set_array(&periods_arr);

    int64_t cumul = 0;
    for (int p = num_periods - 1; p >= 0; p--) {
        cumul += period_value[p];
    }

    int64_t running = 0;
    for (int p = 0; p < num_periods; p++) {
        if (period_value[p] == 0) continue;

        struct json_value pe = {0};
        json_set_object(&pe);

        /* Compute date label */
        int64_t period_ts;
        if (by_month) {
            struct tm ptm = genesis_tm;
            ptm.tm_mon += p;
            ptm.tm_year += ptm.tm_mon / 12;
            ptm.tm_mon %= 12;
            ptm.tm_mday = 1;
            snprintf(buf, sizeof(buf), "%04d-%02d",
                     ptm.tm_year + 1900, ptm.tm_mon + 1);
            period_ts = (int64_t)timegm(&ptm);
        } else {
            period_ts = genesis_ts + (int64_t)p * 86400;
            struct tm dtm;
            time_t dt = (time_t)period_ts;
            gmtime_r(&dt, &dtm);
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                     dtm.tm_year + 1900, dtm.tm_mon + 1, dtm.tm_mday);
        }
        json_push_kv_str(&pe, "date", buf);

        double pval = (double)period_value[p] / 1e8;
        snprintf(buf, sizeof(buf), "%.8f", pval);
        json_push_kv_str(&pe, "zcl", buf);

        json_push_kv_int(&pe, "utxos", period_count[p]);

        /* Cumulative from oldest: running sum from period 0..p */
        running += period_value[p];
        double cpct = total_value > 0
            ? (double)running / (double)total_value * 100.0 : 0.0;
        json_push_kv_real(&pe, "cumulative_pct", cpct);

        json_push_back(&periods_arr, &pe);
        json_free(&pe);
    }

    json_push_kv(result, "periods", &periods_arr);
    json_free(&periods_arr);

    free(period_value);
    free(period_count);
    return true;
}

/* ── Stacked-area HODL wave chart over time ───────────────────── */

/* gethodlwavechart: Classic Unchained Capital-style stacked area chart.
 * For each monthly column, shows 100% of surviving UTXO supply colored
 * by age band. Generates a 1920x1080 PPM at <datadir>/hodlwave_chart.ppm.
 *
 * Approach: scan UTXOs once into (creation_timestamp, value) pairs.
 * For each monthly column T, classify every UTXO created before T by its
 * age at time T, stack bands from oldest (bottom) to youngest (top). */
static bool rpc_gethodlwavechart(const struct json_value *params, bool help,
                                  struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "gethodlwavechart\n"
        "\nGenerates a stacked-area HODL wave chart showing how the age\n"
        "distribution of surviving UTXOs evolved month-by-month over the\n"
        "full ~9-year chain history.\n"
        "\n10 age bands from '> 5y' (bottom, dark purple) to '< 1 day' (top, red).\n"
        "Consensus fork heights marked with cyan lines.\n"
        "\nSaves to <datadir>/hodlwave_chart.ppm\n"
        "\nResult: { chart_file, total_utxos, total_supply_zcl, num_months }\n");

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

    /* ── Step 1: Scan UTXO set into arrays ──────────────────── */
    size_t cap = 2000000;
    int64_t *utxo_ts = malloc(cap * sizeof(int64_t));
    int64_t *utxo_val = malloc(cap * sizeof(int64_t));
    if (!utxo_ts || !utxo_val) {
        free(utxo_ts); free(utxo_val);
        json_set_str(result, "Out of memory");
        return false;
    }

    size_t num_utxos = 0;
    struct db_iterator it;
    db_iter_init(&it, &g_coins_db->db);
    char prefix = 'c';
    db_iter_seek(&it, &prefix, 1);

    while (db_iter_valid(&it)) {
        size_t keylen = 0;
        const char *key = db_iter_key(&it, &keylen);
        if (!key || keylen != 33 || key[0] != 'c') break;

        struct uint256 txid;
        memcpy(txid.data, key + 1, 32);

        struct coins c;
        coins_init(&c);
        if (!coins_view_db_get_coins(g_coins_db, &txid, &c)) {
            coins_free(&c); db_iter_next(&it); continue;
        }

        int height = c.height;
        if (height < 0 || height > tip_height + 100) {
            coins_free(&c); db_iter_next(&it); continue;
        }

        int64_t ts = height_to_timestamp(height);

        for (size_t i = 0; i < c.num_vout; i++) {
            if (!tx_out_is_null(&c.vout[i]) && c.vout[i].value > 0) {
                if (num_utxos >= cap) {
                    cap *= 2;
                    utxo_ts = realloc(utxo_ts, cap * sizeof(int64_t));
                    utxo_val = realloc(utxo_val, cap * sizeof(int64_t));
                    if (!utxo_ts || !utxo_val) {
                        free(utxo_ts); free(utxo_val);
                        coins_free(&c); db_iter_free(&it);
                        json_set_str(result, "Out of memory");
                        return false;
                    }
                }
                utxo_ts[num_utxos] = ts;
                utxo_val[num_utxos] = c.vout[i].value;
                num_utxos++;
            }
        }
        coins_free(&c);
        db_iter_next(&it);
    }
    db_iter_free(&it);

    printf("gethodlwavechart: scanned %zu UTXOs\n", num_utxos);
    fflush(stdout);

    /* ── Step 2: Compute monthly columns ────────────────────── */
    int64_t genesis_ts = height_to_timestamp(0);
    int64_t tip_ts = height_to_timestamp(tip_height);

    struct tm g_tm, t_tm;
    { time_t gt = (time_t)genesis_ts; gmtime_r(&gt, &g_tm); }
    { time_t tt = (time_t)tip_ts;     gmtime_r(&tt, &t_tm); }
    int num_months = (t_tm.tm_year - g_tm.tm_year) * 12
                   + (t_tm.tm_mon - g_tm.tm_mon) + 1;
    if (num_months < 1) num_months = 1;
    if (num_months > 200) num_months = 200;

    #define NBUCKETS 10
    /* Age thresholds in seconds (same as gethodlwave) */
    const int64_t age_thresh[NBUCKETS] = {
        86400,              /* < 1 day */
        604800,             /* 1d - 1w */
        2592000,            /* 1w - 1m */
        2592000LL * 3,      /* 1 - 3m */
        2592000LL * 6,      /* 3 - 6m */
        31557600LL,         /* 6 - 12m */
        31557600LL * 2,     /* 1 - 2y */
        31557600LL * 3,     /* 2 - 3y */
        31557600LL * 5,     /* 3 - 5y */
        0                   /* > 5y (unlimited) */
    };

    static const char *bucket_labels[NBUCKETS] = {
        "< 1 day", "1d-1w", "1w-1m", "1-3m", "3-6m",
        "6-12m",   "1-2y",  "2-3y",  "3-5y", "> 5y"
    };

    /* grid[month][bucket] = total satoshi value */
    int64_t *grid = calloc((size_t)(num_months * NBUCKETS), sizeof(int64_t));
    if (!grid) {
        free(utxo_ts); free(utxo_val);
        json_set_str(result, "Out of memory");
        return false;
    }

    /* For each month, compute the timestamp at the 1st of that month */
    int64_t *month_ts = malloc((size_t)num_months * sizeof(int64_t));
    if (!month_ts) {
        free(grid); free(utxo_ts); free(utxo_val);
        json_set_str(result, "Out of memory");
        return false;
    }
    for (int m = 0; m < num_months; m++) {
        struct tm mtm = g_tm;
        mtm.tm_mon += m;
        mtm.tm_year += mtm.tm_mon / 12;
        mtm.tm_mon %= 12;
        mtm.tm_mday = 1;
        mtm.tm_hour = 0; mtm.tm_min = 0; mtm.tm_sec = 0;
        month_ts[m] = (int64_t)timegm(&mtm);
    }

    /* Classify each UTXO at each month */
    for (size_t u = 0; u < num_utxos; u++) {
        int64_t cts = utxo_ts[u];
        int64_t val = utxo_val[u];

        /* Find first month where this UTXO exists */
        int start_month = 0;
        if (cts > month_ts[0]) {
            /* Binary search for first month >= creation */
            int lo = 0, hi = num_months - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (month_ts[mid] < cts) lo = mid + 1;
                else hi = mid;
            }
            start_month = lo;
        }

        for (int m = start_month; m < num_months; m++) {
            int64_t age = month_ts[m] - cts;
            if (age < 0) continue;

            int bucket = NBUCKETS - 1;
            for (int b = 0; b < NBUCKETS - 1; b++) {
                if (age < age_thresh[b]) { bucket = b; break; }
            }
            grid[m * NBUCKETS + bucket] += val;
        }
    }

    free(utxo_ts);
    free(utxo_val);

    printf("gethodlwavechart: computed %d monthly columns\n", num_months);
    fflush(stdout);

    /* ── Step 3: Render 1920x1080 stacked-area PPM ──────────── */
    const int IMG_W = 1920;
    const int IMG_H = 1080;
    const int MARGIN_L = 70;
    const int MARGIN_R = 20;
    const int MARGIN_T = 50;
    const int MARGIN_B = 120;
    const int PLOT_W = IMG_W - MARGIN_L - MARGIN_R;
    const int PLOT_H = IMG_H - MARGIN_T - MARGIN_B;

    uint8_t *img = calloc((size_t)(IMG_W * IMG_H * 3), 1);
    if (!img) {
        free(grid); free(month_ts);
        json_set_str(result, "Out of memory");
        return false;
    }

    /* Background: near-black */
    for (int i = 0; i < IMG_W * IMG_H * 3; i += 3) {
        img[i] = 15; img[i+1] = 15; img[i+2] = 20;
    }

    /* Draw stacked area: for each pixel column in the plot area */
    for (int px = 0; px < PLOT_W; px++) {
        /* Map pixel to month */
        int m = (int)((double)px / PLOT_W * num_months);
        if (m >= num_months) m = num_months - 1;

        /* Compute total value at this month */
        int64_t month_total = 0;
        for (int b = 0; b < NBUCKETS; b++)
            month_total += grid[m * NBUCKETS + b];
        if (month_total == 0) continue;

        /* Stack bands bottom-to-top: oldest (bucket 9) at bottom,
         * youngest (bucket 0) at top */
        double y_bottom = 0.0;
        for (int b = NBUCKETS - 1; b >= 0; b--) {
            double frac = (double)grid[m * NBUCKETS + b] / (double)month_total;
            double y_top = y_bottom + frac;

            int py_bottom = MARGIN_T + PLOT_H - (int)(y_bottom * PLOT_H);
            int py_top = MARGIN_T + PLOT_H - (int)(y_top * PLOT_H);
            if (py_top < MARGIN_T) py_top = MARGIN_T;
            if (py_bottom > MARGIN_T + PLOT_H) py_bottom = MARGIN_T + PLOT_H;

            int x = MARGIN_L + px;
            for (int y = py_top; y < py_bottom; y++) {
                int off = (y * IMG_W + x) * 3;
                img[off]   = hodl_colors[b][0];
                img[off+1] = hodl_colors[b][1];
                img[off+2] = hodl_colors[b][2];
            }

            y_bottom = y_top;
        }
    }

    /* Consensus fork markers (cyan vertical lines) */
    int fork_heights[] = { 476969, 585318, 585322, 707000 };
    for (int f = 0; f < 4; f++) {
        int64_t fts = height_to_timestamp(fork_heights[f]);
        /* Find month index for this fork */
        int fm = 0;
        for (int m = 0; m < num_months; m++) {
            if (month_ts[m] <= fts) fm = m; else break;
        }
        int fx = MARGIN_L + (int)((double)fm / num_months * PLOT_W);
        if (fx >= MARGIN_L && fx < MARGIN_L + PLOT_W) {
            for (int y = MARGIN_T; y < MARGIN_T + PLOT_H; y++) {
                int off = (y * IMG_W + fx) * 3;
                /* Semi-transparent: blend with existing */
                img[off]   = (uint8_t)((img[off]   + 0) / 2);
                img[off+1] = (uint8_t)((img[off+1] + 255) / 2);
                img[off+2] = (uint8_t)((img[off+2] + 255) / 2);
            }
        }
    }

    /* Y-axis gridlines: 25%, 50%, 75% */
    for (int pct = 25; pct <= 75; pct += 25) {
        int y = MARGIN_T + PLOT_H - (int)((pct / 100.0) * PLOT_H);
        for (int x = MARGIN_L; x < MARGIN_L + PLOT_W; x += 3) {
            int off = (y * IMG_W + x) * 3;
            img[off] = 50; img[off+1] = 50; img[off+2] = 55;
        }
    }

    /* X-axis: year markers */
    for (int m = 0; m < num_months; m += 12) {
        int x = MARGIN_L + (int)((double)m / num_months * PLOT_W);
        for (int y = MARGIN_T; y < MARGIN_T + PLOT_H; y += 4) {
            int off = (y * IMG_W + x) * 3;
            img[off] = 50; img[off+1] = 50; img[off+2] = 55;
        }
    }

    /* Legend at bottom: 10 colored rectangles with labels */
    int legend_y = MARGIN_T + PLOT_H + 30;
    int legend_box = 18;
    int legend_spacing = (PLOT_W - 10) / NBUCKETS;
    for (int b = 0; b < NBUCKETS; b++) {
        int lx = MARGIN_L + b * legend_spacing;
        /* Draw color swatch */
        for (int dy = 0; dy < legend_box; dy++) {
            for (int dx = 0; dx < legend_box; dx++) {
                int y = legend_y + dy;
                int x = lx + dx;
                if (y < IMG_H && x < IMG_W) {
                    int off = (y * IMG_W + x) * 3;
                    img[off]   = hodl_colors[b][0];
                    img[off+1] = hodl_colors[b][1];
                    img[off+2] = hodl_colors[b][2];
                }
            }
        }
    }

    /* Write PNG */
    char png_path[512];
    snprintf(png_path, sizeof(png_path), "%s/hodlwave_chart.png",
             g_datadir ? g_datadir : ".");
    if (png_write_rgb(png_path, img, (uint32_t)IMG_W, (uint32_t)IMG_H))
        printf("gethodlwavechart: wrote %s\n", png_path);
    free(img);

    /* ── JSON response ──────────────────────────────────────── */
    json_set_object(result);
    json_push_kv_str(result, "chart_file", png_path);
    json_push_kv_int(result, "total_utxos", (int64_t)num_utxos);
    json_push_kv_int(result, "num_months", num_months);

    char buf[64];
    int64_t total_supply = 0;
    for (int b = 0; b < NBUCKETS; b++)
        total_supply += grid[(num_months - 1) * NBUCKETS + b];
    snprintf(buf, sizeof(buf), "%.8f", (double)total_supply / 1e8);
    json_push_kv_str(result, "total_supply_zcl", buf);

    /* Current age distribution (last month) */
    struct json_value dist = {0};
    json_set_array(&dist);
    int64_t cumul = 0;
    for (int b = NBUCKETS - 1; b >= 0; b--) {
        cumul += grid[(num_months - 1) * NBUCKETS + b];
        struct json_value e = {0};
        json_set_object(&e);
        json_push_kv_str(&e, "age", bucket_labels[b]);
        snprintf(buf, sizeof(buf), "%.8f",
                 (double)grid[(num_months - 1) * NBUCKETS + b] / 1e8);
        json_push_kv_str(&e, "zcl", buf);
        double cpct = total_supply > 0
            ? (double)cumul / (double)total_supply * 100.0 : 0.0;
        json_push_kv_real(&e, "cumulative_pct", cpct);
        json_push_back(&dist, &e);
        json_free(&e);
    }
    json_push_kv(result, "current_distribution", &dist);
    json_free(&dist);

    /* Legend labels for the chart */
    struct json_value legend = {0};
    json_set_array(&legend);
    for (int b = 0; b < NBUCKETS; b++) {
        struct json_value lb = {0};
        json_set_object(&lb);
        json_push_kv_str(&lb, "label", bucket_labels[b]);
        snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                 hodl_colors[b][0], hodl_colors[b][1], hodl_colors[b][2]);
        json_push_kv_str(&lb, "color", buf);
        json_push_back(&legend, &lb);
        json_free(&lb);
    }
    json_push_kv(result, "legend", &legend);
    json_free(&legend);

    free(grid);
    free(month_ts);

    #undef NBUCKETS
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

/* ── indexlegacy: import full chain from zclassicd LevelDB → our SQLite ── */

static struct node_db *g_node_db_bc = NULL;

void rpc_blockchain_set_node_db(struct node_db *ndb) { g_node_db_bc = ndb; }

static bool rpc_indexlegacy(const struct json_value *params, bool help,
                             struct json_value *result)
{
    RPC_HELP(help, result,
        "indexlegacy ( \"legacy_datadir\" )\n"
        "\nImport the ENTIRE blockchain from a legacy zclassicd node into SQLite.\n"
        "Reads LevelDB block index, walks block files, indexes all blocks,\n"
        "transactions, and UTXOs. The legacy node should be stopped first.\n"
        "\nArguments:\n"
        "1. legacy_datadir  (string, optional, default: ~/.zclassic)\n"
        "\nThis is a heavy operation — may take 30+ minutes for 3M blocks.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    const char *legacy_dir = rpc_permit_str(&p, 0, "legacy_datadir", NULL);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    char default_dir[512];
    if (!legacy_dir || legacy_dir[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(default_dir, sizeof(default_dir),
                 "%s/.zclassic", home ? home : "/root");
        legacy_dir = default_dir;
    }

    if (!g_node_db_bc || !g_node_db_bc->open) {
        json_set_str(result, "SQLite database not available");
        return false;
    }

    printf("indexlegacy: scanning block files from %s/blocks/\n", legacy_dir);
    fflush(stdout);

    /* Two-pass approach:
     * Pass 1: Scan all block files, record (height, file, offset, size)
     * Pass 2: Sort by height, process in order (so spends find UTXOs)
     *
     * Supports 100+ years of blocks (~21M blocks). Uses a sparse
     * height-indexed array that grows dynamically. */

    struct blk_loc {
        int file;
        uint32_t offset;
        uint32_t size;
    };

    int locs_cap = 4000000; /* initial, grows as needed */
    struct blk_loc *locs = calloc((size_t)locs_cap, sizeof(struct blk_loc));
    if (!locs) { json_set_str(result, "Out of memory"); return false; }

    int max_height = -1;
    int total_found = 0;

    /* Always full re-index — wipe first, then scan everything */
    printf("indexlegacy: Wiping all chain data for clean re-index...\n");
    fflush(stdout);
    node_db_exec(g_node_db_bc, "DELETE FROM utxos");
    node_db_exec(g_node_db_bc, "DELETE FROM transactions");
    node_db_exec(g_node_db_bc, "DELETE FROM blocks");

    /* ── Pass 1: Scan all blocks, build hash→(file,offset,size) map.
     * Then chain-walk from genesis using prev_hash to assign heights.
     * This works for ALL blocks including pre-BIP34 genesis era. ── */
    static const uint8_t ZCL_MAGIC[4] = {0x24, 0xe9, 0x27, 0x64};
    int64_t t_start = (int64_t)time(NULL);

    printf("indexlegacy: Pass 1 — scanning all blocks + building hash chain...\n");
    fflush(stdout);

    /* Hash→index map: store block hash, prev_hash, file pos for each block */
    struct raw_blk {
        uint8_t hash[32];
        uint8_t prev_hash[32];
        int file;
        uint32_t offset;
        uint32_t size;
    };
    int raw_cap = 4000000;
    struct raw_blk *raw = calloc((size_t)raw_cap, sizeof(struct raw_blk));
    if (!raw) { free(locs); json_set_str(result, "OOM"); return false; }
    int raw_count = 0;

    for (int file_num = 0; file_num < 1000; file_num++) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", legacy_dir, file_num);

        struct stat st;
        if (stat(path, &st) != 0) break;

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        size_t fsize = (size_t)st.st_size;
        uint8_t *data = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (data == MAP_FAILED) continue;

        size_t pos = 0;
        while (pos + 8 < fsize) {
            if (memcmp(data + pos, ZCL_MAGIC, 4) != 0) { pos++; continue; }
            uint32_t block_size;
            memcpy(&block_size, data + pos + 4, 4);
            if (block_size < 80 || block_size > 8 * 1024 * 1024 ||
                pos + 8 + block_size > fsize) { pos++; continue; }

            /* Parse just the block header (first ~1487 bytes) to get hash + prev_hash.
             * We need to deserialize to compute the block hash (double-SHA256 of header). */
            struct block blk;
            block_init(&blk);
            struct byte_stream bs;
            stream_init_from_data(&bs, data + pos + 8, block_size);
            bool ok = block_deserialize(&blk, &bs);
            stream_free(&bs);

            if (ok) {
                if (raw_count >= raw_cap) {
                    raw_cap *= 2;
                    raw = realloc(raw, (size_t)raw_cap * sizeof(struct raw_blk));
                }
                struct uint256 bh;
                block_header_get_hash(&blk.header, &bh);
                memcpy(raw[raw_count].hash, bh.data, 32);
                memcpy(raw[raw_count].prev_hash, blk.header.hashPrevBlock.data, 32);
                raw[raw_count].file = file_num;
                raw[raw_count].offset = (uint32_t)(pos + 8);
                raw[raw_count].size = block_size;
                raw_count++;
            }
            block_free(&blk);
            pos += 8 + block_size;
        }
        munmap(data, fsize);

        if (file_num % 10 == 0) {
            printf("  blk%05d.dat — %d blocks total\n", file_num, raw_count);
            fflush(stdout);
        }
    }

    printf("indexlegacy: Pass 1 found %d raw blocks. Building height chain...\n",
           raw_count);
    fflush(stdout);

    /* Build hash→index lookup (simple hash table) */
    #define HASH_BUCKETS 4194304  /* 4M buckets */
    struct hash_entry { int idx; int next; };
    int *hash_heads = malloc((size_t)HASH_BUCKETS * sizeof(int));
    struct hash_entry *hash_nodes = malloc((size_t)raw_count * sizeof(struct hash_entry));
    if (!hash_heads || !hash_nodes) {
        free(raw); free(locs); free(hash_heads); free(hash_nodes);
        json_set_str(result, "OOM"); return false;
    }
    memset(hash_heads, -1, (size_t)HASH_BUCKETS * sizeof(int));

    for (int i = 0; i < raw_count; i++) {
        uint32_t bucket;
        memcpy(&bucket, raw[i].hash, 4);
        bucket %= HASH_BUCKETS;
        hash_nodes[i].idx = i;
        hash_nodes[i].next = hash_heads[bucket];
        hash_heads[bucket] = i;
    }

    /* Find genesis block (prev_hash = all zeros) */
    int genesis_idx = -1;
    uint8_t zero32[32] = {0};
    for (int i = 0; i < raw_count; i++) {
        if (memcmp(raw[i].prev_hash, zero32, 32) == 0) {
            genesis_idx = i;
            break;
        }
    }

    if (genesis_idx < 0) {
        free(raw); free(locs); free(hash_heads); free(hash_nodes);
        json_set_str(result, "Genesis block not found");
        return false;
    }

    /* Walk the chain from genesis, assigning heights.
     * For each block, find the next block whose prev_hash matches our hash. */
    int *height_map = calloc((size_t)raw_count, sizeof(int));
    for (int i = 0; i < raw_count; i++) height_map[i] = -1;
    height_map[genesis_idx] = 0;

    /* Build child→parent index (reverse: for each hash, find blocks pointing to it) */
    /* Actually simpler: walk forward. Start at genesis, find who points to us. */
    /* Better approach: build prev_hash→index map, then walk from genesis forward */
    int *prev_heads = malloc((size_t)HASH_BUCKETS * sizeof(int));
    struct hash_entry *prev_nodes = malloc((size_t)raw_count * sizeof(struct hash_entry));
    memset(prev_heads, -1, (size_t)HASH_BUCKETS * sizeof(int));
    for (int i = 0; i < raw_count; i++) {
        uint32_t bucket;
        memcpy(&bucket, raw[i].prev_hash, 4);
        bucket %= HASH_BUCKETS;
        prev_nodes[i].idx = i;
        prev_nodes[i].next = prev_heads[bucket];
        prev_heads[bucket] = i;
    }

    /* BFS from genesis: find children (blocks whose prev_hash = our hash) */
    int *queue = malloc((size_t)raw_count * sizeof(int));
    int q_head = 0, q_tail = 0;
    queue[q_tail++] = genesis_idx;
    int assigned = 0;

    while (q_head < q_tail) {
        int cur = queue[q_head++];
        int cur_height = height_map[cur];

        /* Store in height→location array */
        while (cur_height >= locs_cap) {
            int new_cap = locs_cap * 2;
            struct blk_loc *tmp = realloc(locs,
                (size_t)new_cap * sizeof(struct blk_loc));
            if (!tmp) break;
            memset(tmp + locs_cap, 0,
                   (size_t)(new_cap - locs_cap) * sizeof(struct blk_loc));
            locs = tmp;
            locs_cap = new_cap;
        }
        if (cur_height < locs_cap) {
            locs[cur_height].file = raw[cur].file;
            locs[cur_height].offset = raw[cur].offset;
            locs[cur_height].size = raw[cur].size;
            if (cur_height > max_height) max_height = cur_height;
            total_found++;
        }
        assigned++;

        /* Find children: blocks whose prev_hash == our hash */
        uint32_t bucket;
        memcpy(&bucket, raw[cur].hash, 4);
        bucket %= HASH_BUCKETS;
        for (int e = prev_heads[bucket]; e >= 0; e = prev_nodes[e].next) {
            int child = prev_nodes[e].idx;
            if (memcmp(raw[child].prev_hash, raw[cur].hash, 32) == 0 &&
                height_map[child] < 0) {
                height_map[child] = cur_height + 1;
                queue[q_tail++] = child;
            }
        }
    }

    free(queue);
    free(prev_heads);
    free(prev_nodes);
    free(hash_heads);
    free(hash_nodes);
    free(height_map);
    free(raw);

    int64_t pass1_time = (int64_t)time(NULL) - t_start;
    printf("indexlegacy: Pass 1 complete — %d blocks chained, max height %d, "
           "%d assigned (%" PRId64 "s)\n",
           raw_count, max_height, assigned, pass1_time);
    fflush(stdout);

    if (max_height < 0) {
        free(locs);
        json_set_str(result, "No blocks found");
        return false;
    }

    /* ── Wipe existing UTXO data (stale from out-of-order indexing) ── */
    /* ── Pass 2: Process blocks in HEIGHT ORDER ── */
    printf("indexlegacy: Pass 2 — indexing %d blocks in height order...\n",
           total_found);
    fflush(stdout);

    int64_t t_pass2 = (int64_t)time(NULL);
    int blocks_indexed = 0;
    int txs_indexed = 0;
    int utxos_created = 0;
    int utxos_spent = 0;
    int last_file = -1;
    uint8_t *mmap_data = NULL;
    size_t mmap_size = 0;

    node_db_begin(g_node_db_bc);

    for (int h = 0; h <= max_height; h++) {
        if (locs[h].size == 0) continue; /* no block at this height */

        /* mmap the block file if not already mapped */
        if (locs[h].file != last_file) {
            if (mmap_data) munmap(mmap_data, mmap_size);
            char path[1200];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     legacy_dir, locs[h].file);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            mmap_size = (size_t)st.st_size;
            mmap_data = mmap(NULL, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mmap_data == MAP_FAILED) { mmap_data = NULL; continue; }
            last_file = locs[h].file;
        }
        if (!mmap_data) continue;
        if (locs[h].offset + locs[h].size > mmap_size) continue;

        /* Deserialize block */
        struct block blk;
        block_init(&blk);
        struct byte_stream bs;
        stream_init_from_data(&bs, mmap_data + locs[h].offset, locs[h].size);
        if (!block_deserialize(&blk, &bs)) {
            stream_free(&bs);
            block_free(&blk);
            continue;
        }
        stream_free(&bs);

        /* Compute block hash */
        struct uint256 block_hash;
        block_header_get_hash(&blk.header, &block_hash);

        /* Index block */
        struct db_block db_blk;
        memset(&db_blk, 0, sizeof(db_blk));
        memcpy(db_blk.hash, block_hash.data, 32);
        db_blk.height = h;
        memcpy(db_blk.merkle_root, blk.header.hashMerkleRoot.data, 32);
        memcpy(db_blk.sapling_root, blk.header.hashFinalSaplingRoot.data, 32);
        memcpy(db_blk.nonce, blk.header.nNonce.data, 32);
        memcpy(db_blk.prev_hash, blk.header.hashPrevBlock.data, 32);
        db_blk.version = blk.header.nVersion;
        db_blk.time = blk.header.nTime;
        db_blk.bits = blk.header.nBits;
        db_blk.num_tx = (int)blk.num_vtx;
        db_blk.file_num = locs[h].file;
        db_blk.data_pos = (int)locs[h].offset;
        db_blk.solution = blk.header.nSolution;
        db_blk.solution_len = blk.header.nSolutionSize;
        db_blk.status = 29; /* BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA */

        db_block_save(g_node_db_bc, &db_blk);
        blocks_indexed++;

        /* Index transactions + UTXOs */
        for (size_t i = 0; i < blk.num_vtx; i++) {
            const struct transaction *tx = &blk.vtx[i];

            struct db_tx_index db_tx;
            memset(&db_tx, 0, sizeof(db_tx));
            memcpy(db_tx.txid, tx->hash.data, 32);
            memcpy(db_tx.block_hash, block_hash.data, 32);
            db_tx.block_height = h;
            db_tx.tx_index = (int)i;
            db_tx.file_num = locs[h].file;
            db_tx.file_pos = (int)locs[h].offset;
            db_tx.is_coinbase = (i == 0);
            db_tx_save(g_node_db_bc, &db_tx);
            txs_indexed++;

            /* Spend inputs (delete UTXOs) — works because we process in height order */
            if (i > 0) {
                for (size_t j = 0; j < tx->num_vin; j++) {
                    db_utxo_delete(g_node_db_bc,
                        tx->vin[j].prevout.hash.data,
                        tx->vin[j].prevout.n);
                    utxos_spent++;
                }
            }

            /* Index OP_RETURN outputs for ZSLP token tracking */
            if (tx->num_vout > 0 &&
                tx->vout[0].script_pub_key.size > 0 &&
                tx->vout[0].script_pub_key.data[0] == 0x6a) {
                const uint8_t *script = tx->vout[0].script_pub_key.data;
                size_t script_len = tx->vout[0].script_pub_key.size;
                struct slp_message slp;
                if (slp_parse(script, script_len, &slp)) {
                    if (slp.type == SLP_TX_GENESIS) {
                        /* Store token in zslp_tokens table */
                        sqlite3_stmt *ts = NULL;
                        if (sqlite3_prepare_v2(g_node_db_bc->db,
                                "INSERT OR IGNORE INTO zslp_tokens"
                                "(token_id,ticker,name,decimals,document_url,"
                                "genesis_height,total_minted) VALUES(?,?,?,?,?,?,?)",
                                -1, &ts, NULL) == SQLITE_OK) {
                            sqlite3_bind_blob(ts, 1, tx->hash.data, 32, SQLITE_STATIC);
                            sqlite3_bind_text(ts, 2, slp.ticker, -1, SQLITE_STATIC);
                            sqlite3_bind_text(ts, 3, slp.name, -1, SQLITE_STATIC);
                            sqlite3_bind_int(ts, 4, slp.decimals);
                            sqlite3_bind_text(ts, 5, slp.document_url, -1, SQLITE_STATIC);
                            sqlite3_bind_int(ts, 6, h);
                            sqlite3_bind_int64(ts, 7, (int64_t)slp.initial_quantity);
                            sqlite3_step(ts);
                            sqlite3_finalize(ts);
                        }
                    }
                    /* Store transfer records — one per output for SEND */
                    const uint8_t *tok_id = (slp.type == SLP_TX_GENESIS)
                        ? tx->hash.data : slp.token_id.data;
                    int num_records = 1;
                    if (slp.type == SLP_TX_SEND) num_records = slp.num_outputs;
                    if (num_records < 1) num_records = 1;

                    for (int q = 0; q < num_records; q++) {
                        int64_t amount = 0;
                        if (slp.type == SLP_TX_GENESIS)
                            amount = (int64_t)slp.initial_quantity;
                        else if (slp.type == SLP_TX_MINT)
                            amount = (int64_t)slp.additional_quantity;
                        else if (slp.type == SLP_TX_SEND && q < slp.num_outputs)
                            amount = (int64_t)slp.output_quantities[q];

                        /* Extract recipient address from vout[q+1] if P2PKH */
                        uint8_t to_addr[20] = {0};
                        bool has_to = false;
                        int out_idx = q + 1; /* SLP outputs start at vout 1 */
                        if (slp.type == SLP_TX_GENESIS) out_idx = 1;
                        if (out_idx < (int)tx->num_vout) {
                            const uint8_t *sd = tx->vout[out_idx].script_pub_key.data;
                            size_t sl = tx->vout[out_idx].script_pub_key.size;
                            if (sl == 25 && sd[0] == 0x76 && sd[1] == 0xa9 &&
                                sd[2] == 0x14 && sd[23] == 0x88 && sd[24] == 0xac) {
                                memcpy(to_addr, sd + 3, 20);
                                has_to = true;
                            }
                        }

                        sqlite3_stmt *xs = NULL;
                        if (sqlite3_prepare_v2(g_node_db_bc->db,
                                "INSERT OR IGNORE INTO zslp_transfers"
                                "(txid,block_height,token_id,tx_type,amount,vout,to_addr)"
                                " VALUES(?,?,?,?,?,?,?)",
                                -1, &xs, NULL) == SQLITE_OK) {
                            sqlite3_bind_blob(xs, 1, tx->hash.data, 32, SQLITE_STATIC);
                            sqlite3_bind_int(xs, 2, h);
                            sqlite3_bind_blob(xs, 3, tok_id, 32, SQLITE_STATIC);
                            sqlite3_bind_int(xs, 4, (int)slp.type);
                            sqlite3_bind_int64(xs, 5, amount);
                            sqlite3_bind_int(xs, 6, q);
                            if (has_to)
                                sqlite3_bind_blob(xs, 7, to_addr, 20, SQLITE_STATIC);
                            else
                                sqlite3_bind_null(xs, 7);
                            sqlite3_step(xs);
                            sqlite3_finalize(xs);
                        }
                    }
                }
            }

            /* Create output UTXOs */
            for (size_t j = 0; j < tx->num_vout; j++) {
                if (tx->vout[j].value == 0 &&
                    tx->vout[j].script_pub_key.size > 0 &&
                    tx->vout[j].script_pub_key.data[0] == 0x6a)
                    continue;

                struct db_utxo u;
                memset(&u, 0, sizeof(u));
                memcpy(u.txid, tx->hash.data, 32);
                u.vout = (uint32_t)j;
                u.value = tx->vout[j].value;
                u.script = (uint8_t *)tx->vout[j].script_pub_key.data;
                u.script_len = tx->vout[j].script_pub_key.size;
                u.height = h;
                u.is_coinbase = (i == 0);

                const uint8_t *sd = tx->vout[j].script_pub_key.data;
                size_t sl = tx->vout[j].script_pub_key.size;
                if (sl == 25 && sd[0] == 0x76 && sd[1] == 0xa9 &&
                    sd[2] == 0x14 && sd[23] == 0x88 && sd[24] == 0xac) {
                    memcpy(u.address_hash, sd + 3, 20);
                    u.has_address = true;
                    u.script_type = SCRIPT_P2PKH;
                } else if (sl == 23 && sd[0] == 0xa9 && sd[1] == 0x14 &&
                           sd[22] == 0x87) {
                    memcpy(u.address_hash, sd + 2, 20);
                    u.has_address = true;
                    u.script_type = SCRIPT_P2SH;
                }

                db_utxo_save(g_node_db_bc, &u);
                utxos_created++;
            }
        }

        block_free(&blk);

        if (blocks_indexed % 10000 == 0 && blocks_indexed > 0) {
            node_db_commit(g_node_db_bc);
            int64_t elapsed = (int64_t)time(NULL) - t_pass2;
            double rate = elapsed > 0 ?
                (double)blocks_indexed / (double)elapsed : 0;
            int remaining = total_found - blocks_indexed;
            int eta = rate > 0 ? (int)((double)remaining / rate) : 0;
            printf("  height %d/%d — %d txs, %d utxos (+%d -%d) "
                   "(%.0f blk/s, ETA %dm%ds)\n",
                   h, max_height, txs_indexed, utxos_created - utxos_spent,
                   utxos_created, utxos_spent,
                   rate, eta / 60, eta % 60);
            fflush(stdout);
            node_db_begin(g_node_db_bc);
        }
    }

    if (mmap_data) munmap(mmap_data, mmap_size);
    node_db_commit(g_node_db_bc);

    /* Update tip */
    {
        struct db_block tip_blk;
        if (db_block_find_by_height(g_node_db_bc, max_height, &tip_blk))
            node_db_sync_set_tip(g_node_db_bc, tip_blk.hash, max_height);
    }

    int64_t total_elapsed = (int64_t)time(NULL) - t_start;
    int net_utxos = utxos_created - utxos_spent;
    printf("indexlegacy: COMPLETE — %d blocks, %d txs, %d net UTXOs "
           "(+%d -%d) in %" PRId64 "m%" PRId64 "s\n",
           blocks_indexed, txs_indexed, net_utxos,
           utxos_created, utxos_spent,
           total_elapsed / 60, total_elapsed % 60);
    fflush(stdout);

    free(locs);

    json_set_object(result);
    json_push_kv_int(result, "blocks_indexed", blocks_indexed);
    json_push_kv_int(result, "transactions_indexed", txs_indexed);
    json_push_kv_int(result, "utxos_net", net_utxos);
    json_push_kv_int(result, "utxos_created", utxos_created);
    json_push_kv_int(result, "utxos_spent", utxos_spent);
    json_push_kv_int(result, "max_height", max_height);
    json_push_kv_int(result, "elapsed_seconds", total_elapsed);
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
        { "blockchain", "gethodlwaveimage",      rpc_gethodlwaveimage,      true },
        { "blockchain", "gethodlwavetimeline",  rpc_gethodlwavetimeline,   true },
        { "blockchain", "gethodlwavechart",     rpc_gethodlwavechart,      true },
        { "blockchain", "reindexchainstate",    rpc_reindexchainstate,     false },
        { "blockchain", "indexlegacy",          rpc_indexlegacy,            false },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
