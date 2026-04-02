/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared macros and declarations for explorer subsystems. */

#ifndef ZCL_CONTROLLERS_EXPLORER_INTERNAL_H
#define ZCL_CONTROLLERS_EXPLORER_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>
#include "views/format_helpers.h"

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

/* Nav helper: emits <nav> with active class on the matching link.
 * Pass NULL for active to highlight nothing. */
static inline size_t explorer_emit_nav(char *buf, size_t max, const char *active)
{
    static const struct { const char *href; const char *label; const char *id; } links[] = {
        { "/explorer",          "Blocks",    "blocks"   },
        { "/explorer/stats",    "Stats",     "stats"    },
        { "/explorer/hodl",     "HODL Wave", "hodl"     },
        { "/explorer/tokens",   "Tokens",    "tokens"   },
        { "/explorer/events",   "Events",    "events"   },
        { "/explorer/factoids", "Factoids",  "factoids" },
    };
    size_t off = 0;
    APPEND(off, buf, max, "<nav class='nav'>");
    for (size_t i = 0; i < sizeof(links)/sizeof(links[0]); i++) {
        bool act = active && strcmp(active, links[i].id) == 0;
        APPEND(off, buf, max, "<a href='%s'%s>%s</a>",
               links[i].href, act ? " class='active'" : "", links[i].label);
    }
    APPEND(off, buf, max,
        "<div class='search'>"
        "<form action='/explorer/search' method='get'>"
        "<label for='explorer-search' class='sr-only'>Search</label>"
        "<input id='explorer-search' name='q' placeholder='Search block, tx, or address...' "
        "aria-label='Search blocks, transactions, or addresses'>"
        "</form></div></nav>");
    return off;
}

/* Legacy EXPLORER_NAV macro — kept for error pages and one-shot snprintf.
 * Does not highlight any active link; use explorer_emit_nav() for that. */
#define EXPLORER_NAV \
    "<nav class='nav'>" \
    "<a href='/explorer'>Blocks</a>" \
    "<a href='/explorer/stats'>Stats</a>" \
    "<a href='/explorer/hodl'>HODL Wave</a>" \
    "<a href='/explorer/tokens'>Tokens</a>" \
    "<a href='/explorer/events'>Events</a>" \
    "<a href='/explorer/factoids'>Factoids</a>" \
    "<div class='search'>" \
    "<form action='/explorer/search' method='get'>" \
    "<input name='q' placeholder='Search block, tx, or address...'>" \
    "</form></div></nav>"

#define EXPLORER_FOOTER \
    "<footer>ZClassic23 Block Explorer &mdash; Pure C23 &mdash; zclnet.net</footer>" \
    "</body></html>"

/* ── SQLite query helpers (DRY — one definition for all controllers) ── */

static inline int64_t sql_query_i64(sqlite3 *db, const char *sql)
{
    int64_t val = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            val = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

static inline int sql_query_int(sqlite3 *db, const char *sql)
{
    return (int)sql_query_i64(db, sql);
}

struct sql_row_i64_2 {
    int64_t v0;
    int64_t v1;
};

struct sql_row_i64_3 {
    int64_t v0;
    int64_t v1;
    int64_t v2;
};

struct explorer_token_stats {
    int64_t token_count;
    int64_t transfer_count;
};

struct explorer_address_stats {
    int64_t total;
    int64_t nonzero;
};

struct explorer_privacy_stats {
    int64_t joinsplits;
    int64_t sapling_spends;
    int64_t sapling_outputs;
    int64_t net_shielded_sat;
};

struct explorer_utxo_stats {
    int64_t count;
    int64_t dust_under_0001;
    int64_t total_value_sat;
};

struct explorer_op_return_stats {
    int64_t total;
    int64_t zslp;
};

struct explorer_transaction_stats {
    int64_t total;
    int64_t coinbase;
    int64_t inputs;
    int64_t outputs;
    int64_t empty_blocks;
};

struct explorer_chain_stats {
    int64_t height;
    int64_t blocks;
};

static inline bool sql_query_row_i64_2(sqlite3 *db, const char *sql,
                                       struct sql_row_i64_2 *out)
{
    sqlite3_stmt *s = NULL;

    if (out) {
        out->v0 = 0;
        out->v1 = 0;
    }
    if (!out)
        return false;

    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            out->v0 = sqlite3_column_int64(s, 0);
            out->v1 = sqlite3_column_int64(s, 1);
            sqlite3_finalize(s);
            return true;
        }
        sqlite3_finalize(s);
    }
    return false;
}

