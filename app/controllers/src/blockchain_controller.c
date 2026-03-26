/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "views/format_helpers.h"
#include "controllers/blockchain_controller.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/mmr.h"
#include "chain/pow.h"
#include "coins/utxo_commitment.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/undo.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "json/json.h"
#include "models/zslp.h"
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
#include "crypto/sha3.h"
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
#include <pthread.h>

static struct main_state *g_main_state = NULL;
static struct tx_mempool *g_mempool = NULL;
static const char *g_datadir = NULL;
static struct coins_view_db *g_coins_db = NULL;
static struct coins_view_cache *g_coins_tip = NULL;
static struct node_db *g_node_db_bc = NULL;

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

    if (!g_node_db_bc || !g_node_db_bc->open) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush in-memory UTXO cache to SQLite for accurate totals */
    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    int tip_height = active_chain_height(&g_main_state->chain_active);
    struct block_index *tip = active_chain_tip(&g_main_state->chain_active);

    int64_t total_amount = 0;
    int64_t num_txs = 0;
    int64_t num_txouts = 0;

    /* Query UTXO set from SQLite */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(g_node_db_bc->db,
        "SELECT COUNT(DISTINCT txid), COUNT(*), COALESCE(SUM(value),0)"
        " FROM utxos", -1, &s, NULL);
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        num_txs = sqlite3_column_int64(s, 0);
        num_txouts = sqlite3_column_int64(s, 1);
        total_amount = sqlite3_column_int64(s, 2);
    }
    sqlite3_finalize(s);

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
             (long long)(total_amount / ZATOSHI_PER_ZCL),
             (long long)(total_amount % ZATOSHI_PER_ZCL));
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

    if (!g_node_db_bc || !g_node_db_bc->open) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!g_main_state || !active_chain_tip(&g_main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush in-memory UTXO cache to SQLite for accurate totals */
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

    /* Query UTXOs from SQLite for HODL wave analysis */
    sqlite3_stmt *hodl_s = NULL;
    sqlite3_prepare_v2(g_node_db_bc->db,
        "SELECT height, value FROM utxos",
        -1, &hodl_s, NULL);

    while (hodl_s && sqlite3_step(hodl_s) == SQLITE_ROW) {
        int height = sqlite3_column_int(hodl_s, 0);
        int64_t value = sqlite3_column_int64(hodl_s, 1);

        if (height < 0 || height > tip_height + 100) {
            skipped++;
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

        /* Each SQLite row is one UTXO */
        bucket_values[bucket_idx] += value;
        bucket_counts[bucket_idx]++;
        if (age_secs >= SECS_PER_YEAR) {
            over_1y_value += value;
            over_1y_count++;
        }
        total_txs++;
    }
    sqlite3_finalize(hodl_s);

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
    double total_zcl = (double)total_value / (double)ZATOSHI_PER_ZCL;
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

        double bval = (double)bucket_values[b] / (double)ZATOSHI_PER_ZCL;
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
             (long long)(total_value / ZATOSHI_PER_ZCL),
             (long long)(total_value % ZATOSHI_PER_ZCL));
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
                 (long long)(hodl_values[b] / ZATOSHI_PER_ZCL),
                 (long long)(hodl_values[b] % ZATOSHI_PER_ZCL));
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
                    size_t new_cap = cap * 2;
                    int64_t *new_ts = realloc(utxo_ts, new_cap * sizeof(int64_t));
                    if (!new_ts) {
                        free(utxo_ts); free(utxo_val);
                        coins_free(&c); db_iter_free(&it);
                        json_set_str(result, "Out of memory");
                        return false;
                    }
                    utxo_ts = new_ts;
                    int64_t *new_val = realloc(utxo_val, new_cap * sizeof(int64_t));
                    if (!new_val) {
                        free(utxo_ts); free(utxo_val);
                        coins_free(&c); db_iter_free(&it);
                        json_set_str(result, "Out of memory");
                        return false;
                    }
                    utxo_ts = new_ts;
                    utxo_val = new_val;
                    cap = new_cap;
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

    /* Step 3.5: Reset sapling tree state — must replay from empty.
     * Use the global node_db (set later in file via rpc_blockchain_set_node_db). */
    {
        extern struct node_db *g_active_node_db;
        if (g_active_node_db && g_active_node_db->open) {
            node_db_state_set(g_active_node_db, "sapling_tree", NULL, 0);
            node_db_state_set(g_active_node_db, "sapling_tree_rescan_height", NULL, 0);
        }
    }

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

/* ── Phase B data structures for parallel block extraction ── */

#define N_INDEX_THREADS 4
#define IDX_BATCH_CAP   4000000

struct idx_tx_input {
    uint8_t txid[32]; uint8_t prev_txid[32];
    uint32_t vin_index; uint32_t prev_vout; int height;
};
struct idx_tx_output {
    uint8_t txid[32]; int64_t value;
    uint8_t addr_hash[20]; bool has_addr;
    uint32_t vout; int script_type; int height;
};
struct idx_joinsplit {
    uint8_t txid[32]; uint8_t anchor[32];
    uint8_t nullifiers[2][32];
    int64_t vpub_old; int64_t vpub_new;
    uint32_t js_index; int height;
};
struct idx_sapling_spend {
    uint8_t txid[32]; uint8_t cv[32]; uint8_t anchor[32];
    uint8_t nullifier[32]; uint8_t rk[32];
    uint32_t spend_index; int height;
};
struct idx_sapling_output {
    uint8_t txid[32]; uint8_t cv[32]; uint8_t cm[32];
    uint8_t ephemeral_key[32];
    uint32_t output_index; int height;
};
struct idx_opret {
    uint8_t txid[32]; uint8_t script[256];
    size_t script_len; int is_slp; int height;
};
struct idx_block_shielded {
    int height; int64_t sprout_value; int64_t sapling_value;
    uint8_t block_hash[32];
    uint32_t num_js; uint32_t num_ss; uint32_t num_so;
    uint32_t num_tx;
};

struct blk_loc {
    int file;
    uint32_t offset;
    uint32_t size;
};

struct worker_ctx {
    int thread_id;
    int height_from, height_to;
    const struct blk_loc *locs;
    int max_height;
    const char *legacy_dir;

    /* Output arrays -- allocated by thread */
    struct idx_tx_input *inputs;      int num_inputs;      int cap_inputs;
    struct idx_tx_output *outputs;    int num_outputs;     int cap_outputs;
    struct idx_joinsplit *joinsplits; int num_joinsplits;  int cap_joinsplits;
    struct idx_sapling_spend *sspends;   int num_sspends;  int cap_sspends;
    struct idx_sapling_output *soutputs; int num_soutputs; int cap_soutputs;
    struct idx_opret *oprets;         int num_oprets;      int cap_oprets;
    struct idx_block_shielded *blocks_sh; int num_blocks_sh; int cap_blocks_sh;
};

