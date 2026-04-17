/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * HODL Wave analytics: UTXO age distribution, heatmaps, charts.
 * Extracted from blockchain_controller.c for file size. */

#include "controllers/hodl_controller.h"
#include "controllers/strong_params.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "chain/subsidy.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "storage/coins_db.h"
#include "storage/dbwrapper.h"
#include "core/uint256.h"
#include "json/json.h"
#include "util/png_writer.h"
#include "models/database.h"
#include "views/format_helpers.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <sqlite3.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

struct hodl_context {
    struct main_state *main_state;
    struct coins_view_cache *coins_tip;
    struct node_db *node_db;
    const char *datadir;
};

static struct hodl_context g_hodl_ctx = {0};

static struct hodl_context *hodl_ctx(void)
{
    return &g_hodl_ctx;
}

static struct coins_view_db *hodl_coins_db(struct hodl_context *ctx)
{
    if (!ctx || !ctx->coins_tip)
        LOG_NULL("hodl", "hodl_coins_db: ctx or coins_tip is NULL");
    if (ctx->coins_tip->base.impl == NULL)
        LOG_NULL("hodl", "hodl_coins_db: coins_view base impl is NULL");
    return (struct coins_view_db *)ctx->coins_tip->base.impl;
}

void rpc_hodl_set_state(struct main_state *ms,
                         struct coins_view_cache *coins_tip,
                         struct node_db *ndb, const char *datadir)
{
    struct hodl_context *ctx = hodl_ctx();
    ctx->main_state = ms;
    ctx->coins_tip = coins_tip;
    ctx->node_db = ndb;
    ctx->datadir = datadir;
}

/* HODL Wave: UTXO age distribution across the entire UTXO set.
 * Uses coins_view_db_get_coins for correct deserialization. */
static bool rpc_gethodlwave(const struct json_value *params, bool help,
                              struct json_value *result)
{
    struct hodl_context *ctx = hodl_ctx();
    (void)params;
    RPC_HELP(help, result,
        "gethodlwave\n"
        "\nScans the entire UTXO set and reports value distribution by age.\n"
        "Inspired by Unchained Capital's Bitcoin HODL Waves analysis.\n"
        "\nBuckets: <1d, 1d-1w, 1w-1m, 1-3m, 3-6m, 6-12m, 1-2y, 2-3y, 3-5y, >5y\n"
        "\nResult: { buckets: [{label, value, pct, utxo_count}], summary }\n");

