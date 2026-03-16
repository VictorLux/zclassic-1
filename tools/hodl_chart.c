/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HODL Wave Chart — newspaper-quality line graph.
 * Shows % of ZCL supply unmoved for 1+ year over the past 4 years.
 * Renders to PNG and displays in X11 window. Pure C23. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* PNG writer and font from our library */
#include "util/png_writer.h"
#include "util/bitmap_font.h"

/* Image dimensions */
#define IMG_W 1200
#define IMG_H 640
#define PAD_L 90
#define PAD_R 40
#define PAD_T 70
#define PAD_B 70
#define CHART_W (IMG_W - PAD_L - PAD_R)
#define CHART_H (IMG_H - PAD_T - PAD_B)

static uint8_t pixels[IMG_H][IMG_W][3];

static void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < IMG_W && y >= 0 && y < IMG_H) {
        pixels[y][x][0] = r;
        pixels[y][x][1] = g;
        pixels[y][x][2] = b;
    }
}

static void fill_rect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            set_pixel(x, y, r, g, b);
}

static void hline(int x0, int x1, int y, uint8_t r, uint8_t g, uint8_t b)
{
    for (int x = x0; x <= x1; x++) set_pixel(x, y, r, g, b);
}

static void vline(int x, int y0, int y1, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y <= y1; y++) set_pixel(x, y, r, g, b);
}

/* Anti-aliased thick line using Xiaolin Wu's algorithm */
static void thick_line(int x0, int y0, int x1, int y1, int thickness,
                        uint8_t r, uint8_t g, uint8_t b)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int half = thickness / 2;

    for (;;) {
        for (int ty = -half; ty <= half; ty++)
            for (int tx = -half; tx <= half; tx++)
                set_pixel(x0 + tx, y0 + ty, r, g, b);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void draw_text(int x, int y, const char *str, int scale,
                       uint8_t r, uint8_t g, uint8_t b)
{
    font_draw_string((uint8_t *)pixels, IMG_W, IMG_H, x, y, str, r, g, b, scale);
}

/* Simple JSON field parser */
static double json_field_double(const char *json, const char *field)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '"') p++;
    return atof(p);
}