/* Macro to grow a per-thread array if at capacity */
#define IDX_GROW(arr, num, cap, type) do {            \
    if ((num) >= (cap)) {                             \
        (cap) = (cap) < 1024 ? 1024 : (cap) * 2;     \
        void *_tmp = realloc((arr), (size_t)(cap) * sizeof(type)); \
        if (!_tmp) { free(arr); (arr) = NULL; (cap) = 0; break; } \
        (arr) = _tmp;                                 \
    }                                                 \
} while (0)

static void *index_worker(void *arg) {
    struct worker_ctx *ctx = arg;

    /* Allocate output arrays */
    ctx->cap_inputs = IDX_BATCH_CAP;
    ctx->cap_outputs = IDX_BATCH_CAP;
    ctx->cap_joinsplits = IDX_BATCH_CAP / 4;
    ctx->cap_sspends = IDX_BATCH_CAP / 4;
    ctx->cap_soutputs = IDX_BATCH_CAP / 4;
    ctx->cap_oprets = IDX_BATCH_CAP / 8;
    ctx->cap_blocks_sh = (ctx->height_to - ctx->height_from + 2);

    ctx->inputs    = malloc((size_t)ctx->cap_inputs    * sizeof(*ctx->inputs));
    ctx->outputs   = malloc((size_t)ctx->cap_outputs   * sizeof(*ctx->outputs));
    ctx->joinsplits= malloc((size_t)ctx->cap_joinsplits* sizeof(*ctx->joinsplits));
    ctx->sspends   = malloc((size_t)ctx->cap_sspends   * sizeof(*ctx->sspends));
    ctx->soutputs  = malloc((size_t)ctx->cap_soutputs  * sizeof(*ctx->soutputs));
    ctx->oprets    = malloc((size_t)ctx->cap_oprets    * sizeof(*ctx->oprets));
    ctx->blocks_sh = malloc((size_t)ctx->cap_blocks_sh * sizeof(*ctx->blocks_sh));

    ctx->num_inputs = ctx->num_outputs = ctx->num_joinsplits = 0;
    ctx->num_sspends = ctx->num_soutputs = ctx->num_oprets = 0;
    ctx->num_blocks_sh = 0;

    int cur_file = -1;
    uint8_t *mdata = NULL;
    size_t msize = 0;

    printf("  Phase B: thread %d processing heights %d-%d\n",
           ctx->thread_id, ctx->height_from, ctx->height_to);
    fflush(stdout);

    for (int h = ctx->height_from; h <= ctx->height_to; h++) {
        if (h > ctx->max_height) break;
        if (ctx->locs[h].size == 0) continue;

        /* mmap block file if needed */
        if (ctx->locs[h].file != cur_file) {
            if (mdata) munmap(mdata, msize);
            char path[1200];
            snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
                     ctx->legacy_dir, ctx->locs[h].file);
            struct stat st2;
            if (stat(path, &st2) != 0) { mdata = NULL; continue; }
            int fd2 = open(path, O_RDONLY);
            if (fd2 < 0) { mdata = NULL; continue; }
            msize = (size_t)st2.st_size;
            mdata = mmap(NULL, msize, PROT_READ, MAP_PRIVATE, fd2, 0);
            close(fd2);
            if (mdata == MAP_FAILED) { mdata = NULL; continue; }
            cur_file = ctx->locs[h].file;
        }
        if (!mdata) continue;
        if (ctx->locs[h].offset + ctx->locs[h].size > msize) continue;

        /* Deserialize block */
        struct block blk2;
        block_init(&blk2);
        struct byte_stream bs2;
        stream_init_from_data(&bs2, mdata + ctx->locs[h].offset,
                              ctx->locs[h].size);
        if (!block_deserialize(&blk2, &bs2)) {
            stream_free(&bs2);
            block_free(&blk2);
            continue;
        }
        stream_free(&bs2);

        struct uint256 bh2;
        block_header_get_hash(&blk2.header, &bh2);

        /* Per-block shielded accumulators */
        int64_t bk_sprout = 0, bk_sapling = 0;
        uint32_t bk_njs = 0, bk_nss = 0, bk_nso = 0;

        for (size_t ti = 0; ti < blk2.num_vtx; ti++) {
            const struct transaction *tx2 = &blk2.vtx[ti];

            /* Transparent inputs (except coinbase) */
            if (ti > 0) {
                for (size_t j = 0; j < tx2->num_vin; j++) {
                    IDX_GROW(ctx->inputs, ctx->num_inputs,
                             ctx->cap_inputs, struct idx_tx_input);
                    struct idx_tx_input *inp = &ctx->inputs[ctx->num_inputs++];
                    memcpy(inp->txid, tx2->hash.data, 32);
                    memcpy(inp->prev_txid, tx2->vin[j].prevout.hash.data, 32);
                    inp->vin_index = (uint32_t)j;
                    inp->prev_vout = tx2->vin[j].prevout.n;
                    inp->height = h;
                }
            }

            /* JoinSplits + sprout nullifiers */
            for (size_t j = 0; j < tx2->num_joinsplit; j++) {
                struct js_description *js = &tx2->v_joinsplit[j];
                bk_sprout += js->vpub_old - js->vpub_new;

                IDX_GROW(ctx->joinsplits, ctx->num_joinsplits,
                         ctx->cap_joinsplits, struct idx_joinsplit);
                struct idx_joinsplit *ij = &ctx->joinsplits[ctx->num_joinsplits++];
                memcpy(ij->txid, tx2->hash.data, 32);
                memcpy(ij->anchor, js->anchor.data, 32);
                for (int nf = 0; nf < ZC_NUM_JS_INPUTS; nf++)
                    memcpy(ij->nullifiers[nf], js->nullifiers[nf].data, 32);
                ij->vpub_old = js->vpub_old;
                ij->vpub_new = js->vpub_new;
                ij->js_index = (uint32_t)j;
                ij->height = h;
                bk_njs++;
            }

            /* Sapling spends */
            for (size_t j = 0; j < tx2->num_shielded_spend; j++) {
                struct spend_description *sd = &tx2->v_shielded_spend[j];
                IDX_GROW(ctx->sspends, ctx->num_sspends,
                         ctx->cap_sspends, struct idx_sapling_spend);
                struct idx_sapling_spend *isp = &ctx->sspends[ctx->num_sspends++];
                memcpy(isp->txid, tx2->hash.data, 32);
                memcpy(isp->cv, sd->cv.data, 32);
                memcpy(isp->anchor, sd->anchor.data, 32);
                memcpy(isp->nullifier, sd->nullifier.data, 32);
                memcpy(isp->rk, sd->rk.data, 32);
                isp->spend_index = (uint32_t)j;
                isp->height = h;
                bk_nss++;
            }

            /* Sapling outputs */
            for (size_t j = 0; j < tx2->num_shielded_output; j++) {
                struct output_description *od = &tx2->v_shielded_output[j];
                IDX_GROW(ctx->soutputs, ctx->num_soutputs,
                         ctx->cap_soutputs, struct idx_sapling_output);
                struct idx_sapling_output *iso = &ctx->soutputs[ctx->num_soutputs++];
                memcpy(iso->txid, tx2->hash.data, 32);
                memcpy(iso->cv, od->cv.data, 32);
                memcpy(iso->cm, od->cm.data, 32);
                memcpy(iso->ephemeral_key, od->ephemeral_key.data, 32);
                iso->output_index = (uint32_t)j;
                iso->height = h;
                bk_nso++;
            }

            /* Sapling value balance */
            bk_sapling += tx2->value_balance;

            /* Transparent outputs */
            for (size_t j = 0; j < tx2->num_vout; j++) {
                const uint8_t *scr = tx2->vout[j].script_pub_key.data;
                size_t scr_len = tx2->vout[j].script_pub_key.size;

                IDX_GROW(ctx->outputs, ctx->num_outputs,
                         ctx->cap_outputs, struct idx_tx_output);
                struct idx_tx_output *ot = &ctx->outputs[ctx->num_outputs++];
                memcpy(ot->txid, tx2->hash.data, 32);
                ot->value = tx2->vout[j].value;
                ot->vout = (uint32_t)j;
                ot->height = h;
                ot->has_addr = false;
                ot->script_type = 0;

                if (scr_len == 25 && scr[0] == 0x76 && scr[1] == 0xa9 &&
                    scr[2] == 0x14 && scr[23] == 0x88 && scr[24] == 0xac) {
                    memcpy(ot->addr_hash, scr + 3, 20);
                    ot->has_addr = true;
                    ot->script_type = SCRIPT_P2PKH;
                } else if (scr_len == 23 && scr[0] == 0xa9 && scr[1] == 0x14 &&
                           scr[22] == 0x87) {
                    memcpy(ot->addr_hash, scr + 2, 20);
                    ot->has_addr = true;
                    ot->script_type = SCRIPT_P2SH;
                }
            }

            /* OP_RETURN (first per tx) */
            for (size_t j = 0; j < tx2->num_vout; j++) {
                if (tx2->vout[j].script_pub_key.size > 0 &&
                    tx2->vout[j].script_pub_key.data[0] == 0x6a) {
                    const uint8_t *scr = tx2->vout[j].script_pub_key.data;
                    size_t scr_len = tx2->vout[j].script_pub_key.size;
                    struct slp_message slp_chk;
                    int is_slp_val = slp_parse(scr, scr_len, &slp_chk) ? 1 : 0;

                    IDX_GROW(ctx->oprets, ctx->num_oprets,
                             ctx->cap_oprets, struct idx_opret);
                    struct idx_opret *op = &ctx->oprets[ctx->num_oprets++];
                    memcpy(op->txid, tx2->hash.data, 32);
                    op->script_len = scr_len > 256 ? 256 : scr_len;
                    memcpy(op->script, scr, op->script_len);
                    op->is_slp = is_slp_val;
                    op->height = h;
                    break; /* only first OP_RETURN per tx */
                }
            }
        }

        /* Record per-block shielded data */
        IDX_GROW(ctx->blocks_sh, ctx->num_blocks_sh,
                 ctx->cap_blocks_sh, struct idx_block_shielded);
        struct idx_block_shielded *bsh = &ctx->blocks_sh[ctx->num_blocks_sh++];
        bsh->height = h;
        bsh->sprout_value = bk_sprout;
        bsh->sapling_value = bk_sapling;
        memcpy(bsh->block_hash, bh2.data, 32);
        bsh->num_js = bk_njs;
        bsh->num_ss = bk_nss;
        bsh->num_so = bk_nso;
        bsh->num_tx = (uint32_t)blk2.num_vtx;

        block_free(&blk2);
    }

    if (mdata) munmap(mdata, msize);
    return NULL;
}

