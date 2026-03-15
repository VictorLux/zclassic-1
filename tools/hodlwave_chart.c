/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HODL wave stacked-area chart generator. Pure C23.
 * Reads UTXOs from SQLite, renders 1920x1080 PNG with labels.
 *
 * Build:  cc -std=c23 -O2 -Ilib/util/include -o hodlwave_chart \
 *         tools/hodlwave_chart.c lib/util/src/png_writer.c \
 *         lib/util/src/bitmap_font.c -lsqlite3 -lm
 * Usage:  ./hodlwave_chart [node.db] [output.png] [start_year] */

#include "util/png_writer.h"
#include "util/bitmap_font.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define GENESIS_TIME  1478403829LL
#define BUTTERCUP_HT  707000
#define PRE_SPACING   150
#define POST_SPACING  75
#define NBUCKETS      10

static int64_t height_to_ts(int h)
{
    if (h <= 0) return GENESIS_TIME;
    if (h < BUTTERCUP_HT) return GENESIS_TIME + (int64_t)h * PRE_SPACING;
    return GENESIS_TIME + (int64_t)BUTTERCUP_HT * PRE_SPACING
                        + (int64_t)(h - BUTTERCUP_HT) * POST_SPACING;
}

static const int64_t age_thresh[NBUCKETS] = {
    86400, 604800, 2592000, 2592000LL*3, 2592000LL*6,
    31557600LL, 31557600LL*2, 31557600LL*3, 31557600LL*5, 0
};

static const char *bucket_labels[NBUCKETS] = {
    "< 1 day", "1d - 1w", "1w - 1m", "1 - 3m", "3 - 6m",
    "6 - 12m", "1 - 2y",  "2 - 3y",  "3 - 5y", "> 5 years"
};

static const uint8_t colors[NBUCKETS][3] = {
    {255, 60,  40},   /* < 1 day — red */
    {255, 140, 30},   /* 1d-1w   — orange */
    {255, 215, 45},   /* 1w-1m   — gold */
    {195, 240, 55},   /* 1-3m    — lime */
    {80,  220, 75},   /* 3-6m    — green */
    {35,  200, 160},  /* 6-12m   — teal */
    {45,  155, 230},  /* 1-2y    — sky blue */
    {75,  95,  215},  /* 2-3y    — indigo */
    {130, 65,  195},  /* 3-5y    — purple */
    {95,  40,  140},  /* > 5y    — deep purple */
};

static void fill_rect(uint8_t *img, int W, int H,
                      int x0, int y0, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++)
            if (x >= 0 && y >= 0) {
                int off = (y * W + x) * 3;
                img[off] = r; img[off+1] = g; img[off+2] = b;
            }
}

/* Linear interpolation between two data columns for smooth rendering */
static double lerp_bucket_frac(const int64_t *grid, int num_cols,
                                int bucket, double col_f)
{
    int c0 = (int)col_f;
    int c1 = c0 + 1;
    if (c0 < 0) c0 = 0;
    if (c1 >= num_cols) c1 = num_cols - 1;
    if (c0 >= num_cols) c0 = num_cols - 1;
    double t = col_f - (double)c0;
    if (t < 0) t = 0; if (t > 1) t = 1;

    int64_t tot0 = 0, tot1 = 0;
    for (int b = 0; b < NBUCKETS; b++) {
        tot0 += grid[c0 * NBUCKETS + b];
        tot1 += grid[c1 * NBUCKETS + b];
    }
    if (tot0 == 0 && tot1 == 0) return 0;

    double f0 = tot0 > 0 ? (double)grid[c0 * NBUCKETS + bucket] / (double)tot0 : 0;
    double f1 = tot1 > 0 ? (double)grid[c1 * NBUCKETS + bucket] / (double)tot1 : 0;
    return f0 + (f1 - f0) * t;
}