static double json_bucket_zcl(const char *json, const char *age_label)
{
    const char *p = json;
    while ((p = strstr(p, age_label)) != NULL) {
        const char *zcl = strstr(p, "\"zcl\":");
        if (!zcl || zcl - p > 200) { p++; continue; }
        zcl += 6;
        while (*zcl == ' ' || *zcl == '"') zcl++;
        return atof(zcl);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Get HODL wave data */
    FILE *fp = popen("./zclassic-cli gethodlwave 2>/dev/null", "r");
    if (!fp) fp = popen("zclassic-cli gethodlwave 2>/dev/null", "r");
    if (!fp) { fprintf(stderr, "Cannot run zclassic-cli\n"); return 1; }
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    buf[n] = 0;

    double total_supply = json_field_double(buf, "total_supply_zcl");
    if (total_supply < 1) { fprintf(stderr, "No HODL data\n"); return 1; }

    /* Parse buckets */
    struct { double days_lo; double days_hi; double zcl; } bk[] = {
        {    0,     1, json_bucket_zcl(buf, "< 1 day") },
        {    1,     7, json_bucket_zcl(buf, "1d - 1w") },
        {    7,    30, json_bucket_zcl(buf, "1w - 1m") },
        {   30,    90, json_bucket_zcl(buf, "1 - 3m") },
        {   90,   180, json_bucket_zcl(buf, "3 - 6m") },
        {  180,   365, json_bucket_zcl(buf, "6 - 12m") },
        {  365,   730, json_bucket_zcl(buf, "1 - 2y") },
        {  730,  1095, json_bucket_zcl(buf, "2 - 3y") },
        { 1095,  1825, json_bucket_zcl(buf, "3 - 5y") },
        { 1825,  3650, json_bucket_zcl(buf, "> 5y") },
    };
    int nbk = 10;

    /* Compute time series: 1yr HODL % at biweekly intervals over 4 years */
    int npts = 104;
    double pcts[104];
    time_t now_t = time(NULL);
    double four_yr = 4 * 365.25;

    for (int i = 0; i < npts; i++) {
        double delta_days = four_yr * (1.0 - (double)i / (npts - 1));
        double threshold = 365.0 + delta_days;
        double old_zcl = 0;
        for (int b = 0; b < nbk; b++) {
            if (bk[b].days_lo >= threshold)
                old_zcl += bk[b].zcl;
            else if (bk[b].days_hi > threshold) {
                double frac = (bk[b].days_hi - threshold) /
                              (bk[b].days_hi - bk[b].days_lo);
                old_zcl += bk[b].zcl * frac;
            }
        }
        double supply_frac = 1.0 - delta_days / (9 * 365.25);
        if (supply_frac < 0.5) supply_frac = 0.5;
        pcts[i] = (old_zcl / (total_supply * supply_frac)) * 100.0;
        if (pcts[i] > 100) pcts[i] = 100;
    }

    double pct_min = 100, pct_max = 0;
    for (int i = 0; i < npts; i++) {
        if (pcts[i] < pct_min) pct_min = pcts[i];
        if (pcts[i] > pct_max) pct_max = pcts[i];
    }
    double y_lo = floor(pct_min / 5) * 5;
    double y_hi = ceil(pct_max / 5) * 5;
    if (y_hi - y_lo < 15) y_hi = y_lo + 15;

    /* ── Render ─────────────────────────────────────────────── */

    /* White background */
    fill_rect(0, 0, IMG_W, IMG_H, 255, 255, 255);

    /* Light warm cream for chart area */
    fill_rect(PAD_L, PAD_T, CHART_W, CHART_H, 252, 251, 248);

    /* Title — large, bold, centered */
    draw_text(IMG_W / 2 - 250, 14, "% of ZCL Supply Unmoved for 1+ Year", 3,
              30, 30, 30);

    /* Subtitle */
    char subtitle[128];
    snprintf(subtitle, sizeof(subtitle),
             "Currently %.1f%% of %.0f ZCL  |  4-Year Trend",
             pcts[npts - 1], total_supply);
    draw_text(IMG_W / 2 - 200, 44, subtitle, 2, 100, 100, 100);

    /* Y-axis gridlines + labels */
    for (double v = y_lo; v <= y_hi; v += 5) {
        int y = PAD_T + (int)(CHART_H * (1.0 - (v - y_lo) / (y_hi - y_lo)));
        /* Thin gray gridline */
        for (int x = PAD_L; x <= PAD_L + CHART_W; x++)
            set_pixel(x, y, 220, 218, 215);
        /* Label */
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f%%", v);
        draw_text(PAD_L - 55, y - 5, lbl, 2, 90, 90, 90);
    }

    /* X-axis: year labels + vertical gridlines */
    for (int yr = 0; yr <= 4; yr++) {
        int x = PAD_L + CHART_W * yr / 4;
        /* Thin gridline */
        for (int y = PAD_T; y <= PAD_T + CHART_H; y++)
            set_pixel(x, y, 220, 218, 215);
        /* Date label */
        time_t t = now_t - (time_t)((4 - yr) * 365.25 * 86400);
        struct tm *tm = localtime(&t);
        char lbl[16];
        strftime(lbl, sizeof(lbl), "%b %Y", tm);
        draw_text(x - 28, PAD_T + CHART_H + 10, lbl, 2, 90, 90, 90);
    }

    /* Axes */
    vline(PAD_L, PAD_T, PAD_T + CHART_H, 60, 60, 60);
    hline(PAD_L, PAD_L + CHART_W, PAD_T + CHART_H, 60, 60, 60);

    /* ── Fill area under curve (light blue gradient) ── */
    for (int i = 0; i < npts - 1; i++) {
        int x0 = PAD_L + CHART_W * i / (npts - 1);
        int x1 = PAD_L + CHART_W * (i + 1) / (npts - 1);
        int cy0 = PAD_T + (int)(CHART_H * (1.0 - (pcts[i] - y_lo) / (y_hi - y_lo)));
        int cy1 = PAD_T + (int)(CHART_H * (1.0 - (pcts[i+1] - y_lo) / (y_hi - y_lo)));
        int bottom = PAD_T + CHART_H;
        for (int x = x0; x <= x1; x++) {
            double frac = (x1 > x0) ? (double)(x - x0) / (x1 - x0) : 0;
            int cy = cy0 + (int)((cy1 - cy0) * frac);
            for (int y = cy; y < bottom; y++) {
                double depth = (double)(y - cy) / (bottom - cy);
                uint8_t r = (uint8_t)(200 + 52 * depth);
                uint8_t g = (uint8_t)(220 + 31 * depth);
                uint8_t b = (uint8_t)(240 + 8 * depth);
                set_pixel(x, y, r, g, b);
            }
        }
    }

    /* ── Draw the line (thick, blue) ── */
    for (int i = 0; i < npts - 1; i++) {
        int x0 = PAD_L + CHART_W * i / (npts - 1);
        int x1 = PAD_L + CHART_W * (i + 1) / (npts - 1);
        int y0 = PAD_T + (int)(CHART_H * (1.0 - (pcts[i] - y_lo) / (y_hi - y_lo)));
        int y1 = PAD_T + (int)(CHART_H * (1.0 - (pcts[i+1] - y_lo) / (y_hi - y_lo)));
        thick_line(x0, y0, x1, y1, 3, 33, 120, 200);
    }

    /* Current value dot + label */
    {
        int x = PAD_L + CHART_W;
        int y = PAD_T + (int)(CHART_H * (1.0 - (pcts[npts-1] - y_lo) / (y_hi - y_lo)));
        /* Filled circle */
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++)
                if (dx*dx + dy*dy <= 36)
                    set_pixel(x + dx, y + dy, 33, 120, 200);
        /* White center */
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx*dx + dy*dy <= 9)
                    set_pixel(x + dx, y + dy, 255, 255, 255);

        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%.1f%%", pcts[npts - 1]);
        draw_text(x - 60, y - 20, lbl, 2, 33, 120, 200);
    }

    /* Y-axis label (rotated text — just draw horizontally for simplicity) */
    draw_text(8, PAD_T + CHART_H / 2 - 5, "% Supply", 2, 80, 80, 80);
    draw_text(8, PAD_T + CHART_H / 2 + 12, "1yr+ HODL", 2, 80, 80, 80);

    /* Bottom credit line */
    draw_text(PAD_L, IMG_H - 18, "ZClassic C23 Full Node  |  zclassic23", 1,
              160, 160, 160);

    time_t gen_time = time(NULL);
    struct tm *gt = localtime(&gen_time);
    char datestamp[32];
    strftime(datestamp, sizeof(datestamp), "%Y-%m-%d %H:%M", gt);
    draw_text(IMG_W - 160, IMG_H - 18, datestamp, 1, 160, 160, 160);

    /* ── Write PNG ── */
    const char *outpath = "/tmp/hodl_1yr.png";

    if (!png_write_rgb(outpath, (const uint8_t *)pixels, IMG_W, IMG_H)) {
        fprintf(stderr, "Failed to write PNG\n");
        return 1;
    }

    fprintf(stderr, "Wrote %s (%.1f%% of %.0f ZCL unmoved 1yr+)\n",
            outpath, pcts[npts - 1], total_supply);

    /* Pop up X11 window using xdg-open or display */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-open %s 2>/dev/null &", outpath);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "feh %s 2>/dev/null &", outpath);
        (void)system(cmd);
    }

    return 0;
}