/* ── importchainstate: read UTXO set from external LevelDB chainstate ── */

static bool rpc_importchainstate(const struct json_value *params, bool help,
                                   struct json_value *result)
{
    RPC_HELP(help, result,
        "importchainstate \"chainstate_path\"\n"
        "\nRebuild the UTXO index from an external LevelDB chainstate directory.\n"
        "Use this to import the complete UTXO set from a zclassicd node:\n"
        "  importchainstate /home/user/.zclassic/chainstate\n"
        "\nThis replaces all UTXOs in SQLite with those from the given chainstate.\n"
        "The source node should be stopped to avoid LevelDB lock conflicts.\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 1, 1);
    const char *cs_path = rpc_require_str(&p, 0, "chainstate_path");
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    if (!g_node_db_bc || !g_node_db_bc->open) {
        json_set_str(result, "Node database not open");
        return false;
    }

    printf("importchainstate: opening %s...\n", cs_path);
    fflush(stdout);

    struct coins_view_db ext_db;
    memset(&ext_db, 0, sizeof(ext_db));
    if (!coins_view_db_open(&ext_db, cs_path, 256, false, false)) {
        json_set_str(result, "Cannot open chainstate LevelDB");
        return false;
    }

    int count = node_db_sync_import_utxos(g_node_db_bc, &ext_db);
    coins_view_db_close(&ext_db);

    if (count < 0) {
        json_set_str(result, "Import failed");
        return false;
    }

    /* Rebuild wallet_utxos and addresses from new UTXO set */
    sqlite3_exec(g_node_db_bc->db, "BEGIN", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db,
        "DELETE FROM wallet_utxos", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db,
        "INSERT INTO wallet_utxos "
        "(txid, vout, value, address_hash, script, height, is_coinbase) "
        "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
        "u.height, u.is_coinbase "
        "FROM utxos u INNER JOIN wallet_keys wk "
        "ON u.address_hash = wk.pubkey_hash",
        NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db,
        "DELETE FROM addresses", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "COMMIT", NULL, NULL, NULL);

    json_set_object(result);
    json_push_kv_int(result, "utxos_imported", count);

    /* Report balance */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(g_node_db_bc->db,
        "SELECT COALESCE(SUM(value),0) FROM utxos",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        json_push_kv_int(result, "total_value_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    s = NULL;
    sqlite3_prepare_v2(g_node_db_bc->db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos WHERE spent_txid IS NULL",
        -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW)
        json_push_kv_int(result, "wallet_balance_zatoshi",
                          sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);

    printf("importchainstate: done — %d UTXOs imported\n", count);
    fflush(stdout);
    return true;
}

/* ── indexlegacy: import full chain from zclassicd LevelDB → our SQLite ── */

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

    int locs_cap = 4000000; /* initial, grows as needed */
    struct blk_loc *locs = calloc((size_t)locs_cap, sizeof(struct blk_loc));
    if (!locs) { json_set_str(result, "Out of memory"); return false; }

    int max_height = -1;
    int total_found = 0;

    /* ── Turbo mode: aggressive SQLite settings for bulk import ── */
    printf("indexlegacy: Entering turbo mode (synchronous=OFF, WAL)...\n");
    fflush(stdout);
    /* Stay in WAL mode — journal_mode=OFF loses data when concurrent
     * writes happen (sync_controller commits new blocks via P2P).
     * WAL mode handles concurrent readers/writers safely. */
    sqlite3_exec(g_node_db_bc->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "PRAGMA wal_autocheckpoint=10000", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL); /* 512MB */
    sqlite3_exec(g_node_db_bc->db, "PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "PRAGMA mmap_size=1073741824", NULL, NULL, NULL); /* 1GB */

    /* Drop all indexes before bulk insert — recreated after */
    printf("indexlegacy: Dropping indexes for fast bulk insert...\n");
    fflush(stdout);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_utxo_address", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_utxo_value", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_utxo_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_utxo_height_value", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_tx_block", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_tx_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_height_all", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_prev", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_chainwork", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_time", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_sprout_value", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_sapling_value", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_time_sprout", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_time_sapling", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_blocks_num_tx", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_txo_addr", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_txo_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_txi_prev", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_txi_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_ss_nf", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_ss_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_so_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_js_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_spnf_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_opret_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_opret_slp", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_zslp_xfer_token", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_zslp_xfer_height", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_zslp_xfer_addr", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "DROP INDEX IF EXISTS idx_zslp_ticker", NULL, NULL, NULL);

    /* Additive index: INSERT OR IGNORE for blocks/transactions (immutable chain
     * data that never changes). Phase B uses INSERT OR IGNORE for tx_outputs,
     * tx_inputs, joinsplits, etc. We DO NOT wipe — this makes indexlegacy
     * idempotent and crash-safe. Only addresses are recomputed from UTXOs. */
    printf("indexlegacy: Additive index mode (no wipe, INSERT OR IGNORE)...\n");
    fflush(stdout);
    node_db_exec(g_node_db_bc, "DELETE FROM addresses");
    node_db_exec(g_node_db_bc, "DELETE FROM view_integrity");

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

    /* ================================================================
     * Phase A: Sequential core chain data (blocks, txs, UTXOs, ZSLP)
     * Must be sequential because UTXO spends depend on height order.
     * ================================================================ */
    printf("indexlegacy: Phase A — core chain data (%d blocks)...\n",
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
        if (locs[h].size == 0) continue;

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

            /* Count spent inputs (but don't modify utxos table —
             * that's the canonical UTXO store managed by coins_view_sqlite) */
            if (i > 0) {
                for (size_t j = 0; j < tx->num_vin; j++) {
                    /* db_utxo_delete removed: utxos table is canonical */
                    utxos_spent++;
                }
            }

            /* Index OP_RETURN outputs for ZSLP token tracking (rare) */
            if (tx->num_vout > 0 &&
                tx->vout[0].script_pub_key.size > 0 &&
                tx->vout[0].script_pub_key.data[0] == 0x6a) {
                const uint8_t *script = tx->vout[0].script_pub_key.data;
                size_t script_len = tx->vout[0].script_pub_key.size;
                struct slp_message slp;
                bool is_slp = slp_parse(script, script_len, &slp);

                if (is_slp) {
                    uint8_t tok_id[32];
                    if (slp.type == SLP_TX_GENESIS) {
                        memcpy(tok_id, tx->hash.data, 32);
                    } else {
                        for (int b = 0; b < 32; b++)
                            tok_id[b] = slp.token_id.data[31 - b];
                    }

                    if (slp.type == SLP_TX_GENESIS)
                        db_zslp_token_save(g_node_db_bc, tok_id,
                            slp.ticker, slp.name, slp.decimals,
                            slp.document_url, h,
                            (int64_t)slp.initial_quantity);

                    int num_outputs_slp = (slp.type == SLP_TX_SEND)
                        ? slp.num_outputs : 1;
                    if (num_outputs_slp < 1) num_outputs_slp = 1;

                    for (int q = 0; q < num_outputs_slp; q++) {
                        int64_t amount = 0;
                        if (slp.type == SLP_TX_GENESIS)
                            amount = (int64_t)slp.initial_quantity;
                        else if (slp.type == SLP_TX_MINT)
                            amount = (int64_t)slp.additional_quantity;
                        else if (q < slp.num_outputs)
                            amount = (int64_t)slp.output_quantities[q];

                        uint8_t to_addr[20];
                        const uint8_t *to = NULL;
                        int out_idx = (slp.type == SLP_TX_GENESIS) ? 1 : q + 1;
                        if (out_idx < (int)tx->num_vout) {
                            const uint8_t *sd2 = tx->vout[out_idx].script_pub_key.data;
                            size_t sl2 = tx->vout[out_idx].script_pub_key.size;
                            if (sl2 == 25 && sd2[0] == 0x76 && sd2[1] == 0xa9 &&
                                sd2[2] == 0x14 && sd2[23] == 0x88 && sd2[24] == 0xac) {
                                memcpy(to_addr, sd2 + 3, 20);
                                to = to_addr;
                            }
                        }

                        db_zslp_transfer_save(g_node_db_bc, tx->hash.data,
                            h, tok_id, (int)slp.type, amount, q, to);
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

                /* db_utxo_save removed: utxos table is canonical */
                utxos_created++;
            }
        }

        block_free(&blk);

        if (blocks_indexed % 100000 == 0 && blocks_indexed > 0) {
            node_db_commit(g_node_db_bc);
            int64_t elapsed = (int64_t)time(NULL) - t_pass2;
            double rate = elapsed > 0 ?
                (double)blocks_indexed / (double)elapsed : 0;
            int remaining = total_found - blocks_indexed;
            int eta = rate > 0 ? (int)((double)remaining / rate) : 0;
            printf("  Phase A: height %d/%d — %d txs, %d utxos (+%d -%d) "
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

    int64_t phase_a_time = (int64_t)time(NULL) - t_pass2;
    printf("indexlegacy: Phase A complete — %d blocks, %d txs, %d net UTXOs "
           "in %" PRId64 "s\n",
           blocks_indexed, txs_indexed, utxos_created - utxos_spent,
           phase_a_time);
    fflush(stdout);

    /* ================================================================
     * Phase B: Parallel extraction of detailed chain data
     * Worker threads re-read blocks and extract tx_inputs, tx_outputs,
     * joinsplits, sapling_spends, sapling_outputs, sprout_nullifiers,
     * op_returns, and per-block shielded values into memory arrays.
     * Then the main thread writes everything sequentially to SQLite.
     * ================================================================ */

    printf("indexlegacy: Phase B — parallel extraction with %d threads...\n",
           N_INDEX_THREADS);
    fflush(stdout);
    int64_t t_phase_b = (int64_t)time(NULL);

    /* Divide height range among threads */
    struct worker_ctx workers[N_INDEX_THREADS];
    pthread_t threads[N_INDEX_THREADS];
    int heights_per_thread = (max_height + 1 + N_INDEX_THREADS - 1) / N_INDEX_THREADS;

    for (int t = 0; t < N_INDEX_THREADS; t++) {
        memset(&workers[t], 0, sizeof(workers[t]));
        workers[t].thread_id = t;
        workers[t].height_from = t * heights_per_thread;
        workers[t].height_to = (t + 1) * heights_per_thread - 1;
        if (workers[t].height_to > max_height)
            workers[t].height_to = max_height;
        workers[t].locs = locs;
        workers[t].max_height = max_height;
        workers[t].legacy_dir = legacy_dir;
        pthread_create(&threads[t], NULL, index_worker, &workers[t]);
    }

    for (int t = 0; t < N_INDEX_THREADS; t++)
        pthread_join(threads[t], NULL);

    int64_t extract_time = (int64_t)time(NULL) - t_phase_b;
    printf("indexlegacy: Phase B extraction complete in %" PRId64 "s\n",
           extract_time);
    fflush(stdout);

    /* ── Phase B write: sequential INSERT into SQLite ── */
    printf("indexlegacy: Phase B — writing extracted data to SQLite...\n");
    fflush(stdout);
    int64_t t_write = (int64_t)time(NULL);

    int64_t joinsplits_indexed = 0, sapling_spends_indexed = 0;
    int64_t sapling_outputs_indexed = 0, sprout_nullifiers_indexed = 0;
    int64_t op_returns_indexed = 0;
    int64_t total_inputs = 0, total_outputs = 0;
    int64_t batch_rows = 0;
    struct idx_block_shielded *all_bsh = NULL;
    

    /* Phase B MUST use a separate sqlite3 connection. The main handle
     * has sync_controller's batch transaction which gets rolled back
     * on any block validation error — destroying all Phase B inserts.
     * WAL mode allows concurrent readers but only ONE writer. We set
     * a 60-second busy timeout so Phase B waits for the write lock. */
    const char *phase_b_path = sqlite3_db_filename(g_node_db_bc->db, "main");
    sqlite3 *phase_b_db = NULL;
    if (sqlite3_open(phase_b_path, &phase_b_db) != SQLITE_OK || !phase_b_db) {
        printf("indexlegacy: Phase B FATAL: cannot open DB: %s\n",
               phase_b_db ? sqlite3_errmsg(phase_b_db) : "null");
        fflush(stdout);
        /* Fall through with empty tables rather than crash */
        phase_b_db = g_node_db_bc->db; /* fallback */
    }
    sqlite3_exec(phase_b_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(phase_b_db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_busy_timeout(phase_b_db, 60000); /* 60s wait for write lock */
    bool phase_b_own_txn = true;
    sqlite3_exec(phase_b_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    sqlite3_stmt *stmt_txo = NULL, *stmt_txi = NULL, *stmt_js = NULL;
    sqlite3_stmt *stmt_ss = NULL, *stmt_so = NULL, *stmt_spnf = NULL;
    sqlite3_stmt *stmt_opret = NULL, *stmt_integrity = NULL;
    sqlite3_stmt *stmt_update_shielded = NULL;

    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO tx_outputs"
        "(txid,vout,value,script_type,address_hash,block_height)"
        " VALUES(?,?,?,?,?,?)",
        -1, &stmt_txo, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO tx_inputs"
        "(txid,vin_index,prev_txid,prev_vout,block_height)"
        " VALUES(?,?,?,?,?)",
        -1, &stmt_txi, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO joinsplits"
        "(txid,js_index,vpub_old,vpub_new,anchor,block_height)"
        " VALUES(?,?,?,?,?,?)",
        -1, &stmt_js, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO sapling_spends"
        "(txid,spend_index,cv,anchor,nullifier,rk,block_height)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &stmt_ss, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO sapling_outputs"
        "(txid,output_index,cv,cm,ephemeral_key,block_height)"
        " VALUES(?,?,?,?,?,?)",
        -1, &stmt_so, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO sprout_nullifiers"
        "(nullifier,txid,block_height)"
        " VALUES(?,?,?)",
        -1, &stmt_spnf, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR IGNORE INTO op_returns"
        "(txid,block_height,script,is_slp)"
        " VALUES(?,?,?,?)",
        -1, &stmt_opret, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "INSERT OR REPLACE INTO view_integrity"
        "(height,sha3_hash) VALUES(?,?)",
        -1, &stmt_integrity, NULL);
    sqlite3_prepare_v2(phase_b_db,
        "UPDATE blocks SET sprout_value=?,sapling_value=?"
        " WHERE height=?",
        -1, &stmt_update_shielded, NULL);

    /* Phase B BEGIN on separate connection */

    /* Write tx_inputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_inputs; k++) {
            struct idx_tx_input *inp = &workers[t].inputs[k];
            sqlite3_reset(stmt_txi);
            sqlite3_bind_blob(stmt_txi, 1, inp->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_txi, 2, (int)inp->vin_index);
            sqlite3_bind_blob(stmt_txi, 3, inp->prev_txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_txi, 4, (int)inp->prev_vout);
            sqlite3_bind_int(stmt_txi, 5, inp->height);
            {
                int rc = sqlite3_step(stmt_txi);
                if (rc != SQLITE_DONE && rc != SQLITE_ROW && total_inputs == 0)
                    printf("Phase B: first INSERT rc=%d (%s) autocommit=%d\n",
                           rc, sqlite3_errmsg(phase_b_db),
                           sqlite3_get_autocommit(phase_b_db));
            }
            total_inputs++;
            if (++batch_rows % 500000 == 0) {
                if (phase_b_own_txn) {
                    sqlite3_exec(phase_b_db, "COMMIT", NULL, NULL, NULL);
                    sqlite3_exec(phase_b_db, "BEGIN", NULL, NULL, NULL);
                }
                printf("  Phase B write: %lld rows...\n", (long long)batch_rows);
                fflush(stdout);
            }
        }
    }

    /* Write tx_outputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_outputs; k++) {
            struct idx_tx_output *ot = &workers[t].outputs[k];
            sqlite3_reset(stmt_txo);
            sqlite3_bind_blob(stmt_txo, 1, ot->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_txo, 2, (int)ot->vout);
            sqlite3_bind_int64(stmt_txo, 3, ot->value);
            sqlite3_bind_int(stmt_txo, 4, ot->script_type);
            if (ot->has_addr)
                sqlite3_bind_blob(stmt_txo, 5, ot->addr_hash, 20, SQLITE_STATIC);
            else
                sqlite3_bind_null(stmt_txo, 5);
            sqlite3_bind_int(stmt_txo, 6, ot->height);
            sqlite3_step(stmt_txo);
            total_outputs++;
        }
    }

    /* Write joinsplits + sprout nullifiers from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_joinsplits; k++) {
            struct idx_joinsplit *ij = &workers[t].joinsplits[k];
            sqlite3_reset(stmt_js);
            sqlite3_bind_blob(stmt_js, 1, ij->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_js, 2, (int)ij->js_index);
            sqlite3_bind_int64(stmt_js, 3, ij->vpub_old);
            sqlite3_bind_int64(stmt_js, 4, ij->vpub_new);
            sqlite3_bind_blob(stmt_js, 5, ij->anchor, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_js, 6, ij->height);
            sqlite3_step(stmt_js);
            joinsplits_indexed++;

            /* Write both sprout nullifiers */
            for (int nf = 0; nf < 2; nf++) {
                sqlite3_reset(stmt_spnf);
                sqlite3_bind_blob(stmt_spnf, 1, ij->nullifiers[nf], 32, SQLITE_STATIC);
                sqlite3_bind_blob(stmt_spnf, 2, ij->txid, 32, SQLITE_STATIC);
                sqlite3_bind_int(stmt_spnf, 3, ij->height);
                sqlite3_step(stmt_spnf);
                sprout_nullifiers_indexed++;
            }
        }
    }

    /* Write sapling spends from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_sspends; k++) {
            struct idx_sapling_spend *is2 = &workers[t].sspends[k];
            sqlite3_reset(stmt_ss);
            sqlite3_bind_blob(stmt_ss, 1, is2->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_ss, 2, (int)is2->spend_index);
            sqlite3_bind_blob(stmt_ss, 3, is2->cv, 32, SQLITE_STATIC);
            sqlite3_bind_blob(stmt_ss, 4, is2->anchor, 32, SQLITE_STATIC);
            sqlite3_bind_blob(stmt_ss, 5, is2->nullifier, 32, SQLITE_STATIC);
            sqlite3_bind_blob(stmt_ss, 6, is2->rk, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_ss, 7, is2->height);
            sqlite3_step(stmt_ss);
            sapling_spends_indexed++;
        }
    }

    /* Write sapling outputs from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_soutputs; k++) {
            struct idx_sapling_output *io = &workers[t].soutputs[k];
            sqlite3_reset(stmt_so);
            sqlite3_bind_blob(stmt_so, 1, io->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_so, 2, (int)io->output_index);
            sqlite3_bind_blob(stmt_so, 3, io->cv, 32, SQLITE_STATIC);
            sqlite3_bind_blob(stmt_so, 4, io->cm, 32, SQLITE_STATIC);
            sqlite3_bind_blob(stmt_so, 5, io->ephemeral_key, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_so, 6, io->height);
            sqlite3_step(stmt_so);
            sapling_outputs_indexed++;
        }
    }

    /* Write op_returns from all threads */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        for (int k = 0; k < workers[t].num_oprets; k++) {
            struct idx_opret *op = &workers[t].oprets[k];
            sqlite3_reset(stmt_opret);
            sqlite3_bind_blob(stmt_opret, 1, op->txid, 32, SQLITE_STATIC);
            sqlite3_bind_int(stmt_opret, 2, op->height);
            sqlite3_bind_blob(stmt_opret, 3, op->script, (int)op->script_len, SQLITE_STATIC);
            sqlite3_bind_int(stmt_opret, 4, op->is_slp);
            sqlite3_step(stmt_opret);
            op_returns_indexed++;
        }
    }

    /* Collect all block_shielded entries, sort by height for SHA3 chain */
    int total_bsh = 0;
    for (int t = 0; t < N_INDEX_THREADS; t++)
        total_bsh += workers[t].num_blocks_sh;

    all_bsh = malloc((size_t)total_bsh * sizeof(*all_bsh));
    int bsh_idx = 0;
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        if (workers[t].num_blocks_sh > 0) {
            memcpy(all_bsh + bsh_idx, workers[t].blocks_sh,
                   (size_t)workers[t].num_blocks_sh * sizeof(*all_bsh));
            bsh_idx += workers[t].num_blocks_sh;
        }
    }

    /* Sort by height (threads are already sorted within, but merge all) */
    /* Simple: threads process contiguous ranges so concatenation is sorted.
     * But verify by sorting anyway for safety. */
    for (int a = 1; a < total_bsh; a++) {
        if (all_bsh[a].height < all_bsh[a - 1].height) {
            /* Need to sort — use insertion sort on nearly-sorted data */
            for (int b = a; b > 0 && all_bsh[b].height < all_bsh[b-1].height; b--) {
                struct idx_block_shielded tmp_bsh = all_bsh[b];
                all_bsh[b] = all_bsh[b-1];
                all_bsh[b-1] = tmp_bsh;
            }
        }
    }

    /* Update sprout_value/sapling_value + compute SHA3 hash chain */
    uint8_t sha3_prev[32];
    memset(sha3_prev, 0, 32);

    for (int k = 0; k < total_bsh; k++) {
        struct idx_block_shielded *b = &all_bsh[k];

        if (b->sprout_value != 0 || b->sapling_value != 0) {
            sqlite3_reset(stmt_update_shielded);
            sqlite3_bind_int64(stmt_update_shielded, 1, b->sprout_value);
            sqlite3_bind_int64(stmt_update_shielded, 2, b->sapling_value);
            sqlite3_bind_int(stmt_update_shielded, 3, b->height);
            sqlite3_step(stmt_update_shielded);
        }

        /* SHA3-256 integrity hash chain */
        struct sha3_256_ctx sha3;
        sha3_256_init(&sha3);
        sha3_256_write(&sha3, sha3_prev, 32);

        uint32_t h_le = (uint32_t)b->height;
        sha3_256_write(&sha3, (const unsigned char *)&h_le, 4);
        sha3_256_write(&sha3, b->block_hash, 32);

        int64_t sv_le = b->sprout_value;
        sha3_256_write(&sha3, (const unsigned char *)&sv_le, 8);
        int64_t sapv_le = b->sapling_value;
        sha3_256_write(&sha3, (const unsigned char *)&sapv_le, 8);

        sha3_256_write(&sha3, (const unsigned char *)&b->num_tx, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_js, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_ss, 4);
        sha3_256_write(&sha3, (const unsigned char *)&b->num_so, 4);

        uint8_t sha3_out[32];
        sha3_256_finalize(&sha3, sha3_out);
        memcpy(sha3_prev, sha3_out, 32);

        sqlite3_reset(stmt_integrity);
        sqlite3_bind_int(stmt_integrity, 1, b->height);
        sqlite3_bind_blob(stmt_integrity, 2, sha3_out, 32, SQLITE_STATIC);
        sqlite3_step(stmt_integrity);
    }

    sqlite3_exec(phase_b_db, "COMMIT", NULL, NULL, NULL);
    if (phase_b_db != g_node_db_bc->db)
        sqlite3_close(phase_b_db);
    phase_b_db = NULL;

    /* Free all thread buffers */
    for (int t = 0; t < N_INDEX_THREADS; t++) {
        free(workers[t].inputs);
        free(workers[t].outputs);
        free(workers[t].joinsplits);
        free(workers[t].sspends);
        free(workers[t].soutputs);
        free(workers[t].oprets);
        free(workers[t].blocks_sh);
    }
    free(all_bsh);

    sqlite3_finalize(stmt_txo);
    sqlite3_finalize(stmt_txi);
    sqlite3_finalize(stmt_js);
    sqlite3_finalize(stmt_ss);
    sqlite3_finalize(stmt_so);
    sqlite3_finalize(stmt_spnf);
    sqlite3_finalize(stmt_opret);
    sqlite3_finalize(stmt_integrity);
    sqlite3_finalize(stmt_update_shielded);

    int64_t write_time = (int64_t)time(NULL) - t_write;
    printf("indexlegacy: Phase B wrote %" PRId64 " inputs, %" PRId64 " outputs, "
           "%" PRId64 " joinsplits, %" PRId64 " sspends, %" PRId64 " soutputs, "
           "%" PRId64 " spnf, %" PRId64 " oprets in %" PRId64 "s\n",
           total_inputs, total_outputs, joinsplits_indexed,
           sapling_spends_indexed, sapling_outputs_indexed,
           sprout_nullifiers_indexed, op_returns_indexed, write_time);
    fflush(stdout);

    /* ── Rebuild all indexes (dropped before bulk insert for speed) ── */
    printf("indexlegacy: Rebuilding indexes...\n");
    fflush(stdout);
    int64_t t_idx = (int64_t)time(NULL);
    /* Core block/tx/utxo indexes */
    sqlite3_exec(g_node_db_bc->db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_blocks_height ON blocks(height) WHERE status >= 3", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_prev ON blocks(prev_hash)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_chainwork ON blocks(chain_work DESC)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_height_all ON blocks(height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time ON blocks(time)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_sprout_value ON blocks(sprout_value) WHERE sprout_value != 0", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_sapling_value ON blocks(sapling_value) WHERE sapling_value != 0", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time_sprout ON blocks(time, sprout_value) WHERE sprout_value != 0", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_time_sapling ON blocks(time, sapling_value) WHERE sapling_value != 0", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_blocks_num_tx ON blocks(num_tx DESC)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_tx_block ON transactions(block_hash)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_tx_height ON transactions(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_utxo_address ON utxos(address_hash) WHERE address_hash IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_utxo_value ON utxos(value DESC)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_utxo_height ON utxos(height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_utxo_height_value ON utxos(height, value)", NULL, NULL, NULL);
    /* New chain index table indexes */
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_txo_addr ON tx_outputs(address_hash) WHERE address_hash IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_txo_height ON tx_outputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_txi_prev ON tx_inputs(prev_txid, prev_vout)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_txi_height ON tx_inputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_ss_nf ON sapling_spends(nullifier)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_ss_height ON sapling_spends(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_so_height ON sapling_outputs(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_js_height ON joinsplits(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_spnf_height ON sprout_nullifiers(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_opret_height ON op_returns(block_height)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_opret_slp ON op_returns(is_slp) WHERE is_slp = 1", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_token ON zslp_transfers(token_id)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_height ON zslp_transfers(block_height DESC)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_zslp_xfer_addr ON zslp_transfers(to_addr) WHERE to_addr IS NOT NULL", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_zslp_ticker ON zslp_tokens(ticker)", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "CREATE INDEX IF NOT EXISTS idx_addr_balance ON addresses(balance DESC)", NULL, NULL, NULL);
    printf("indexlegacy: Indexes rebuilt in %llds\n",
        (long long)((int64_t)time(NULL) - t_idx));
    fflush(stdout);

    /* Restore safe SQLite settings */
    sqlite3_exec(g_node_db_bc->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(g_node_db_bc->db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    sqlite3_wal_checkpoint_v2(g_node_db_bc->db, NULL,
        SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);

    /* ── Populate addresses from UTXO set ── */
    printf("indexlegacy: Populating addresses from UTXO set...\n");
    fflush(stdout);
    node_db_exec(g_node_db_bc, "DELETE FROM addresses");
    sqlite3_exec(g_node_db_bc->db,
        "INSERT OR REPLACE INTO addresses "
        "(address_hash, script_type, balance, utxo_count, "
        "first_seen_height, last_seen_height) "
        "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
        "MIN(height), MAX(height) "
        "FROM utxos WHERE address_hash IS NOT NULL "
        "GROUP BY address_hash",
        NULL, NULL, NULL);
    int64_t addr_count = 0;
    {
        sqlite3_stmt *chk = NULL;
        if (sqlite3_prepare_v2(g_node_db_bc->db,
                "SELECT count(*) FROM addresses", -1, &chk, NULL) == SQLITE_OK && chk) {
            if (sqlite3_step(chk) == SQLITE_ROW)
                addr_count = sqlite3_column_int64(chk, 0);
            sqlite3_finalize(chk);
        }
    }
    printf("indexlegacy: Populated %" PRId64 " addresses\n", addr_count);
    fflush(stdout);

    /* Update tip */
    {
        struct db_block tip_blk;
        if (db_block_find_by_height(g_node_db_bc, max_height, &tip_blk))
            node_db_sync_set_tip(g_node_db_bc, tip_blk.hash, max_height);
    }

    int64_t total_elapsed = (int64_t)time(NULL) - t_start;
    int net_utxos = utxos_created - utxos_spent;
    printf("indexlegacy: COMPLETE — %d blocks, %d txs, %d net UTXOs "
           "(+%d -%d), %" PRId64 " js, %" PRId64 " sspend, %" PRId64 " sout, "
           "%" PRId64 " spnf, %" PRId64 " opret, %" PRId64 " addrs "
           "in %" PRId64 "m%" PRId64 "s\n",
           blocks_indexed, txs_indexed, net_utxos,
           utxos_created, utxos_spent,
           joinsplits_indexed, sapling_spends_indexed,
           sapling_outputs_indexed, sprout_nullifiers_indexed,
           op_returns_indexed, addr_count,
           total_elapsed / 60, total_elapsed % 60);
    fflush(stdout);

    free(locs);

    json_set_object(result);
    json_push_kv_int(result, "blocks_indexed", blocks_indexed);
    json_push_kv_int(result, "transactions_indexed", txs_indexed);
    json_push_kv_int(result, "utxos_net", net_utxos);
    json_push_kv_int(result, "utxos_created", utxos_created);
    json_push_kv_int(result, "utxos_spent", utxos_spent);
    json_push_kv_int(result, "joinsplits_indexed", (int)joinsplits_indexed);
    json_push_kv_int(result, "sapling_spends_indexed", (int)sapling_spends_indexed);
    json_push_kv_int(result, "sapling_outputs_indexed", (int)sapling_outputs_indexed);
    json_push_kv_int(result, "sprout_nullifiers_indexed", (int)sprout_nullifiers_indexed);
    json_push_kv_int(result, "op_returns_indexed", (int)op_returns_indexed);
    json_push_kv_int(result, "addresses_populated", (int)addr_count);
    json_push_kv_int(result, "max_height", max_height);
    json_push_kv_int(result, "elapsed_seconds", total_elapsed);
    return true;
}

/* ── Global MMR ────────────────────────────────────────── */

static struct mmr g_mmr;
static bool g_mmr_initialized = false;

void rpc_blockchain_mmr_append(const uint8_t block_hash[32])
{
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
    mmr_append(&g_mmr, block_hash);
}

struct mmr *rpc_blockchain_get_mmr(void) { return &g_mmr; }

void rpc_blockchain_mmr_init_from_state(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT value FROM node_state WHERE key='mmr_state'",
            -1, &s, NULL) != SQLITE_OK)
        return;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len >= 12 && mmr_deserialize(&g_mmr, blob, (size_t)len))
            g_mmr_initialized = true;
    }
    sqlite3_finalize(s);
    if (!g_mmr_initialized) {
        mmr_init(&g_mmr);
        g_mmr_initialized = true;
    }
}

void rpc_blockchain_mmr_catchup(struct main_state *ms)
{
    if (!g_mmr_initialized || !ms) return;
    int chain_height = active_chain_height(&ms->chain_active);
    int mmr_height = (int)g_mmr.num_leaves - 1;

    if (mmr_height >= chain_height) return;

    int start = mmr_height + 1;
    int64_t t0 = (int64_t)time(NULL);
    for (int h = start; h <= chain_height; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (bi && bi->phashBlock)
            mmr_append(&g_mmr, bi->phashBlock->data);
    }
    int64_t elapsed = (int64_t)time(NULL) - t0;
    printf("MMR catchup: %d → %d (%d blocks, %llds)\n",
           start, chain_height, chain_height - start + 1, (long long)elapsed);
}

void rpc_blockchain_mmr_save(struct node_db *ndb)
{
    if (!ndb || !ndb->open || !g_mmr_initialized) return;
    uint8_t buf[MMR_SERIALIZED_MAX];
    size_t len = mmr_serialize(&g_mmr, buf, sizeof(buf));
    if (len == 0) return;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('mmr_state',?)", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_blob(s, 1, buf, (int)len, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ── SHA3 UTXO commitment RPC ──────────────────────────── */

static bool rpc_getutxocommitment(const struct json_value *params, bool help,
                                    struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getutxocommitment\n"
        "\nComputes SHA3-256 hash over the entire UTXO set in canonical order.\n"
        "This is a deterministic commitment that two nodes can compare.\n");

    if (!g_node_db_bc || !g_node_db_bc->open) {
        json_set_str(result, "Database not available");
        return false;
    }
    if (!g_main_state) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush coins cache first */
    if (g_coins_tip)
        coins_view_cache_flush(g_coins_tip);

    uint8_t sha3_hash[32];
    uint64_t count = 0;
    int64_t t0 = (int64_t)time(NULL);
    utxo_commitment_sha3_compute(g_node_db_bc->db, sha3_hash, &count);
    int64_t elapsed = (int64_t)time(NULL) - t0;

    int tip = active_chain_height(&g_main_state->chain_active);

    /* Save checkpoint */
    utxo_commitment_sha3_save(g_node_db_bc->db, sha3_hash, tip, count);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", sha3_hash[i]);

    json_set_object(result);
    json_push_kv_str(result, "sha3_hash", hex);
    json_push_kv_int(result, "height", tip);
    json_push_kv_int(result, "utxo_count", (int64_t)count);
    json_push_kv_int(result, "elapsed_seconds", elapsed);
    return true;
}

/* ── MMR root RPC ──────────────────────────────────────── */

static bool rpc_getmmrroot(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getmmrroot\n"
        "\nReturns the Merkle Mountain Range root over all block hashes.\n"
        "Uses SHA3-256 with domain separation for power node sync.\n");

    if (!g_mmr_initialized) {
        json_set_str(result, "MMR not initialized");
        return false;
    }

    uint8_t root[32];
    mmr_root(&g_mmr, root);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", root[i]);

    json_set_object(result);
    json_push_kv_str(result, "mmr_root", hex);
    json_push_kv_int(result, "num_leaves", (int64_t)g_mmr.num_leaves);
    json_push_kv_int(result, "num_peaks", g_mmr.num_peaks);
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
        { "blockchain", "importchainstate",     rpc_importchainstate,       false },
        { "blockchain", "indexlegacy",          rpc_indexlegacy,            false },
        { "blockchain", "getutxocommitment",   rpc_getutxocommitment,     true },
        { "blockchain", "getmmrroot",          rpc_getmmrroot,            true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