static inline bool sql_query_row_i64_3(sqlite3 *db, const char *sql,
                                       struct sql_row_i64_3 *out)
{
    sqlite3_stmt *s = NULL;

    if (out) {
        out->v0 = 0;
        out->v1 = 0;
        out->v2 = 0;
    }
    if (!out)
        return false;

    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            out->v0 = sqlite3_column_int64(s, 0);
            out->v1 = sqlite3_column_int64(s, 1);
            out->v2 = sqlite3_column_int64(s, 2);
            sqlite3_finalize(s);
            return true;
        }
        sqlite3_finalize(s);
    }
    return false;
}

static inline void explorer_query_token_stats(sqlite3 *db,
                                              struct explorer_token_stats *out)
{
    if (!out)
        return;

    out->token_count = sql_query_i64(db, "SELECT count(*) FROM zslp_tokens");
    out->transfer_count = sql_query_i64(db, "SELECT count(*) FROM zslp_transfers");
}

static inline void explorer_query_address_stats(sqlite3 *db,
                                                struct explorer_address_stats *out)
{
    if (!out)
        return;

    out->total = sql_query_i64(db, "SELECT count(*) FROM addresses");
    out->nonzero = sql_query_i64(db, "SELECT count(*) FROM addresses WHERE balance > 0");
}

static inline void explorer_query_privacy_stats(sqlite3 *db,
                                                struct explorer_privacy_stats *out)
{
    if (!out)
        return;

    out->joinsplits = sql_query_i64(db, "SELECT count(*) FROM joinsplits");
    out->sapling_spends = sql_query_i64(db, "SELECT count(*) FROM sapling_spends");
    out->sapling_outputs = sql_query_i64(db, "SELECT count(*) FROM sapling_outputs");
    out->net_shielded_sat = sql_query_i64(db, "SELECT COALESCE(SUM(sapling_value), 0) FROM blocks");
}

static inline void explorer_query_utxo_stats(sqlite3 *db,
                                             struct explorer_utxo_stats *out)
{
    if (!out)
        return;

    out->count = sql_query_i64(db, "SELECT count(*) FROM utxos");
    out->dust_under_0001 = sql_query_i64(db, "SELECT count(*) FROM utxos WHERE value < 100000");
    out->total_value_sat = sql_query_i64(db, "SELECT COALESCE(SUM(value),0) FROM utxos");
}

static inline void explorer_query_op_return_stats(sqlite3 *db,
                                                  struct explorer_op_return_stats *out)
{
    if (!out)
        return;

    out->total = sql_query_i64(db, "SELECT count(*) FROM op_returns");
    out->zslp = sql_query_i64(db, "SELECT count(*) FROM op_returns WHERE is_slp = 1");
}

static inline void explorer_query_transaction_stats(sqlite3 *db,
                                                    struct explorer_transaction_stats *out)
{
    if (!out)
        return;

    out->total = sql_query_i64(db, "SELECT count(*) FROM transactions");
    out->coinbase = sql_query_i64(db, "SELECT count(*) FROM transactions WHERE is_coinbase = 1");
    out->inputs = sql_query_i64(db, "SELECT count(*) FROM tx_inputs");
    out->outputs = sql_query_i64(db, "SELECT count(*) FROM tx_outputs");
    out->empty_blocks = sql_query_i64(db, "SELECT count(*) FROM blocks WHERE num_tx <= 1");
}

