/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HODL wave SVG chart generator. Pure C23.
 * Produces a smooth stacked-area chart with system font rendering.
 *
 * Build:  cc -std=c23 -O2 -o hodlwave_svg tools/hodlwave_svg.c -lsqlite3 -lm
 * Usage:  ./hodlwave_svg [node.db] [output.svg] [start_year] */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define GENESIS_TIME  1478403829LL
#define BUTTERCUP_HT  707000
#define NBUCKETS      10

static int64_t height_to_ts(int h)
{
    if (h <= 0) return GENESIS_TIME;
    if (h < BUTTERCUP_HT) return GENESIS_TIME + (int64_t)h * 150;
    return GENESIS_TIME + (int64_t)BUTTERCUP_HT * 150
                        + (int64_t)(h - BUTTERCUP_HT) * 75;
}

static const int64_t age_thresh[NBUCKETS] = {
    86400, 604800, 2592000, 2592000LL*3, 2592000LL*6,
    31557600LL, 31557600LL*2, 31557600LL*3, 31557600LL*5, 0
};

/* Warm (young/bottom) → Cool (old/top): canonical Unchained ordering */
static const char *bucket_labels[NBUCKETS] = {
    "< 1 day", "1d \xe2\x80\x93 1w", "1w \xe2\x80\x93 1m",
    "1 \xe2\x80\x93 3m", "3 \xe2\x80\x93 6m", "6 \xe2\x80\x93 12m",
    "1 \xe2\x80\x93 2y", "2 \xe2\x80\x93 3y", "3 \xe2\x80\x93 5y", "> 5 years"
};

/* XML-safe labels for SVG text elements */
static const char *bucket_labels_xml[NBUCKETS] = {
    "&lt; 1 day", "1d &#x2013; 1w", "1w &#x2013; 1m",
    "1 &#x2013; 3m", "3 &#x2013; 6m", "6 &#x2013; 12m",
    "1 &#x2013; 2y", "2 &#x2013; 3y", "3 &#x2013; 5y", "&gt; 5 years"
};

/* Warm→Cool gradient: red, orange, gold, lime, green, teal, sky, blue, indigo, purple */
static const char *band_colors[NBUCKETS] = {
    "#ff3c28", "#ff8c1e", "#ffd72d", "#c3f037", "#50dc4b",
    "#23c89e", "#2d9be6", "#4b5fd7", "#8241c3", "#5f288c"
};

/* Lighter version for hover/legend swatches */
static const char *band_colors_light[NBUCKETS] = {
    "#ff6650", "#ffa648", "#ffe05a", "#d4f560", "#72e56b",
    "#4ad8b4", "#55b0ee", "#6b7ce0", "#9a60d0", "#7840a0"
};

