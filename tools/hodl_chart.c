/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HODL Wave Chart — newspaper-quality line graph.
 * Shows % of ZCL supply unmoved for 1+ year over the past 4 years.
 * Gets real data from hodltimeseries RPC, renders PNG, opens X11. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "util/png_writer.h"
#include "util/bitmap_font.h"

#define IMG_W 1200
#define IMG_H 640
#define PAD_L 90
#define PAD_R 50
#define PAD_T 70
#define PAD_B 70
#define CHART_W (IMG_W - PAD_L - PAD_R)
#define CHART_H (IMG_H - PAD_T - PAD_B)

static uint8_t pixels[IMG_H][IMG_W][3];

static void px(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < IMG_W && y >= 0 && y < IMG_H) {
        pixels[y][x][0] = r; pixels[y][x][1] = g; pixels[y][x][2] = b;
    }
}

static void fill(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            px(x, y, r, g, b);
}

static void thick_line(int x0, int y0, int x1, int y1, int t,
                        uint8_t r, uint8_t g, uint8_t b)
{
    int dx = abs(x1-x0), dy = abs(y1-y0);
    int sx = x0<x1?1:-1, sy = y0<y1?1:-1;
    int err = dx-dy, half = t/2;
    for (;;) {
        for (int ty=-half; ty<=half; ty++)
            for (int tx=-half; tx<=half; tx++)
                px(x0+tx, y0+ty, r, g, b);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void text(int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    font_draw_string((uint8_t*)pixels, IMG_W, IMG_H, x, y, s, r, g, b, scale);
}

int main(void)
{
    /* Get real data from node */
    FILE *fp = popen("./zclassic-cli hodltimeseries 9 2>/dev/null", "r");
    if (!fp) fp = popen("zclassic-cli hodltimeseries 9 2>/dev/null", "r");
    if (!fp) { fprintf(stderr, "Cannot get data\n"); return 1; }
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf)-1, fp);
    pclose(fp);
    buf[n] = 0;

    /* Parse JSON array of {time, pct, supply_zcl} */
    int npts = 0;
    double pcts[128];
    int64_t times[128];
    double supplies[128];

    const char *p = buf;
    while (npts < 128 && (p = strstr(p, "\"time\":")) != NULL) {
        p += 7;
        times[npts] = atoll(p);
        const char *pp = strstr(p, "\"pct\":\"");
        if (!pp) break;
        pcts[npts] = atof(pp + 7);
        const char *sp = strstr(p, "\"supply_zcl\":\"");
        if (sp) supplies[npts] = atof(sp + 14);
        npts++;
    }

    if (npts < 2) { fprintf(stderr, "Not enough data (%d points)\n", npts); return 1; }

    double y_min = 100, y_max = 0;
    for (int i = 0; i < npts; i++) {
        if (pcts[i] < y_min) y_min = pcts[i];
        if (pcts[i] > y_max) y_max = pcts[i];
    }
    double y_lo = floor(y_min / 10) * 10;
    double y_hi = ceil(y_max / 10) * 10;
    if (y_lo < 0) y_lo = 0;
    if (y_hi > 100) y_hi = 100;
    if (y_hi - y_lo < 20) y_hi = y_lo + 20;

    int64_t t_min = times[0], t_max = times[npts-1];
    double t_range = (double)(t_max - t_min);

    /* Compute pixel positions */
    int xs[128], ys_px[128];
    for (int i = 0; i < npts; i++) {
        xs[i] = PAD_L + (int)(CHART_W * (double)(times[i] - t_min) / t_range);
        ys_px[i] = PAD_T + (int)(CHART_H * (1.0 - (pcts[i] - y_lo) / (y_hi - y_lo)));
    }

    /* ── Render ── */
    fill(0, 0, IMG_W, IMG_H, 255, 255, 255);
    fill(PAD_L, PAD_T, CHART_W, CHART_H, 252, 251, 247);

    /* Title */
    text(IMG_W/2 - 270, 12, "% of ZCL Supply Unmoved for 1+ Year", 3, 25, 25, 25);

    char subtitle[128];
    snprintf(subtitle, sizeof(subtitle),
             "Currently %.1f%% of %.0f ZCL  |  Full History from UTXO Set",
             pcts[npts-1], supplies[npts-1]);
    text(IMG_W/2 - 230, 44, subtitle, 2, 110, 110, 110);

    /* Y gridlines + labels */
    double ystep = (y_hi - y_lo > 50) ? 10 : 5;
    for (double v = y_lo; v <= y_hi + 0.1; v += ystep) {
        int y = PAD_T + (int)(CHART_H * (1.0 - (v - y_lo) / (y_hi - y_lo)));
        for (int x = PAD_L; x <= PAD_L + CHART_W; x++)
            px(x, y, 225, 222, 218);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f%%", v);
        text(PAD_L - 50, y - 5, lbl, 2, 100, 100, 100);
    }

    /* X axis: year labels */
    for (int yr = 0; yr <= 4; yr++) {
        int64_t t = t_min + (int64_t)(t_range * yr / 4);
        int x = PAD_L + CHART_W * yr / 4;
        for (int y = PAD_T; y <= PAD_T + CHART_H; y++)
            px(x, y, 225, 222, 218);
        time_t tt = (time_t)t;
        struct tm *tm = localtime(&tt);
        char lbl[16];
        strftime(lbl, sizeof(lbl), "%b %Y", tm);
        text(x - 28, PAD_T + CHART_H + 12, lbl, 2, 100, 100, 100);
    }

    /* Axes */
    for (int y = PAD_T; y <= PAD_T + CHART_H; y++) px(PAD_L, y, 80, 80, 80);
    for (int x = PAD_L; x <= PAD_L + CHART_W; x++) px(x, PAD_T + CHART_H, 80, 80, 80);

    /* Fill area under curve */
    for (int i = 0; i < npts - 1; i++) {
        int bottom = PAD_T + CHART_H;
        for (int x = xs[i]; x <= xs[i+1]; x++) {
            double frac = (xs[i+1] > xs[i])
                ? (double)(x - xs[i]) / (xs[i+1] - xs[i]) : 0;
            int cy = ys_px[i] + (int)((ys_px[i+1] - ys_px[i]) * frac);
            for (int y = cy; y < bottom; y++) {
                double d = (double)(y - cy) / (double)(bottom - cy + 1);
                uint8_t cr = (uint8_t)(190 + 62*d);
                uint8_t cg = (uint8_t)(215 + 36*d);
                uint8_t cb = (uint8_t)(245 + 6*d);
                px(x, y, cr, cg, cb);
            }
        }
    }

    /* Line */
    for (int i = 0; i < npts - 1; i++)
        thick_line(xs[i], ys_px[i], xs[i+1], ys_px[i+1], 3, 30, 110, 195);

    /* Endpoint dot */
    for (int dy = -7; dy <= 7; dy++)
        for (int dx = -7; dx <= 7; dx++)
            if (dx*dx + dy*dy <= 49)
                px(xs[npts-1]+dx, ys_px[npts-1]+dy, 30, 110, 195);
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
            if (dx*dx + dy*dy <= 9)
                px(xs[npts-1]+dx, ys_px[npts-1]+dy, 255, 255, 255);

    char endlbl[16];
    snprintf(endlbl, sizeof(endlbl), "%.1f%%", pcts[npts-1]);
    text(xs[npts-1] - 55, ys_px[npts-1] - 22, endlbl, 2, 30, 110, 195);

    /* Start value label */
    char startlbl[16];
    snprintf(startlbl, sizeof(startlbl), "%.1f%%", pcts[0]);
    text(xs[0] + 8, ys_px[0] - 15, startlbl, 2, 30, 110, 195);

    /* Y-axis label */
    text(6, PAD_T + CHART_H/2 - 5, "% Supply", 2, 90, 90, 90);
    text(6, PAD_T + CHART_H/2 + 12, "1yr+ HODL", 2, 90, 90, 90);

    /* Footer */
    text(PAD_L, IMG_H - 16, "ZClassic C23 Full Node", 1, 170, 170, 170);
    time_t gen_time = time(NULL);
    struct tm *gt = localtime(&gen_time);
    char ds[32];
    strftime(ds, sizeof(ds), "%Y-%m-%d %H:%M", gt);
    text(IMG_W - 140, IMG_H - 16, ds, 1, 170, 170, 170);

    /* Write PNG */
    const char *outpath = "/tmp/hodl_1yr.png";
    if (!png_write_rgb(outpath, (const uint8_t *)pixels, IMG_W, IMG_H)) {
        fprintf(stderr, "Failed to write PNG\n"); return 1;
    }

    fprintf(stderr, "Wrote %s (%.1f%% HODL, %d data points from UTXO scan)\n",
            outpath, pcts[npts-1], npts);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-open %s 2>/dev/null &", outpath);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "feh %s 2>/dev/null &", outpath);
        (void)system(cmd);
    }

    return 0;
}