static inline void explorer_query_chain_stats(sqlite3 *db,
                                              struct explorer_chain_stats *out)
{
    if (!out)
        return;

    out->height = sql_query_i64(db, "SELECT MAX(height) FROM blocks");
    out->blocks = sql_query_i64(db, "SELECT count(*) FROM blocks");
}

static inline bool explorer_open_readonly_db(const char *datadir, sqlite3 **db_out)
{
    char dbpath[1024];

    if (db_out)
        *db_out = NULL;
    if (!datadir || !db_out)
        return false;

    snprintf(dbpath, sizeof(dbpath), "%s/node.db", datadir);
    if (sqlite3_open_v2(dbpath, db_out, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (*db_out) {
            sqlite3_close(*db_out);
            *db_out = NULL;
        }
        return false;
    }

    sqlite3_busy_timeout(*db_out, 30000);
    return true;
}

static inline bool sql_query_text(sqlite3 *db, const char *sql,
                                   char *out, size_t max)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (t) { snprintf(out, max, "%s", t); sqlite3_finalize(s); return true; }
        }
        sqlite3_finalize(s);
    }
    if (max > 0) out[0] = '\0';
    return false;
}

/* ── Number formatting with comma separators ─────────────── */

static inline int format_with_commas(char *buf, size_t max, int64_t val)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%" PRId64, val);
    if (len <= 0 || (size_t)len >= sizeof(tmp)) { buf[0] = '\0'; return 0; }

    bool neg = (tmp[0] == '-');
    int digits_start = neg ? 1 : 0;
    int ndigits = len - digits_start;
    int ncommas = (ndigits - 1) / 3;
    int total = len + ncommas;

    if ((size_t)total >= max) { buf[0] = '\0'; return 0; }

    int src = len - 1;
    int dst = total;
    buf[dst--] = '\0';
    int count = 0;
    while (src >= digits_start) {
        buf[dst--] = tmp[src--];
        count++;
        if (count % 3 == 0 && src >= digits_start)
            buf[dst--] = ',';
    }
    if (neg) buf[dst] = '-';
    return total;
}

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
        whole = (-zatoshi) / ZATOSHI_PER_ZCL;
        frac = (-zatoshi) % ZATOSHI_PER_ZCL;
        snprintf(buf, max, "-%" PRId64 ".%08" PRId64, whole, frac);
    } else {
        whole = zatoshi / ZATOSHI_PER_ZCL;
        frac = zatoshi % ZATOSHI_PER_ZCL;
        snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
    }
}

/* Shared difficulty calculation — canonical version in chain/pow.h */
#include "chain/pow.h"
static inline double explorer_difficulty_from_bits(uint32_t bits)
{
    return difficulty_from_bits(bits);
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

#define BUTTERCUP_ACTIVATION_HEIGHT 707000
#define PRE_BC_HALVING   840000
#define POST_BC_HALVING  1680000
#define BASE_SUBSIDY_SAT 1250000000LL  /* 12.5 ZCL */

static inline int64_t zcl_total_supply_zatoshi(int64_t height)
{
    if (height <= 0) return 0;

    const int64_t base = 1250000000LL;   /* 12.5 ZCL in zatoshi */
    const int64_t buttercup = 707000;
    const int64_t post_interval = 1680000;
    const int64_t post_base = base / 2;  /* 625000000 (spacing ratio = 2) */

    int64_t total = 0;

    /* Block 0: slow-start -> 0 zatoshi. Block 1: full 12.5 ZCL. */
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

/* Aliases used by factoids, API, and stats code */
static inline int64_t compute_supply_at_height(int64_t height)
{
    return zcl_total_supply_zatoshi(height);
}

static inline double supply_zcl_at_height(int64_t height)
{
    return (double)zcl_total_supply_zatoshi(height) / (double)ZATOSHI_PER_ZCL;
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
