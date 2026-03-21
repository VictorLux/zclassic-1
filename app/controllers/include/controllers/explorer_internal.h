/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared macros and declarations for explorer subsystems. */

#ifndef ZCL_CONTROLLERS_EXPLORER_INTERNAL_H
#define ZCL_CONTROLLERS_EXPLORER_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>

/* ── Append helper ─────────────────────────────────────────── */
#define APPEND(off, buf, max, ...) do { \
    if ((off) < (max)) { \
        size_t _rem = (max) - (off); \
        int _n = snprintf((char *)(buf) + (off), _rem, __VA_ARGS__); \
        if (_n > 0) { \
            (off) += ((size_t)_n < _rem) ? (size_t)_n : _rem - 1; \
        } \
    } \
} while(0)

/* ── Page template macros ──────────────────────────────────── */
#define EXPLORER_HEADER(title) \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: text/html; charset=utf-8\r\n" \
    "Connection: close\r\n\r\n" \
    "<!DOCTYPE html><html><head><meta charset='utf-8'>" \
    "<meta name='viewport' content='width=device-width,initial-scale=1'>" \
    "<title>" title "</title>" \
    "<link rel='icon' type='image/png' href='/explorer/favicon.png'>" \
    "<link rel='stylesheet' href='/explorer/style.css'>" \
    "</head><body>"

#define EXPLORER_NAV \
    "<div class='nav'>" \
    "<a href='/explorer'>Blocks</a>" \
    "<a href='/explorer/stats'>Stats</a>" \
    "<a href='/explorer/hodl'>HODL Wave</a>" \
    "<a href='/explorer/tokens'>Tokens</a>" \
    "<a href='/explorer/factoids'>Factoids</a>" \
    "<div class='search'>" \
    "<form action='/explorer/search' method='get'>" \
    "<input name='q' placeholder='Search block, tx, or address...'>" \
    "</form></div></div>"

#define EXPLORER_FOOTER \
    "<footer>ZClassic23 Block Explorer &mdash; Pure C23 &mdash; zclnet.net</footer>" \
    "</body></html>"

/* ── Shared formatting helpers (static inline for header-only use) ── */