    if (!ctx->node_db || !ctx->node_db->open) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!ctx->main_state || !active_chain_tip(&ctx->main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Flush in-memory UTXO cache to SQLite for accurate totals */
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int tip_height = active_chain_height(&ctx->main_state->chain_active);

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
    sqlite3_prepare_v2(ctx->node_db->db,
        "SELECT height, value FROM utxos",
        -1, &hodl_s, NULL);

    while (hodl_s && AR_STEP_ROW_READONLY(hodl_s) == SQLITE_ROW) {
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
    struct hodl_context *ctx = hodl_ctx();
    struct coins_view_db *coins_db = hodl_coins_db(ctx);
    (void)params;
    RPC_HELP(help, result,
        "gethodlwaveimage\n"
        "\nGenerates a HODL wave heatmap image from the current UTXO set.\n"
        "\nThe image shows:\n"
        "  Top: UTXO creation heatmap (X=block height, Y=value log scale)\n"
        "  Bottom: HODL wave age distribution bar\n"
        "\nSaves to <datadir>/hodlwave.ppm\n"
        "\nResult: { file, width, height, total_utxos, total_value }\n");

    if (!coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!ctx->main_state || !active_chain_tip(&ctx->main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int tip_height = active_chain_height(&ctx->main_state->chain_active);
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
    int64_t *grid = zcl_calloc((size_t)(HEATMAP_H * IMG_W), sizeof(int64_t), "heatmap_grid");
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
    db_iter_init(&it, &coins_db->db);
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
        if (coins_view_db_get_coins(coins_db, &txid, &c)) {
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
    uint8_t *pixels = zcl_calloc((size_t)(IMG_W * IMG_H * 3), 1, "heatmap_pixels");
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
             ctx->datadir ? ctx->datadir : ".");

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
    struct hodl_context *ctx = hodl_ctx();
    struct coins_view_db *coins_db = hodl_coins_db(ctx);
    RPC_HELP(help, result,
        "gethodlwavetimeline ( \"granularity\" )\n"
        "\nTime-series of surviving UTXO value by creation date.\n"
        "Shows how much ZCL last moved on each day/month is still unspent.\n"
        "Generates a PPM chart at <datadir>/hodlwave_timeline.ppm\n"
        "\nArguments:\n"
        "  1. granularity   (string, optional, default=\"month\") \"day\" or \"month\"\n"
        "\nResult: { periods: [{date, zcl, utxos, cumulative_pct}], chart_file }\n");

    if (!coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!ctx->main_state || !active_chain_tip(&ctx->main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }

    /* Parse granularity */
    bool by_month = true;
    if (params && params->type == JSON_ARR && params->num_children > 0) {
        if (params->children[0].type != JSON_STR) {
            json_set_str(result, "granularity must be \"day\" or \"month\"");
            return false;
        }
        if (strcmp(params->children[0].val.s, "day") == 0) {
            by_month = false;
        } else if (strcmp(params->children[0].val.s, "month") != 0) {
            json_set_str(result, "granularity must be \"day\" or \"month\"");
            return false;
        }
    }

    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int tip_height = active_chain_height(&ctx->main_state->chain_active);
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
    int64_t *period_value = zcl_calloc((size_t)num_periods, sizeof(int64_t), "period_value");
    int64_t *period_count = zcl_calloc((size_t)num_periods, sizeof(int64_t), "period_count");
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
    db_iter_init(&it, &coins_db->db);
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
        if (!coins_view_db_get_coins(coins_db, &txid, &c)) {
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
    uint8_t *img = zcl_calloc((size_t)(IMG_W * IMG_H * 3), 1, "chart_pixels");
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
             ctx->datadir ? ctx->datadir : ".");
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
    struct hodl_context *ctx = hodl_ctx();
    struct coins_view_db *coins_db = hodl_coins_db(ctx);
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

    if (!coins_db) {
        json_set_str(result, "Coins database not available");
        return false;
    }
    if (!ctx->main_state || !active_chain_tip(&ctx->main_state->chain_active)) {
        json_set_str(result, "Chain not loaded");
        return false;
    }
    if (ctx->coins_tip)
        coins_view_cache_flush(ctx->coins_tip);

    int tip_height = active_chain_height(&ctx->main_state->chain_active);

    /* ── Step 1: Scan UTXO set into arrays ──────────────────── */
    size_t cap = 2000000;
    int64_t *utxo_ts = zcl_malloc(cap * sizeof(int64_t), "hodl_utxo_ts");
    int64_t *utxo_val = zcl_malloc(cap * sizeof(int64_t), "hodl_utxo_val");
    if (!utxo_ts || !utxo_val) {
        free(utxo_ts); free(utxo_val);
        json_set_str(result, "Out of memory");
        return false;
    }

    size_t num_utxos = 0;
    struct db_iterator it;
    db_iter_init(&it, &coins_db->db);
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
        if (!coins_view_db_get_coins(coins_db, &txid, &c)) {
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
                    int64_t *new_ts = zcl_realloc(utxo_ts, new_cap * sizeof(int64_t), "hodl_utxo_ts");
                    if (!new_ts) {
                        free(utxo_ts); free(utxo_val);
                        coins_free(&c); db_iter_free(&it);
                        json_set_str(result, "Out of memory");
                        return false;
                    }
                    utxo_ts = new_ts;
                    int64_t *new_val = zcl_realloc(utxo_val, new_cap * sizeof(int64_t), "hodl_utxo_val");
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
    int64_t *grid = zcl_calloc((size_t)(num_months * NBUCKETS), sizeof(int64_t), "hodl_wave_grid");
    if (!grid) {
        free(utxo_ts); free(utxo_val);
        json_set_str(result, "Out of memory");
        return false;
    }

    /* For each month, compute the timestamp at the 1st of that month */
    int64_t *month_ts = zcl_malloc((size_t)num_months * sizeof(int64_t), "hodl_month_ts");
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

    uint8_t *img = zcl_calloc((size_t)(IMG_W * IMG_H * 3), 1, "hodl_wave_img");
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
             ctx->datadir ? ctx->datadir : ".");
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

void register_hodl_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "gethodlwave",         rpc_gethodlwave,         true },
        { "blockchain", "gethodlwaveimage",    rpc_gethodlwaveimage,    true },
        { "blockchain", "gethodlwavetimeline", rpc_gethodlwavetimeline, true },
        { "blockchain", "gethodlwavechart",    rpc_gethodlwavechart,    true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