int main(int argc, char **argv)
{
    const char *db_path = argc > 1 ? argv[1] : "/home/bob/.zclassic-c23/node.db";
    const char *out_path = argc > 2 ? argv[2] : "/tmp/hodlwave.svg";
    int start_year = argc > 3 ? atoi(argv[3]) : 0;

    sqlite3 *db;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL)) {
        fprintf(stderr, "Cannot open %s\n", db_path); return 1;
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
        if (n >= cap) { cap *= 2; uts = realloc(uts, cap * 8); uval = realloc(uval, cap * 8); }
        uts[n] = height_to_ts(sqlite3_column_int(stmt, 0));
        uval[n] = sqlite3_column_int64(stmt, 1);
        n++;
    }
    sqlite3_finalize(stmt); sqlite3_close(db);
    printf("Loaded %zu UTXOs\n", n);

    int64_t tip_ts = height_to_ts(3043007);
    int64_t view_start = GENESIS_TIME;
    if (start_year >= 2017 && start_year <= 2025) {
        struct tm sy = {0}; sy.tm_year = start_year - 1900; sy.tm_mday = 1;
        view_start = (int64_t)timegm(&sy);
    }

    /* Weekly columns */
    int NC = (int)((tip_ts - view_start) / (86400 * 7)) + 1;
    if (NC > 2000) NC = 2000;
    printf("Computing %d weekly columns...\n", NC);

    int64_t *cts = malloc((size_t)NC * 8);
    for (int c = 0; c < NC; c++) cts[c] = view_start + (int64_t)c * 86400 * 7;

    int64_t *grid = calloc((size_t)(NC * NBUCKETS), 8);

    for (size_t u = 0; u < n; u++) {
        if (u % 200000 == 0) printf("  %zu / %zu\n", u, n);
        int64_t ct = uts[u], val = uval[u];
        int s = 0;
        if (ct > cts[0]) {
            int lo = 0, hi = NC - 1;
            while (lo < hi) { int m = (lo+hi)/2; if (cts[m] < ct) lo = m+1; else hi = m; }
            s = lo;
        }
        for (int c = s; c < NC; c++) {
            int64_t age = cts[c] - ct;
            if (age < 0) continue;
            int b = NBUCKETS - 1;
            for (int i = 0; i < NBUCKETS - 1; i++)
                if (age < age_thresh[i]) { b = i; break; }
            grid[c * NBUCKETS + b] += val;
        }
    }
    free(uts); free(uval);

    /* Compute stacked fractions with smoothing */
    double *fracs = calloc((size_t)(NC * NBUCKETS), sizeof(double));
    for (int c = 0; c < NC; c++) {
        int64_t tot = 0;
        for (int b = 0; b < NBUCKETS; b++) tot += grid[c * NBUCKETS + b];
        if (tot > 0)
            for (int b = 0; b < NBUCKETS; b++)
                fracs[c * NBUCKETS + b] = (double)grid[c * NBUCKETS + b] / (double)tot;
    }

    /* 3-point moving average for smoother curves */
    double *smooth = calloc((size_t)(NC * NBUCKETS), sizeof(double));
    for (int c = 0; c < NC; c++)
        for (int b = 0; b < NBUCKETS; b++) {
            double sum = fracs[c * NBUCKETS + b];
            int cnt = 1;
            if (c > 0) { sum += fracs[(c-1) * NBUCKETS + b]; cnt++; }
            if (c < NC-1) { sum += fracs[(c+1) * NBUCKETS + b]; cnt++; }
            smooth[c * NBUCKETS + b] = sum / cnt;
        }
    free(fracs);

    /* Compute cumulative Y positions for stacking (young at bottom, old at top) */
    double *ybot = calloc((size_t)(NC * NBUCKETS), sizeof(double));
    double *ytop = calloc((size_t)(NC * NBUCKETS), sizeof(double));
    for (int c = 0; c < NC; c++) {
        double cum = 0;
        for (int b = 0; b < NBUCKETS; b++) {
            ybot[c * NBUCKETS + b] = cum;
            cum += smooth[c * NBUCKETS + b];
            ytop[c * NBUCKETS + b] = cum;
        }
    }

    /* Stats */
    int64_t final_total = 0, over_1y = 0;
    for (int b = 0; b < NBUCKETS; b++) {
        int64_t v = grid[(NC-1) * NBUCKETS + b];
        final_total += v;
        if (b >= 5) over_1y += v;
    }
    double pct_1y = final_total > 0 ? (double)over_1y / (double)final_total * 100.0 : 0;

    printf("Rendering SVG...\n");

    /* ── SVG output ─────────────────────────────────────────── */
    const double VW = 1920, VH = 1080;
    const double ML = 140, MR = 30, MT = 160, MB = 250;
    const double PW = VW - ML - MR, PH = VH - MT - MB;

    FILE *f = fopen(out_path, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", out_path); return 1; }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               "viewBox=\"0 0 %.0f %.0f\" width=\"1920\" height=\"1080\">\n", VW, VH);

    /* Background */
    fprintf(f, "<defs>\n");
    fprintf(f, "  <linearGradient id=\"bg\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">\n");
    fprintf(f, "    <stop offset=\"0%%\" stop-color=\"#12141e\"/>\n");
    fprintf(f, "    <stop offset=\"100%%\" stop-color=\"#0e1018\"/>\n");
    fprintf(f, "  </linearGradient>\n");
    fprintf(f, "</defs>\n");
    fprintf(f, "<rect width=\"%.0f\" height=\"%.0f\" fill=\"url(#bg)\"/>\n", VW, VH);

    /* Plot area */
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" "
               "fill=\"#161822\" rx=\"4\"/>\n", ML, MT, PW, PH);

    /* Stacked area bands: draw from top (oldest) to bottom (youngest) */
    for (int b = NBUCKETS - 1; b >= 0; b--) {
        fprintf(f, "<path d=\"M%.1f,%.1f ", ML, MT + PH);

        /* Top edge: left to right */
        for (int c = 0; c < NC; c++) {
            double x = ML + (double)c / (NC - 1) * PW;
            double y = MT + PH - ytop[c * NBUCKETS + b] * PH;
            fprintf(f, "L%.2f,%.2f ", x, y);
        }

        /* Bottom edge: right to left */
        for (int c = NC - 1; c >= 0; c--) {
            double x = ML + (double)c / (NC - 1) * PW;
            double y = MT + PH - ybot[c * NBUCKETS + b] * PH;
            fprintf(f, "L%.2f,%.2f ", x, y);
        }

        fprintf(f, "Z\" fill=\"%s\" opacity=\"0.92\"/>\n", band_colors[b]);
    }

    /* ── Cumulative boundary lines: 6-month and 1-year ──────── */
    /* In our stacking (young=bottom, old=top):
     * Top of bucket 4 (3-6m) = 6-month boundary
     * Top of bucket 5 (6-12m) = 1-year boundary
     * Everything ABOVE the line is older than that threshold. */
    struct { int bucket; const char *label_prefix; const char *color; const char *dash; } bounds[] = {
        { 4, "6mo", "#ffffff", "6,3" },
        { 5, "1yr", "#ffdd44", "none" },
    };
    for (int bi = 0; bi < 2; bi++) {
        int bkt = bounds[bi].bucket;

        /* Draw the boundary polyline */
        fprintf(f, "<path d=\"M");
        for (int c = 0; c < NC; c++) {
            double x = ML + (double)c / (NC - 1) * PW;
            double y = MT + PH - ytop[c * NBUCKETS + bkt] * PH;
            fprintf(f, "%s%.2f,%.2f", c > 0 ? " L" : "", x, y);
        }
        fprintf(f, "\" fill=\"none\" stroke=\"%s\" stroke-width=\"3\" "
                   "stroke-dasharray=\"%s\" opacity=\"0.85\"/>\n",
                bounds[bi].color, bounds[bi].dash);

        /* Label at right edge showing current % above this line */
        double right_y = MT + PH - ytop[(NC-1) * NBUCKETS + bkt] * PH;
        double pct_above = (1.0 - ytop[(NC-1) * NBUCKETS + bkt]) * 100.0;

        /* Background pill */
        fprintf(f, "<rect x=\"%.0f\" y=\"%.1f\" width=\"200\" height=\"38\" "
                   "rx=\"6\" fill=\"#10121c\" fill-opacity=\"0.92\" "
                   "stroke=\"%s\" stroke-width=\"1.5\" stroke-opacity=\"0.7\"/>\n",
                ML + PW - 210, right_y - 19, bounds[bi].color);

        /* Label text */
        fprintf(f, "<text x=\"%.0f\" y=\"%.1f\" fill=\"%s\" "
                   "font-family=\"'Segoe UI',Arial,sans-serif\" "
                   "font-size=\"24\" font-weight=\"700\">"
                   "&gt; %s: %.1f%%</text>\n",
                ML + PW - 200, right_y + 8, bounds[bi].color,
                bounds[bi].label_prefix, pct_above);

        /* Also label at left edge */
        double left_y = MT + PH - ytop[0 * NBUCKETS + bkt] * PH;
        double left_pct = (1.0 - ytop[0 * NBUCKETS + bkt]) * 100.0;
        fprintf(f, "<rect x=\"%.0f\" y=\"%.1f\" width=\"190\" height=\"36\" "
                   "rx=\"6\" fill=\"#10121c\" fill-opacity=\"0.92\" "
                   "stroke=\"%s\" stroke-width=\"1.5\" stroke-opacity=\"0.7\"/>\n",
                ML + 6, left_y - 18, bounds[bi].color);
        fprintf(f, "<text x=\"%.0f\" y=\"%.1f\" fill=\"%s\" "
                   "font-family=\"'Segoe UI',Arial,sans-serif\" "
                   "font-size=\"22\" font-weight=\"700\">"
                   "&gt; %s: %.1f%%</text>\n",
                ML + 14, left_y + 7, bounds[bi].color,
                bounds[bi].label_prefix, left_pct);
    }

    /* Y-axis gridlines + labels */
    fprintf(f, "<g font-family=\"'Segoe UI','Helvetica Neue',Arial,sans-serif\" "
               "font-size=\"38\" font-weight=\"700\" fill=\"#e0e4f0\">\n");
    for (int pct = 0; pct <= 100; pct += 25) {
        double y = MT + PH - (pct / 100.0) * PH;
        if (pct > 0 && pct < 100)
            fprintf(f, "  <line x1=\"%.0f\" y1=\"%.1f\" x2=\"%.0f\" y2=\"%.1f\" "
                       "stroke=\"#2a2e3e\" stroke-width=\"0.5\" stroke-dasharray=\"4,4\"/>\n",
                    ML, y, ML + PW, y);
        fprintf(f, "  <text x=\"%.0f\" y=\"%.1f\" text-anchor=\"end\" "
                   "alignment-baseline=\"middle\">%d%%</text>\n",
                ML - 16, y, pct);
    }
    fprintf(f, "</g>\n");

    /* X-axis year labels */
    int first_year = start_year > 0 ? start_year : 2017;
    fprintf(f, "<g font-family=\"'Segoe UI','Helvetica Neue',Arial,sans-serif\" "
               "font-size=\"36\" font-weight=\"700\" fill=\"#e0e4f0\">\n");
    for (int year = first_year; year <= 2025; year++) {
        struct tm ytm = {0}; ytm.tm_year = year - 1900; ytm.tm_mday = 1;
        int64_t yts = (int64_t)timegm(&ytm);
        double frac = (double)(yts - view_start) / (tip_ts - view_start);
        double x = ML + frac * PW;
        if (x < ML || x > ML + PW) continue;

        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\" "
                   "stroke=\"#2a2e3e\" stroke-width=\"0.5\" stroke-dasharray=\"6,6\"/>\n",
                x, MT, x, MT + PH);
        fprintf(f, "  <line x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\" "
                   "stroke=\"#4a4e5e\" stroke-width=\"1\"/>\n",
                x, MT + PH, x, MT + PH + 8);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.0f\" text-anchor=\"middle\">%d</text>\n",
                x, MT + PH + 36, year);
    }
    fprintf(f, "</g>\n");

    /* Consensus fork markers */
    struct { int ht; const char *name; } forks[] = {
        {476969, "Sapling"}, {585318, "Bubbles"}, {707000, "Buttercup"}
    };
    for (int fi = 0; fi < 3; fi++) {
        int64_t fts = height_to_ts(forks[fi].ht);
        if (fts < view_start) continue;
        double frac = (double)(fts - view_start) / (tip_ts - view_start);
        double x = ML + frac * PW;
        if (x < ML || x > ML + PW) continue;

        fprintf(f, "<line x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\" "
                   "stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-dasharray=\"6,4\" "
                   "opacity=\"0.6\"/>\n", x, MT, x, MT + PH);
        fprintf(f, "<rect x=\"%.1f\" y=\"%.0f\" width=\"%d\" height=\"40\" "
                   "rx=\"6\" fill=\"#1a1d2a\" fill-opacity=\"0.93\" "
                   "stroke=\"#ffffff\" stroke-width=\"0.8\" stroke-opacity=\"0.5\"/>\n",
                x + 5, MT + 6, (int)strlen(forks[fi].name) * 17 + 20);
        fprintf(f, "<text x=\"%.1f\" y=\"%.0f\" fill=\"#f0f2ff\" "
                   "font-family=\"'Segoe UI',Arial,sans-serif\" font-size=\"28\" "
                   "font-weight=\"700\">%s</text>\n",
                x + 15, MT + 36, forks[fi].name);
    }

    /* Plot border */
    fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" "
               "fill=\"none\" stroke=\"#3a3e52\" stroke-width=\"1\" rx=\"4\"/>\n",
            ML, MT, PW, PH);

    /* Title */
    fprintf(f, "<text x=\"%.0f\" y=\"60\" fill=\"#f0f2ff\" "
               "font-family=\"'Segoe UI','Helvetica Neue',Arial,sans-serif\" "
               "font-size=\"68\" font-weight=\"800\" "
               "letter-spacing=\"1\">ZClassic HODL Wave</text>\n", ML);

    /* Subtitle */
    fprintf(f, "<text x=\"%.0f\" y=\"105\" fill=\"#c8d0ea\" "
               "font-family=\"'Segoe UI','Helvetica Neue',Arial,sans-serif\" "
               "font-size=\"34\" font-weight=\"600\">"
               "%.0f%% unmoved &gt; 1 year"
               "  \xe2\x94\x82  %.0f ZCL"
               "  \xe2\x94\x82  %zu UTXOs"
               "</text>\n", ML, pct_1y, (double)final_total / 1e8, n);

    /* Sub-subtitle */
    fprintf(f, "<text x=\"%.0f\" y=\"140\" fill=\"#a0a8c8\" "
               "font-family=\"'Segoe UI',Arial,sans-serif\" font-size=\"24\" "
               "font-weight=\"500\">"
               "Each band = %% of supply last moved within that age range"
               "</text>\n", ML);

    /* Legend */
    double leg_y = MT + PH + 50;
    double leg_col_w = PW / 5.0;
    fprintf(f, "<g font-family=\"'Segoe UI','Helvetica Neue',Arial,sans-serif\" font-size=\"30\">\n");

    for (int b = 0; b < NBUCKETS; b++) {
        int row = b / 5, col = b % 5;
        double lx = ML + col * leg_col_w;
        double ly = leg_y + row * 58;
        double bpct = final_total > 0
            ? (double)grid[(NC-1)*NBUCKETS + b] / (double)final_total * 100.0 : 0;

        fprintf(f, "  <rect x=\"%.1f\" y=\"%.1f\" width=\"32\" height=\"32\" "
                   "rx=\"6\" fill=\"%s\" stroke=\"#5a5e72\" stroke-width=\"1.5\"/>\n",
                lx, ly, band_colors[b]);
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" fill=\"#e4e8fa\" "
                   "font-weight=\"700\">%s</text>\n",
                lx + 42, ly + 24, bucket_labels_xml[b]);

        /* Percentage */
        double text_w = (b == 0 || b == 9) ? 135 : 115;
        fprintf(f, "  <text x=\"%.1f\" y=\"%.1f\" fill=\"#a0a8c8\" "
                   "font-size=\"26\" font-weight=\"600\">%.1f%%</text>\n",
                lx + 42 + text_w, ly + 24, bpct);
    }
    fprintf(f, "</g>\n");

    fprintf(f, "</svg>\n");
    fclose(f);

    free(grid); free(smooth); free(ybot); free(ytop); free(cts);
    printf("Wrote %s\n", out_path);
    return 0;
}