static inline void explorer_format_time(char *buf, size_t max, uint32_t t)
{
    time_t ts = (time_t)t;
    struct tm tm;
    gmtime_r(&ts, &tm);
    strftime(buf, max, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

static inline void explorer_format_zcl(char *buf, size_t max, int64_t zatoshi)
{
    int64_t whole, frac;
    if (zatoshi < 0) {
        whole = (-zatoshi) / 100000000LL;
        frac = (-zatoshi) % 100000000LL;
        snprintf(buf, max, "-%" PRId64 ".%08" PRId64, whole, frac);
    } else {
        whole = zatoshi / 100000000LL;
        frac = zatoshi % 100000000LL;
        snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
    }
}

static inline double explorer_difficulty_from_bits(uint32_t bits)
{
    if (bits == 0) return 1.0;
    int exp = (int)((bits >> 24) & 0xff);
    double target = (double)(bits & 0x00ffffff) * pow(256.0, exp - 3);
    double pow_limit = (double)0x07ffff * pow(256.0, 0x1f - 3);
    return (target > 0) ? pow_limit / target : 1.0;
}

static inline void explorer_format_y_label(char *buf, size_t max, double val)
{
    double av = val < 0 ? -val : val;
    if (av >= 1e9)       snprintf(buf, max, "%.1fG", val / 1e9);
    else if (av >= 1e6)  snprintf(buf, max, "%.1fM", val / 1e6);
    else if (av >= 1e4)  snprintf(buf, max, "%.0fK", val / 1e3);
    else if (av >= 1e3)  snprintf(buf, max, "%.1fK", val / 1e3);
    else if (av >= 100)  snprintf(buf, max, "%.0f", val);
    else if (av >= 10)   snprintf(buf, max, "%.1f", val);
    else if (av >= 1)    snprintf(buf, max, "%.2f", val);
    else if (av >= 0.01) snprintf(buf, max, "%.3f", val);
    else                 snprintf(buf, max, "%.4f", val);
}

/* ── Correct ZClassic supply calculation ───────────────────────
 *
 * Matches get_block_subsidy() + consensus_halving() from consensus code.
 * The chain has three subsidy eras:
 *   Block 0:          0 ZCL (slow-start first half, nSubsidySlowStartInterval=2)
 *   Block 1:          12.5 ZCL (slow-start second half)
 *   Blocks 2-706999:  12.5 ZCL (pre-Buttercup, 0 halvings)
 *   Blocks 707000+:   base/2 >> (era+3), where era = (h-1-707000)/1680000
 *
 * Post-Buttercup subsidy per block:
 *   Era 0 (707000-2387000):   0.78125 ZCL   (625000000>>3 = 78125000 zatoshi)
 *   Era 1 (2387001-4067000):  0.390625 ZCL  (625000000>>4 = 39062500 zatoshi)
 *   Era 2 (4067001-5747000):  0.1953125 ZCL (625000000>>5 = 19531250 zatoshi)
 *   ...
 *
 * Returns total supply in zatoshi (int64_t). Overflow-safe for all heights.
 */
static inline int64_t zcl_total_supply_zatoshi(int64_t height)
{
    if (height <= 0) return 0;

    const int64_t base = 1250000000LL;   /* 12.5 ZCL in zatoshi */
    const int64_t buttercup = 707000;
    const int64_t post_interval = 1680000;
    const int64_t post_base = base / 2;  /* 625000000 (spacing ratio = 2) */

    int64_t total = 0;

    /* Block 0: slow-start → 0 zatoshi. Block 1: full 12.5 ZCL. */
    total += base;
    if (height == 1) return total;

    /* Pre-Buttercup: blocks 2..min(height, 706999) at 12.5 ZCL each.
     * No halvings occur pre-Buttercup (first would be at block 840001). */
    int64_t pre_end = height < buttercup ? height : buttercup - 1;
    total += (pre_end - 1) * base;  /* count = pre_end - 2 + 1 = pre_end - 1 */
    if (height < buttercup) return total;

    /* Post-Buttercup: iterate through halving eras.
     * consensus_halving() returns (h-1-707000)/1680000 + 3.
     * Era 0: blocks 707000..2387000  (1680001 blocks)
     * Era k>0: blocks (707000+k*1680000+1)..(707000+(k+1)*1680000) (1680000 blocks) */
    for (int era = 0; era < 61; era++) {
        int64_t subsidy = post_base >> (era + 3);
        if (subsidy == 0) break;

        int64_t era_start = buttercup + (int64_t)era * post_interval
                            + (era > 0 ? 1 : 0);
        int64_t era_end   = buttercup + (int64_t)(era + 1) * post_interval;

        if (era_start > height) break;
        if (era_end > height) era_end = height;

        total += (era_end - era_start + 1) * subsidy;
    }

    return total;
}

/* SVG line chart — renders into buffer at *off, advances *off */
static inline void explorer_svg_line_chart(char *out, size_t max, size_t *off,
                            const char *title, const char *color,
                            double *values, const char labels[][20],
                            int count, const char *y_label)
{
    if (count < 2) return;

    double min_v = values[0], max_v = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < min_v) min_v = values[i];
        if (values[i] > max_v) max_v = values[i];
    }
    if (max_v == min_v) max_v = min_v + 1;

    double pos_min = min_v > 0 ? min_v : 0.01;
    double pos_max = max_v > 0 ? max_v : 1;
    bool use_log = (pos_max / pos_min > 100);

    double range = max_v - min_v;
    double log_min = 0, log_range = 1;
    if (use_log) {
        log_min = log10(pos_min > 0 ? pos_min : 0.01);
        double log_max = log10(pos_max);
        log_range = log_max - log_min;
        if (log_range < 0.1) log_range = 0.1;
    }

    int w = 800, h = 300, pad_l = 90, pad_r = 20, pad_t = 40, pad_b = 60;
    int plot_w = w - pad_l - pad_r;
    int plot_h = h - pad_t - pad_b;

    APPEND(*off, out, max,
        "<svg viewBox='0 0 %d %d' style='width:100%%;max-width:%dpx;height:auto;"
        "background:#0c0c0c;border-radius:8px;margin:4px 0'>",
        w, h, w);

    if (title && title[0])
        APPEND(*off, out, max,
            "<text x='%d' y='25' fill='#33ff99' font-size='16' font-weight='600'>%s%s</text>",
            pad_l, title, use_log ? " (log scale)" : "");

    for (int i = 0; i <= 4; i++) {
        int y = pad_t + plot_h - (plot_h * i / 4);
        double val;
        if (use_log) {
            double log_val = log_min + log_range * i / 4.0;
            val = pow(10.0, log_val);
        } else {
            val = min_v + range * i / 4.0;
        }
        char lbl[32];
        explorer_format_y_label(lbl, sizeof(lbl), val);
        APPEND(*off, out, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1a1a1a' stroke-width='1'/>"
            "<text x='%d' y='%d' fill='#666' font-size='13' text-anchor='end'>%s</text>",
            pad_l, y, w - pad_r, y,
            pad_l - 10, y + 5, lbl);
    }

    APPEND(*off, out, max,
        "<text x='14' y='%d' fill='#888' font-size='12' "
        "transform='rotate(-90,14,%d)' text-anchor='middle'>%s</text>",
        pad_t + plot_h / 2, pad_t + plot_h / 2, y_label);

    #define VAL_TO_Y(v) (use_log \
        ? (pad_t + plot_h - (int)(((log10((v) > 0 ? (v) : 0.01)) - log_min) / log_range * plot_h)) \
        : (pad_t + plot_h - (int)(((v) - min_v) / range * plot_h)))

    APPEND(*off, out, max, "<polyline fill='none' stroke='%s' stroke-width='2.5' "
        "stroke-linejoin='round' points='", color);

    for (int i = 0; i < count; i++) {
        int x = pad_l + plot_w * i / (count - 1);
        int y = VAL_TO_Y(values[i]);
        APPEND(*off, out, max, "%d,%d ", x, y);
    }
    APPEND(*off, out, max, "'/>");

    APPEND(*off, out, max,
        "<polyline fill='%s' fill-opacity='0.1' stroke='none' points='%d,%d ",
        color, pad_l, pad_t + plot_h);
    for (int i = 0; i < count; i++) {
        int x = pad_l + plot_w * i / (count - 1);
        int y = VAL_TO_Y(values[i]);
        APPEND(*off, out, max, "%d,%d ", x, y);
    }
    APPEND(*off, out, max, "%d,%d '/>", w - pad_r, pad_t + plot_h);

    #undef VAL_TO_Y

    int label_step = count > 10 ? count / 6 : 1;
    for (int i = 0; i < count; i += label_step) {
        int x = pad_l + plot_w * i / (count - 1);
        APPEND(*off, out, max,
            "<text x='%d' y='%d' fill='#666' font-size='11' text-anchor='middle'>%s</text>",
            x, h - 10, labels[i]);
    }

    APPEND(*off, out, max, "</svg>");
}

#endif