int main(int argc, char **argv)
{
    const char *db_path = argc > 1 ? argv[1]
        : "/home/bob/.zclassic-c23/node.db";
    const char *out_path = argc > 2 ? argv[2]
        : "/home/bob/.zclassic-c23/hodlwave_chart.png";

    sqlite3 *db;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL)) {
        fprintf(stderr, "Cannot open %s\n", db_path);
        return 1;
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT height, value FROM utxos "
        "WHERE height >= 0 AND height <= 3100000 AND value > 0",
        -1, &stmt, NULL);

    size_t cap = 2000000, n = 0;
    int64_t *uts = malloc(cap * sizeof(int64_t));
    int64_t *uval = malloc(cap * sizeof(int64_t));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            uts = realloc(uts, cap * sizeof(int64_t));
            uval = realloc(uval, cap * sizeof(int64_t));
        }
        uts[n] = height_to_ts(sqlite3_column_int(stmt, 0));
        uval[n] = sqlite3_column_int64(stmt, 1);
        n++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    printf("Loaded %zu UTXOs\n", n);

    int64_t tip_ts = height_to_ts(3043007);

    int start_year = argc > 3 ? atoi(argv[3]) : 0;
    int64_t view_start = GENESIS_TIME;
    if (start_year >= 2017 && start_year <= 2025) {
        struct tm sy = {0};
        sy.tm_year = start_year - 1900;
        sy.tm_mon = 0; sy.tm_mday = 1;
        view_start = (int64_t)timegm(&sy);
    }

    /* Weekly columns */
    int num_cols = (int)((tip_ts - view_start) / (86400 * 7)) + 1;
    if (num_cols > 2000) num_cols = 2000;
    if (num_cols < 10) num_cols = 10;
    printf("Computing %d weekly columns...\n", num_cols);

    int64_t *col_ts = malloc((size_t)num_cols * sizeof(int64_t));
    for (int c = 0; c < num_cols; c++)
        col_ts[c] = view_start + (int64_t)c * 86400 * 7;

    int64_t *grid = calloc((size_t)(num_cols * NBUCKETS), sizeof(int64_t));

    for (size_t u = 0; u < n; u++) {
        if (u % 200000 == 0) printf("  UTXO %zu / %zu\n", u, n);
        int64_t cts = uts[u], val = uval[u];
        int start = 0;
        if (cts > col_ts[0]) {
            int lo = 0, hi = num_cols - 1;
            while (lo < hi) { int mid = (lo+hi)/2; if (col_ts[mid] < cts) lo = mid+1; else hi = mid; }
            start = lo;
        }
        for (int c = start; c < num_cols; c++) {
            int64_t age = col_ts[c] - cts;
            if (age < 0) continue;
            int b = NBUCKETS - 1;
            for (int i = 0; i < NBUCKETS - 1; i++)
                if (age < age_thresh[i]) { b = i; break; }
            grid[c * NBUCKETS + b] += val;
        }
    }
    free(uts); free(uval);
    printf("Rendering...\n");

    /* ── Layout ─────────────────────────────────────────────── */
    const int W = 1920, H = 1080;
    const int ML = 100, MR = 40, MT = 100, MB = 180;
    const int PW = W - ML - MR, PH = H - MT - MB;

    uint8_t *img = calloc((size_t)(W * H * 3), 1);

    /* Background gradient: dark navy */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int off = (y * W + x) * 3;
            img[off]   = 16;
            img[off+1] = 18;
            img[off+2] = 28;
        }

    /* Plot area: slightly lighter */
    fill_rect(img, W, H, ML, MT, PW, PH, 22, 24, 35);

    /* ── Stacked area with interpolation ────────────────────── */
    for (int px = 0; px < PW; px++) {
        double col_f = (double)px / PW * (num_cols - 1);

        /* Get interpolated fractions for each bucket */
        double fracs[NBUCKETS];
        for (int b = 0; b < NBUCKETS; b++)
            fracs[b] = lerp_bucket_frac(grid, num_cols, b, col_f);

        /* Stack: oldest at bottom */
        double y_bot = 0.0;
        for (int b = NBUCKETS - 1; b >= 0; b--) {
            double y_top = y_bot + fracs[b];

            int py_bot = MT + PH - (int)(y_bot * PH);
            int py_top = MT + PH - (int)(y_top * PH);
            if (py_top < MT) py_top = MT;
            if (py_bot > MT + PH) py_bot = MT + PH;

            int x = ML + px;
            for (int y = py_top; y < py_bot; y++) {
                int off = (y * W + x) * 3;
                img[off]   = colors[b][0];
                img[off+1] = colors[b][1];
                img[off+2] = colors[b][2];
            }
            y_bot = y_top;
        }
    }

    /* ── Y-axis labels (scale 2 = 12x20 px) ─────────────────── */
    for (int pct = 0; pct <= 100; pct += 25) {
        int y = MT + PH - (int)((pct / 100.0) * PH);

        if (pct > 0 && pct < 100) {
            for (int x = ML; x < ML + PW; x += 3) {
                int off = (y * W + x) * 3;
                img[off] = 50; img[off+1] = 52; img[off+2] = 65;
            }
        }

        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        int lx = ML - (int)strlen(buf) * FONT_W * 2 - 8;
        font_draw_string(img, W, H, lx, y - FONT_H, buf, 160, 165, 180, 2);
    }

    /* ── X-axis year labels (scale 2) ────────────────────────── */
    int first_year = start_year > 0 ? start_year : 2017;
    for (int year = first_year; year <= 2025; year++) {
        struct tm ytm = {0};
        ytm.tm_year = year - 1900; ytm.tm_mon = 0; ytm.tm_mday = 1;
        int64_t yts = (int64_t)timegm(&ytm);
        double frac = (double)(yts - view_start) / (tip_ts - view_start);
        int x = ML + (int)(frac * PW);
        if (x < ML || x >= ML + PW) continue;

        /* Tick */
        fill_rect(img, W, H, x, MT + PH, 2, 8, 100, 105, 120);

        /* Subtle vertical gridline */
        for (int y = MT; y < MT + PH; y += 3) {
            int off = (y * W + x) * 3;
            img[off] = 38; img[off+1] = 40; img[off+2] = 52;
        }

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", year);
        font_draw_string(img, W, H, x - 12 * 2, MT + PH + 14,
                         buf, 160, 165, 180, 2);
    }

    /* ── Consensus fork markers ──────────────────────────────── */
    struct { int height; const char *name; } forks[] = {
        {476969,  "Sapling"},
        {585318,  "Bubbles"},
        {707000,  "Buttercup"},
    };
    for (int f = 0; f < 3; f++) {
        int64_t fts = height_to_ts(forks[f].height);
        if (fts < view_start) continue;
        int fc = (int)((double)(fts - view_start) / (tip_ts - view_start) * PW);
        int fx = ML + fc;
        if (fx < ML || fx >= ML + PW) continue;

        /* White dashed line */
        for (int y = MT; y < MT + PH; y++) {
            if (y % 6 < 4) {
                int off = (y * W + fx) * 3;
                img[off] = 220; img[off+1] = 225; img[off+2] = 235;
                if (fx + 1 < ML + PW) {
                    off = (y * W + fx + 1) * 3;
                    img[off] = 220; img[off+1] = 225; img[off+2] = 235;
                }
            }
        }
        /* Label with background */
        int lw = (int)strlen(forks[f].name) * FONT_W * 2;
        fill_rect(img, W, H, fx + 4, MT + 4, lw + 6, FONT_H * 2 + 4,
                  30, 32, 45);
        font_draw_string(img, W, H, fx + 7, MT + 6,
                         forks[f].name, 220, 225, 240, 2);
    }

    /* ── Plot border ─────────────────────────────────────────── */
    for (int x = ML; x < ML + PW; x++) {
        int off = (MT * W + x) * 3;
        img[off] = img[off+1] = img[off+2] = 70;
        off = ((MT + PH) * W + x) * 3;
        img[off] = img[off+1] = img[off+2] = 70;
    }
    for (int y = MT; y < MT + PH; y++) {
        int off = (y * W + ML) * 3;
        img[off] = img[off+1] = img[off+2] = 70;
        off = (y * W + ML + PW - 1) * 3;
        img[off] = img[off+1] = img[off+2] = 70;
    }

    /* ── Title (scale 4 = 24x40 px) ──────────────────────────── */
    font_draw_string(img, W, H, ML, 15,
                     "ZClassic HODL Wave", 235, 240, 252, 4);

    /* Subtitle (scale 2) */
    int64_t final_total = 0, over_1y = 0;
    for (int b = 0; b < NBUCKETS; b++) {
        int64_t v = grid[(num_cols - 1) * NBUCKETS + b];
        final_total += v;
        if (b >= 5) over_1y += v;
    }
    double pct_1y = final_total > 0
        ? (double)over_1y / (double)final_total * 100.0 : 0.0;

    char subtitle[256];
    snprintf(subtitle, sizeof(subtitle),
             "%.0f%% of supply unmoved > 1 year  |  "
             "%.0f ZCL  |  %zu UTXOs",
             pct_1y, (double)final_total / 1e8, n);
    font_draw_string(img, W, H, ML, 62, subtitle, 130, 135, 155, 2);

    /* ── Legend (scale 2, bottom area) ────────────────────────── */
    int legend_y = MT + PH + 55;
    int swatch = 24;
    int col_w = PW / 5;

    for (int b = 0; b < NBUCKETS; b++) {
        int row = b / 5;
        int col = b % 5;
        int lx = ML + col * col_w;
        int ly = legend_y + row * 42;

        /* Color swatch with border */
        fill_rect(img, W, H, lx, ly, swatch, swatch,
                  colors[b][0], colors[b][1], colors[b][2]);
        for (int i = 0; i < swatch; i++) {
            int off = (ly * W + lx + i) * 3;
            img[off] = img[off+1] = img[off+2] = 90;
            off = ((ly + swatch - 1) * W + lx + i) * 3;
            img[off] = img[off+1] = img[off+2] = 90;
        }
        for (int i = 0; i < swatch; i++) {
            int off = ((ly + i) * W + lx) * 3;
            img[off] = img[off+1] = img[off+2] = 90;
            off = ((ly + i) * W + lx + swatch - 1) * 3;
            img[off] = img[off+1] = img[off+2] = 90;
        }

        /* Label + percentage (scale 2) */
        double bpct = final_total > 0
            ? (double)grid[(num_cols-1)*NBUCKETS + b] / (double)final_total * 100.0
            : 0.0;
        char label[64];
        snprintf(label, sizeof(label), "%s %.1f%%", bucket_labels[b], bpct);
        font_draw_string(img, W, H, lx + swatch + 8, ly + 2,
                         label, 185, 190, 205, 2);
    }

    /* ── Write PNG ──────────────────────────────────────────── */
    if (!png_write_rgb(out_path, img, (uint32_t)W, (uint32_t)H)) {
        fprintf(stderr, "Failed to write %s\n", out_path);
        free(img); free(grid); free(col_ts);
        return 1;
    }
    free(img); free(grid); free(col_ts);
    printf("Wrote %s\n", out_path);
    return 0;
}
