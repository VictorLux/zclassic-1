/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Explorer secondary pages: stats, tokens, factoids, hodl, events, names,
 * market, swaps, messages, css. Split from explorer_controller.c per wave
 * 6c. Includes the compute-thread implementations used by the prewarm
 * pipeline. See explorer_controller_internal.h for shared declarations
 * and controllers/explorer_internal.h for chart/SQL inline helpers. */

#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/explorer_stats.h"
#include "controllers/explorer_factoids.h"
#include "explorer_controller_internal.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "models/database.h"
#include "models/hodl_wave.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "services/hodl_history_service.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/template.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "views/format_helpers.h"
#include "views/wallet_templates_gen.h"
#include "zslp/slp.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Once-flags driving compute-thread spawning. Referenced from
 * explorer_controller.c's prewarm pipeline via the extern declarations
 * in explorer_controller_internal.h. */
_Atomic int g_stats_computing = 0;
_Atomic int g_tokens_computing = 0;
_Atomic int g_factoids_computing = 0;

__attribute__((unused))
static void svg_stacked_area(char *out, size_t max, size_t *off,
                              const char *title,
                              double bands[][50], int num_bands,
                              const char band_labels[][32],
                              const char band_colors[][10],
                              const char x_labels[][20], int count)
{
    if (count < 2) return;

    int w = 800, h = 350, pad_l = 60, pad_r = 20, pad_t = 40, pad_b = 80;
    int plot_w = w - pad_l - pad_r;
    int plot_h = h - pad_t - pad_b;

    APPEND(*off, out, max,
        "<div class='card'>"
        "<h3 style='color:#33ff99;margin:0 0 8px;font-size:20px'>%s</h3>"
        "<svg viewBox='0 0 %d %d' style='width:100%%;max-width:%dpx;height:auto;"
        "background:#0c0c0c;border-radius:8px'>",
        title, w, h, w);

    /* Draw stacked areas from bottom to top */
    for (int b = num_bands - 1; b >= 0; b--) {
        APPEND(*off, out, max,
            "<polygon fill='%s' fill-opacity='0.7' points='%d,%d ",
            band_colors[b], pad_l, pad_t + plot_h);

        for (int i = 0; i < count; i++) {
            double cumulative = 0;
            for (int k = 0; k <= b; k++) cumulative += bands[k][i];
            int x = pad_l + plot_w * i / (count - 1);
            int y = pad_t + plot_h - (int)(cumulative / 100.0 * plot_h);
            APPEND(*off, out, max, "%d,%d ", x, y);
        }
        APPEND(*off, out, max, "%d,%d '/>", w - pad_r, pad_t + plot_h);
    }

    /* X labels */
    int label_step = count > 10 ? count / 6 : 1;
    for (int i = 0; i < count; i += label_step) {
        int x = pad_l + plot_w * i / (count - 1);
        APPEND(*off, out, max,
            "<text x='%d' y='%d' fill='#666' font-size='11' text-anchor='middle'>%s</text>",
            x, h - pad_b + 16, x_labels[i]);
    }

    /* Legend */
    int lx = pad_l;
    int ly = h - 20;
    for (int b = 0; b < num_bands; b++) {
        APPEND(*off, out, max,
            "<rect x='%d' y='%d' width='12' height='12' fill='%s' rx='2'/>"
            "<text x='%d' y='%d' fill='#ccc' font-size='11'>%s</text>",
            lx, ly - 10, band_colors[b], lx + 16, ly, band_labels[b]);
        lx += 16 + 8 * (int)strlen(band_labels[b]) + 20;
    }

    APPEND(*off, out, max, "</svg></div>");
}

/* Stats page — computed in background thread, served from cache */
#define STATS_CACHE_SIZE (1024 * 1024) /* 1MB for comprehensive stats */
static char g_stats_cache[STATS_CACHE_SIZE] = "";
static size_t g_stats_cache_len = 0;

/* ── Disk cache helpers (survive restarts) ────────────────── */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

void cache_save(const char *name, const char *data, size_t len)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0] || len == 0) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "w");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

size_t cache_load(const char *name, char *buf, size_t max)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0]) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t len = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[len] = '\0';
    return len;
}

#pragma GCC diagnostic pop

void *stats_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving while recomputing */
    if (g_stats_cache_len == 0) {
        size_t disk_len = cache_load("stats", g_stats_cache, STATS_CACHE_SIZE);
        if (disk_len > 0) {
            g_stats_cache_len = disk_len;
            printf("Stats: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Stats background: computing comprehensive stats...\n");
    fflush(stdout);
    /* Compute fresh into a temp buffer so we don't blank the disk-loaded cache */
    char *tmp = zcl_malloc(STATS_CACHE_SIZE, "stats_cache_tmp");
    if (!tmp) { g_stats_computing = 0; LOG_NULL("explorer", "stats_compute_thread: malloc(%d) failed", STATS_CACHE_SIZE); }
    size_t len = explorer_stats_build((uint8_t *)tmp, STATS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        memcpy(g_stats_cache, tmp, len);
        g_stats_cache_len = len;
        cache_save("stats", g_stats_cache, len);
    }
    free(tmp);
    g_stats_computing = 0;
    return NULL;
}

/* (stats_query_int64, stats_query_double, stats_tab_css, and stats body
 * moved to explorer_stats.c — see explorer_stats_build()) */

size_t serve_stats(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_stats_cache_len > 0) {
        size_t copy = g_stats_cache_len < max ? g_stats_cache_len : max;
        memcpy(r, g_stats_cache, copy);
        return copy;
    }

    /* Not cached yet — trigger background computation if not running */
    explorer_start_once(&g_stats_computing, stats_compute_thread,
                        "stats_compute");
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:36px;color:#33ff99'>Loading Statistics...</h1>"
        "<p style='font-size:20px;color:#888'>Computing charts from blockchain data.</p>"
        "<p style='font-size:16px;color:#555'>Auto-refreshing every 3 seconds...</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── Factoids Page ────────────────────────────────────────── */

#define FACTOIDS_CACHE_SIZE (1024 * 1024)  /* 1MB — 17 sections with SHA3 */
static char g_factoids_cache[FACTOIDS_CACHE_SIZE] = "";
static size_t g_factoids_cache_len = 0;

void *factoids_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving */
    if (g_factoids_cache_len == 0) {
        size_t disk_len = cache_load("factoids", g_factoids_cache, FACTOIDS_CACHE_SIZE);
        if (disk_len > 0) {
            g_factoids_cache_len = disk_len;
            printf("Factoids: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Factoids background: computing historian data...\n");
    fflush(stdout);
    size_t len = explorer_factoids_build((uint8_t *)g_factoids_cache,
                                          FACTOIDS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        g_factoids_cache_len = len;
        cache_save("factoids", g_factoids_cache, len);
    }
    g_factoids_computing = 0;
    return NULL;
}

size_t serve_factoids(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_factoids_cache_len > 0) {
        size_t copy = g_factoids_cache_len < max ? g_factoids_cache_len : max;
        memcpy(r, g_factoids_cache, copy);
        return copy;
    }

    /* Not cached yet -- trigger background computation */
    explorer_start_once(&g_factoids_computing, factoids_compute_thread,
                        "factoids_compute");
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:36px;color:#33ff99'>Loading Factoids...</h1>"
        "<p style='font-size:20px;color:#888'>Computing historian data from blockchain.</p>"
        "<p style='font-size:16px;color:#555'>Auto-refreshing every 3 seconds...</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── ZSLP Tokens Page ─────────────────────────────────────── */

/* Tokens page cache — precomputed in background */
static char g_tokens_cache[131072] = "";
static size_t g_tokens_cache_len = 0;

void *tokens_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    printf("Tokens background: computing...\n");
    fflush(stdout);

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        g_tokens_computing = 0;
        LOG_NULL("explorer", "tokens_compute_thread: failed to open db %s", db_path);
    }
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);

    uint8_t *r = zcl_malloc(131072, "tokens_html_buf");
    if (!r) { sqlite3_close(db); g_tokens_computing = 0; LOG_NULL("explorer", "tokens_compute_thread: malloc(131072) failed"); }
    size_t max = 131072;
    size_t off = 0;

    APPEND(off, r, max, EXPLORER_HEADER("ZSLP Tokens"));
    off += explorer_emit_nav((char *)r + off, max - off, "tokens");

    /* Count tokens and transfers */
    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);
    int64_t token_count = token_stats.token_count;
    int64_t xfer_count = token_stats.transfer_count;

    APPEND(off, r, max,
        "<h1>ZSLP Tokens</h1>"
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Tokens Created</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Token Transfers</div></div>"
        "</div>"
        "<p style='color:#aaa;font-size:16px'>"
        "Simple Ledger Protocol (ZSLP) tokens on the ZClassic blockchain.</p>",
        token_count, xfer_count);

    /* Token list from SQLite */
    APPEND(off, r, max,
        "<h2>All Tokens (%" PRId64 ")</h2>"
        "<table><tr><th>Ticker</th><th>Name</th><th>Decimals</th>"
        "<th>Supply</th><th>Block</th></tr>",
        token_count);
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT ticker, name, decimals, total_minted, genesis_height, hex(token_id)"
                " FROM zslp_tokens ORDER BY genesis_height LIMIT 100",
                -1, &s, NULL) == SQLITE_OK) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && off + 512 < max) {
                const char *ticker = (const char *)sqlite3_column_text(s, 0);
                const char *name = (const char *)sqlite3_column_text(s, 1);
                int dec = sqlite3_column_int(s, 2);
                int64_t minted = sqlite3_column_int64(s, 3);
                int height = sqlite3_column_int(s, 4);
                const char *tid_hex = (const char *)sqlite3_column_text(s, 5);

                char safe_ticker[128] = "", safe_name[256] = "";
                html_escape(safe_ticker, sizeof(safe_ticker), ticker ? ticker : "");
                html_escape(safe_name, sizeof(safe_name), name ? name : "");

                /* Format supply with decimals */
                char supply[64];
                if (dec > 0 && dec <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < dec; d++) divisor *= 10;
                    snprintf(supply, sizeof(supply), "%" PRId64 ".%0*" PRId64,
                             minted / divisor, dec, minted % divisor);
                } else {
                    snprintf(supply, sizeof(supply), "%" PRId64, minted);
                }

                /* Build linkable token ID (reverse byte order for display) */
                char tid_link[65] = "";
                if (tid_hex && strlen(tid_hex) == 64) {
                    for (int k = 0; k < 32; k++) {
                        tid_link[k*2] = tid_hex[62-k*2];
                        tid_link[k*2+1] = tid_hex[63-k*2];
                    }
                    tid_link[64] = '\0';
                    /* lowercase */
                    for (int k = 0; k < 64; k++)
                        if (tid_link[k] >= 'A' && tid_link[k] <= 'F')
                            tid_link[k] += 32;
                }

                APPEND(off, r, max,
                    "<tr><td style='font-size:18px'>"
                    "<a href='/explorer/token/%s' style='color:#ff99ff;font-weight:700'>%s</a></td>"
                    "<td>%s</td>"
                    "<td>%d</td>"
                    "<td class='amount'>%s</td>"
                    "<td><a href='/explorer/block/%d'>%d</a></td></tr>",
                    tid_link, safe_ticker[0] ? safe_ticker : "(none)",
                    safe_name, dec, supply, height, height);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* Recent transfers */
    APPEND(off, r, max,
        "<h2>Recent Transfers</h2>"
        "<table><tr><th>Block</th><th>Type</th><th>Token</th>"
        "<th>Amount</th></tr>");
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT x.block_height, x.tx_type, x.amount, t.ticker, t.decimals "
                "FROM zslp_transfers x "
                "LEFT JOIN zslp_tokens t ON x.token_id = t.token_id "
                "ORDER BY x.block_height DESC LIMIT 50",
                -1, &s, NULL) == SQLITE_OK) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && off + 256 < max) {
                int height = sqlite3_column_int(s, 0);
                int tx_type = sqlite3_column_int(s, 1);
                int64_t amount = sqlite3_column_int64(s, 2);
                const char *ticker = (const char *)sqlite3_column_text(s, 3);
                int dec = sqlite3_column_int(s, 4);

                const char *type_str = tx_type == 1 ? "GENESIS" :
                                       tx_type == 2 ? "MINT" :
                                       tx_type == 3 ? "SEND" : "?";
                const char *type_class = tx_type == 1 ? "tag-cb" :
                                         tx_type == 2 ? "tag-shielded" : "tag-slp";

                char amt[64];
                if (dec > 0 && dec <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < dec; d++) divisor *= 10;
                    snprintf(amt, sizeof(amt), "%" PRId64 ".%0*" PRId64,
                             amount / divisor, dec, amount % divisor);
                } else {
                    snprintf(amt, sizeof(amt), "%" PRId64, amount);
                }

                char safe_ticker[64] = "";
                html_escape(safe_ticker, sizeof(safe_ticker), ticker ? ticker : "");

                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
                    "<td><span class='tag %s'>%s</span></td>"
                    "<td style='color:#ff99ff'>%s</td>"
                    "<td class='amount'>%s</td></tr>",
                    height, height, type_class, type_str,
                    safe_ticker[0] ? safe_ticker : "?", amt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    /* About section */
    APPEND(off, r, max,
        "<h2>About ZSLP</h2>"
        "<div class='card'>"
        "<p style='font-size:16px;line-height:1.8'>"
        "ZSLP (ZClassic Simple Ledger Protocol) enables custom tokens on the ZClassic blockchain. "
        "Based on the SLP specification, tokens are encoded in "
        "OP_RETURN outputs with no consensus changes required.</p>"
        "<div class='grid' style='margin-top:12px'>"
        "<div class='label'>GENESIS</div><div class='val'>Create a new token (ticker, name, supply, decimals)</div>"
        "<div class='label'>SEND</div><div class='val'>Transfer tokens between addresses</div>"
        "<div class='label'>MINT</div><div class='val'>Create additional supply (if baton exists)</div>"
        "<div class='label'>Token ID</div><div class='val'>The GENESIS transaction hash uniquely identifies each token</div>"
        "<div class='label'>Lokad ID</div><div class='val'><code>SLP\\x00</code> (0x534c5000) in OP_RETURN</div>"
        "</div></div>");

    APPEND(off, r, max, EXPLORER_FOOTER);

    /* Cache result */
    if (off > 0 && off < sizeof(g_tokens_cache)) {
        memcpy(g_tokens_cache, r, off);
        g_tokens_cache_len = off;
    }
    free(r);
    sqlite3_close(db);
    g_tokens_computing = 0;
    printf("Tokens background: cached %zu bytes (%" PRId64 " tokens)\n",
           g_tokens_cache_len, token_count);
    fflush(stdout);
    return NULL;
}

size_t serve_tokens(uint8_t *r, size_t max)
{
    if (g_tokens_cache_len > 0) {
        size_t copy = g_tokens_cache_len < max ? g_tokens_cache_len : max;
        memcpy(r, g_tokens_cache, copy);
        return copy;
    }
    if (!g_tokens_computing) {
        g_tokens_computing = 1;
        explorer_start_once(&g_tokens_computing, tokens_compute_thread,
                            "tokens_compute");
    }
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:32px;color:#ff99ff'>Loading Token Data...</h1>"
        "<p style='font-size:18px;color:#888'>Scanning SQLite for ZSLP tokens.</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}

/* ── ZSLP Token Detail Page ────────────────────────────────── */

size_t serve_token_detail(const char *token_id_hex, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    if (!zcl_is_hex_string(token_id_hex, 64) || !ctx->datadir ||
        !explorer_param_is_printable_ascii(token_id_hex))
        return 0;

    /* Open our own SQLite connection (called from HTTPS thread) */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return 0;
    sqlite3_exec(db, "PRAGMA mmap_size=268435456", NULL, NULL, NULL);

    /* Parse hex token ID — try direct first, then reversed byte order */
    uint8_t token_id[32];
    uint8_t token_id_rev[32];
    if (ParseHex(token_id_hex, token_id, 32) != 32) {
        sqlite3_close(db);
        return 0;
    }
    for (int i = 0; i < 32; i++)
        token_id_rev[31 - i] = token_id[i];

    /* Look up token — try both byte orders */
    char ticker[64] = "", name[128] = "", doc_url[256] = "";
    int decimals = 0, genesis_height = 0;
    int64_t total_minted = 0;
    bool found = false;

    /* Try direct byte order first, then reversed */
    const uint8_t *lookup_id = token_id;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT ticker, name, decimals, document_url, genesis_height, total_minted "
                "FROM zslp_tokens WHERE token_id = ?",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, token_id, 32, SQLITE_STATIC);
            if (AR_STEP_ROW_READONLY(s) != SQLITE_ROW) {
                /* Try reversed */
                sqlite3_reset(s);
                sqlite3_bind_blob(s, 1, token_id_rev, 32, SQLITE_STATIC);
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)  
                    lookup_id = token_id_rev;
            }
            if (sqlite3_column_text(s, 0)) {
                const char *t = (const char *)sqlite3_column_text(s, 0);
                const char *n = (const char *)sqlite3_column_text(s, 1);
                const char *u = (const char *)sqlite3_column_text(s, 3);
                if (t) snprintf(ticker, sizeof(ticker), "%s", t);
                if (n) snprintf(name, sizeof(name), "%s", n);
                if (u) snprintf(doc_url, sizeof(doc_url), "%s", u);
                decimals = sqlite3_column_int(s, 2);
                genesis_height = sqlite3_column_int(s, 4);
                total_minted = sqlite3_column_int64(s, 5);
                found = true;
            }
            sqlite3_finalize(s);
        }
    }

    if (!found) {
        sqlite3_close(db);
        return (size_t)snprintf((char *)r, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><link rel='stylesheet' href='/explorer/style.css'></head><body>"
            EXPLORER_NAV "<h2>Token Not Found</h2>"
            "<p>No ZSLP token with ID: <code>%s</code></p>" EXPLORER_FOOTER,
            token_id_hex);
    }

    size_t off = 0;
    char safe_ticker[128], safe_name[256], safe_url[512];
    html_escape(safe_ticker, sizeof(safe_ticker), ticker);
    html_escape(safe_name, sizeof(safe_name), name);
    html_escape(safe_url, sizeof(safe_url), doc_url);

    char supply[64];
    if (decimals > 0 && decimals <= 8) {
        int64_t divisor = 1;
        for (int d = 0; d < decimals; d++) divisor *= 10;
        snprintf(supply, sizeof(supply), "%" PRId64 ".%0*" PRId64,
                 total_minted / divisor, decimals, total_minted % divisor);
    } else {
        snprintf(supply, sizeof(supply), "%" PRId64, total_minted);
    }

    /* Count transfers for this token */
    int64_t xfer_count = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT count(*) FROM zslp_transfers WHERE token_id = ?",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, lookup_id, 32, SQLITE_STATIC);
            if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)  
                xfer_count = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
    }

    APPEND(off, r, max, EXPLORER_HEADER("Token"));
    off += explorer_emit_nav((char *)r + off, max - off, "tokens");

    /* Token header */
    APPEND(off, r, max,
        "<h1 style='color:#ff99ff'>%s</h1>"
        "<h2>%s</h2>"
        "<div class='card'><div class='grid'>"
        "<div class='label'>Token ID</div><div class='val hash' style='font-size:13px'>%s</div>"
        "<div class='label'>Ticker</div><div class='val' style='color:#ff99ff;font-weight:700;font-size:20px'>%s</div>"
        "<div class='label'>Name</div><div class='val'>%s</div>"
        "<div class='label'>Decimals</div><div class='val'>%d</div>"
        "<div class='label'>Total Supply</div><div class='val amount' style='font-size:20px'>%s</div>"
        "<div class='label'>Genesis Block</div><div class='val'><a href='/explorer/block/%d'>%d</a></div>"
        "<div class='label'>Genesis TX</div><div class='val hash'><a href='/explorer/tx/%s'>%s</a></div>"
        "<div class='label'>Transfers</div><div class='val'>%" PRId64 "</div>",
        safe_ticker[0] ? safe_ticker : "(unnamed)",
        safe_name[0] ? safe_name : "ZSLP Token",
        token_id_hex,
        safe_ticker[0] ? safe_ticker : "(none)",
        safe_name, decimals, supply,
        genesis_height, genesis_height,
        token_id_hex, token_id_hex,
        xfer_count);

    if (safe_url[0])
        APPEND(off, r, max,
            "<div class='label'>Document URL</div><div class='val'>%s</div>",
            safe_url);

    APPEND(off, r, max, "</div></div>");

    /* Transfer history */
    APPEND(off, r, max,
        "<h2>Transfer History (%" PRId64 ")</h2>"
        "<table><tr><th>Block</th><th>Type</th><th>Amount</th></tr>",
        xfer_count);
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT block_height, tx_type, amount, hex(txid) "
                "FROM zslp_transfers WHERE token_id = ? "
                "ORDER BY block_height DESC LIMIT 100",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(s, 1, lookup_id, 32, SQLITE_STATIC);
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && off + 512 < max) {
                int height = sqlite3_column_int(s, 0);
                int tx_type = sqlite3_column_int(s, 1);
                int64_t amount = sqlite3_column_int64(s, 2);
                const char *txid_hex = (const char *)sqlite3_column_text(s, 3);

                const char *type_str = tx_type == 1 ? "GENESIS" :
                                       tx_type == 2 ? "MINT" :
                                       tx_type == 3 ? "SEND" : "?";
                const char *type_class = tx_type == 1 ? "tag-cb" :
                                         tx_type == 2 ? "tag-shielded" : "tag-slp";

                char amt[64];
                if (decimals > 0 && decimals <= 8) {
                    int64_t divisor = 1;
                    for (int d = 0; d < decimals; d++) divisor *= 10;
                    snprintf(amt, sizeof(amt), "%" PRId64 ".%0*" PRId64,
                             amount / divisor, decimals, amount % divisor);
                } else {
                    snprintf(amt, sizeof(amt), "%" PRId64, amount);
                }

                /* Reverse txid for display */
                char txid_disp[65] = "";
                if (txid_hex && strlen(txid_hex) == 64) {
                    for (int k = 0; k < 32; k++) {
                        txid_disp[k*2] = txid_hex[62-k*2];
                        txid_disp[k*2+1] = txid_hex[63-k*2];
                    }
                    txid_disp[64] = '\0';
                    for (int k = 0; k < 64; k++)
                        if (txid_disp[k] >= 'A' && txid_disp[k] <= 'F')
                            txid_disp[k] += 32;
                }
                char short_tx[18];
                snprintf(short_tx, sizeof(short_tx), "%.8s...%.4s",
                         txid_disp, txid_disp + 60);

                APPEND(off, r, max,
                    "<tr><td><a href='/explorer/block/%d'>%d</a></td>"
                    "<td><span class='tag %s'>%s</span></td>"
                    "<td class='amount'>%s</td></tr>",
                    height, height, type_class, type_str, amt);
            }
            sqlite3_finalize(s);
        }
    }
    APPEND(off, r, max, "</table>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    sqlite3_close(db);
    return off;
}

size_t serve_hodl(uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    sqlite3 *db = NULL;
    size_t off = 0;

    if (!ctx->datadir || !explorer_open_readonly_db(ctx->datadir, &db)) {
        APPEND(off, r, max, EXPLORER_HEADER("HODL Wave"));
        off += explorer_emit_nav((char *)r + off, max - off, "hodl");
        APPEND(off, r, max,
            "<div style='max-width:900px;margin:40px auto;color:#ccc'>"
            "<h1>HODL Wave</h1>"
            "<p>Database unavailable. This page will not publish cached HODL data.</p>"
            "</div>" EXPLORER_FOOTER);
        return off;
    }

    /* Canonical tip = blocks.max. utxos is written by connect_tip and
     * lags blocks (briefly during a connect, indefinitely if the
     * indexer is mid-rebuild). Using MAX(blocks, utxos) as we did
     * before could let utxos.height lead blocks.height during catchup,
     * which makes hodl_wave_age_seconds compute negative ages that the
     * silent clamp turns into 0 — visually all UTXOs land in <1d. */
    int64_t tip = sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
    int64_t utxo_tip = sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    if (utxo_tip > tip) {
        /* Anomaly: utxos table ahead of blocks. Don't let that drive
         * the headline — fall back to utxo_tip so age math stays sane,
         * but flag in skipped_rows on the next scan. */
        tip = utxo_tip;
    }

    struct hodl_wave_snapshot hodl;
    bool ok = hodl_wave_scan_current_utxos(db, tip, &hodl);
    sqlite3_close(db);

    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>HODL Wave</title>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>");
    off += explorer_emit_nav((char *)r + off, max - off, "hodl");

    if (!ok) {
        APPEND(off, r, max,
            "<div style='max-width:900px;margin:40px auto;color:#ccc'>"
            "<h1>HODL Wave</h1>"
            "<p>Degraded: %s.</p>"
            "</div>" EXPLORER_FOOTER,
            hodl.status);
        return off;
    }

    double older_pct = hodl_wave_older_than_1y_percent(&hodl);
    char total_fmt[64], older_fmt[64];
    zcl_format_zcl(total_fmt, sizeof(total_fmt), hodl.total_value);
    zcl_format_zcl(older_fmt, sizeof(older_fmt), hodl.older_than_1y_value);

    APPEND(off, r, max,
        "<div style='text-align:center;margin:30px 0 10px'>"
        "<h1 style='font-size:42px;color:#fff;font-weight:800;margin:0;"
        "font-family:Georgia,\"Times New Roman\",serif'>"
        "%.3f%% of Current Transparent UTXO Value Is Older Than 1 Year</h1>"
        "<p style='font-size:19px;color:#888;margin:8px 0 0;"
        "font-family:Georgia,serif'>Current UTXO age distribution at block %" PRId64 "</p>"
        "</div>",
        older_pct, hodl.tip_height);

    /* ── Time-series chart: % held > 1y over time ──────────────── */
    {
        sqlite3 *hist_db = NULL;
        if (explorer_open_readonly_db(ctx->datadir, &hist_db)) {
            static struct hodl_history_row rows_raw[2048];
            int n_raw = hodl_history_load_all(hist_db, rows_raw,
                (int)(sizeof(rows_raw)/sizeof(rows_raw[0])));

            /* Correctness gate. The hodl_history filler queries
             * tx_outputs LEFT JOIN tx_inputs to reconstruct historical
             * UTXO snapshots. If those tables aren't fully populated
             * for a given height, the SQL returns either (0, 0) — a
             * trivially-zero sample — or a numerically wrong pair
             * computed against a partial source. Either way the
             * resulting row is not safe to show.
             *
             * Until we have a way to certify each sample's source
             * coverage at fill time, treat any partial state as
             * insufficient: skip the time-series entirely and rely
             * on the current-snapshot bar chart + headline below. */
            int64_t tx_out_count = sql_query_i64(hist_db,
                "SELECT count(*) FROM tx_outputs");
            int64_t tx_count = sql_query_i64(hist_db,
                "SELECT count(*) FROM transactions");
            sqlite3_close(hist_db);

            /* Conservative threshold: require tx_outputs to have at
             * least one row per transaction we know about (typical
             * txs have 2+ outputs; this is a generous lower bound). */
            bool source_complete = (tx_count > 0) &&
                                   (tx_out_count >= tx_count);

            /* Filter out (0,0) sentinel rows from old partial fills. */
            static struct hodl_history_row rows[2048];
            int n = 0;
            if (source_complete) {
                for (int i = 0; i < n_raw; i++) {
                    if (rows_raw[i].total_zat > 0)
                        rows[n++] = rows_raw[i];
                }
            }

            if (!source_complete || n < 2) {
                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto;"
                    "padding:16px;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;color:#888'>"
                    "<h2 style='color:#bbb;margin-top:0'>"
                    "%% held &gt; 1 year over time</h2>"
                    "<p>Historical snapshots need the per-output index "
                    "(<code>tx_outputs</code>) to be fully populated. "
                    "Coverage so far: %" PRId64 " output rows for "
                    "%" PRId64 " indexed transactions. Once the backfill "
                    "completes, this card switches to a daily-sample "
                    "time-series with mouse-hover detail.</p>"
                    "</div>",
                    tx_out_count, tx_count);
            } else {
                /* Compute min/max for y-axis scaling. Use [floor..100]
                 * with a 5%% headroom floor to keep the curve readable
                 * even when held>1y stays in a tight band. */
                double y_min = rows[0].older_1y_pct, y_max = rows[0].older_1y_pct;
                for (int i = 1; i < n; i++) {
                    if (rows[i].older_1y_pct < y_min) y_min = rows[i].older_1y_pct;
                    if (rows[i].older_1y_pct > y_max) y_max = rows[i].older_1y_pct;
                }
                y_min -= 2.0; y_max += 2.0;
                if (y_min < 0) y_min = 0;
                if (y_max > 100) y_max = 100;
                if (y_max - y_min < 5) { y_min = y_min > 5 ? y_min - 5 : 0; y_max = y_min + 10; }

                int W = 1000, H = 380;
                int pl = 70, pr = 25, pt = 50, pb = 60;
                int pw = W - pl - pr, ph = H - pt - pb;
                int64_t t_min = rows[0].time, t_max = rows[n-1].time;
                if (t_max <= t_min) t_max = t_min + 1;

                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto'>"
                    "<svg id='hodl-ts' viewBox='0 0 %d %d' style='width:100%%;"
                    "height:auto;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;display:block'>"
                    "<text x='30' y='30' fill='#bbb' font-size='18' "
                    "font-family='Georgia,serif'>%% of transparent supply held &gt; 1 year</text>"
                    "<text x='%d' y='30' fill='#666' font-size='12' "
                    "text-anchor='end' font-family='Georgia,serif'>"
                    "%d samples · daily</text>",
                    W, H, W - pr, n);

                /* Y-axis gridlines + labels */
                for (int g = 0; g <= 4; g++) {
                    double yv = y_min + (y_max - y_min) * g / 4.0;
                    int y = pt + ph - (int)((yv - y_min) / (y_max - y_min) * ph);
                    APPEND(off, r, max,
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#1a1a1a'/>"
                        "<text x='%d' y='%d' fill='#777' font-size='12' "
                        "text-anchor='end'>%.1f%%</text>",
                        pl, y, pl + pw, y, pl - 8, y + 4, yv);
                }

                /* X-axis date labels: 5 evenly spaced ticks */
                for (int g = 0; g <= 4; g++) {
                    int64_t t = t_min + (t_max - t_min) * g / 4;
                    int x = pl + pw * g / 4;
                    time_t tt = (time_t)t;
                    struct tm tm_;
                    gmtime_r(&tt, &tm_);
                    char dbuf[16];
                    strftime(dbuf, sizeof(dbuf), "%Y-%m", &tm_);
                    APPEND(off, r, max,
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#1a1a1a'/>"
                        "<text x='%d' y='%d' fill='#777' font-size='12' "
                        "text-anchor='middle' font-family='Georgia,serif'>%s</text>",
                        x, pt, x, pt + ph, x, pt + ph + 18, dbuf);
                }

                /* Polyline through points */
                APPEND(off, r, max,
                    "<polyline fill='none' stroke='#33ff99' "
                    "stroke-width='2' points='");
                for (int i = 0; i < n; i++) {
                    int x = pl + (int)((double)(rows[i].time - t_min) /
                                       (double)(t_max - t_min) * pw);
                    int y = pt + ph - (int)((rows[i].older_1y_pct - y_min) /
                                            (y_max - y_min) * ph);
                    APPEND(off, r, max, "%s%d,%d", i ? " " : "", x, y);
                }
                APPEND(off, r, max, "'/>");

                /* Hover crosshair + tooltip (hidden until JS shows it) */
                APPEND(off, r, max,
                    "<line id='hodl-xhair' x1='0' y1='%d' x2='0' y2='%d' "
                    "stroke='#33ff99' stroke-dasharray='2,3' stroke-width='1' "
                    "style='display:none'/>"
                    "<circle id='hodl-dot' cx='0' cy='0' r='4' "
                    "fill='#33ff99' style='display:none'/>"
                    "<g id='hodl-tip' style='display:none'>"
                    "<rect id='hodl-tip-bg' x='0' y='0' width='220' "
                    "height='86' rx='6' fill='#000' stroke='#33ff99' "
                    "opacity='0.95'/>"
                    "<text id='hodl-tip-date' x='10' y='20' fill='#fff' "
                    "font-size='13' font-family='Georgia,serif'>—</text>"
                    "<text id='hodl-tip-pct' x='10' y='40' fill='#33ff99' "
                    "font-size='15' font-weight='600'>—</text>"
                    "<text id='hodl-tip-amt' x='10' y='60' fill='#bbb' "
                    "font-size='12'>—</text>"
                    "<text id='hodl-tip-h' x='10' y='78' fill='#666' "
                    "font-size='11'>—</text>"
                    "</g>",
                    pt, pt + ph);

                /* Inline data block for JS — one row per sample, packed
                 * as comma-separated integers (height,time,total_zat,
                 * older_zat,pct_x1000) to keep the inline blob compact. */
                APPEND(off, r, max,
                    "<script>(function(){"
                    "var data=[");
                for (int i = 0; i < n; i++) {
                    APPEND(off, r, max,
                        "%s[%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d]",
                        i ? "," : "",
                        rows[i].height, rows[i].time,
                        rows[i].total_zat, rows[i].older_1y_zat,
                        (int)(rows[i].older_1y_pct * 1000.0));
                }
                APPEND(off, r, max,
                    "];"
                    "var W=%d,pl=%d,pr=%d,pt=%d,pb=%d,pw=W-pl-pr;"
                    "var ymin=%.3f,ymax=%.3f,tmin=%" PRId64 ",tmax=%" PRId64 ";"
                    "var svg=document.getElementById('hodl-ts');"
                    "var xhair=document.getElementById('hodl-xhair');"
                    "var dot=document.getElementById('hodl-dot');"
                    "var tip=document.getElementById('hodl-tip');"
                    "var tipBg=document.getElementById('hodl-tip-bg');"
                    "var td=document.getElementById('hodl-tip-date');"
                    "var tp=document.getElementById('hodl-tip-pct');"
                    "var ta=document.getElementById('hodl-tip-amt');"
                    "var th=document.getElementById('hodl-tip-h');"
                    "function fmtZcl(z){"
                    "var n=z/1e8;"
                    "if(n>=1e6)return(n/1e6).toFixed(2)+'M';"
                    "if(n>=1e3)return(n/1e3).toFixed(2)+'k';"
                    "return n.toFixed(2);"
                    "}"
                    "function fmtDate(t){"
                    "var d=new Date(t*1000);"
                    "return d.toISOString().slice(0,10);"
                    "}"
                    "function hide(){"
                    "xhair.style.display='none';"
                    "dot.style.display='none';"
                    "tip.style.display='none';"
                    "}"
                    "function pickNearest(svgX){"
                    "var tfrac=(svgX-pl)/pw;"
                    "var target=tmin+tfrac*(tmax-tmin);"
                    "var lo=0,hi=data.length-1;"
                    "while(lo<hi){var m=(lo+hi)>>1;"
                    "if(data[m][1]<target)lo=m+1;else hi=m;}"
                    "if(lo>0&&Math.abs(data[lo-1][1]-target)<"
                    "Math.abs(data[lo][1]-target))lo--;"
                    "return lo;"
                    "}"
                    "function show(svgX){"
                    "var i=pickNearest(svgX);"
                    "var row=data[i];"
                    "var x=pl+(row[1]-tmin)/(tmax-tmin)*pw;"
                    "var pct=row[4]/1000;"
                    "var y=pt+(%d)-(pct-ymin)/(ymax-ymin)*(%d);"
                    "xhair.setAttribute('x1',x);"
                    "xhair.setAttribute('x2',x);"
                    "xhair.style.display='';"
                    "dot.setAttribute('cx',x);"
                    "dot.setAttribute('cy',y);"
                    "dot.style.display='';"
                    "var tx=x+12;"
                    "if(tx+220>W-pr)tx=x-232;"
                    "var ty=y-50;"
                    "if(ty<pt+5)ty=pt+5;"
                    "tip.setAttribute('transform','translate('+tx+','+ty+')');"
                    "tip.style.display='';"
                    "td.textContent=fmtDate(row[1]);"
                    "tp.textContent=pct.toFixed(3)+'%% held > 1 year';"
                    "ta.textContent=fmtZcl(row[3])+' / '+fmtZcl(row[2])+' ZCL';"
                    "th.textContent='Block '+row[0];"
                    "}"
                    "function pt2svg(e){"
                    "var r=svg.getBoundingClientRect();"
                    "return (e.clientX-r.left)*(W/r.width);"
                    "}"
                    "svg.addEventListener('mousemove',function(e){"
                    "var sx=pt2svg(e);"
                    "if(sx<pl||sx>W-pr){hide();return;}"
                    "show(sx);"
                    "});"
                    "svg.addEventListener('mouseleave',hide);"
                    "svg.addEventListener('touchmove',function(e){"
                    "if(!e.touches[0])return;"
                    "var sx=pt2svg(e.touches[0]);"
                    "if(sx>=pl&&sx<=W-pr)show(sx);"
                    "e.preventDefault();"
                    "},{passive:false});"
                    "})();</script>"
                    "</svg></div>",
                    W, pl, pr, pt, pb, y_min, y_max, t_min, t_max,
                    ph, ph);
            }
        }
    }

    APPEND(off, r, max,
        "<div style='max-width:1000px;margin:20px auto'>"
        "<svg viewBox='0 0 1000 360' style='width:100%%;height:auto;"
        "background:#0c0c0c;border:1px solid #1a1a1a;border-radius:8px;"
        "display:block'>"
        "<text x='30' y='35' fill='#bbb' font-size='18' "
        "font-family='Georgia,serif'>Unspent transparent value by age</text>");

    int x0 = 70, y0 = 285, chart_w = 860, chart_h = 220;
    for (int g = 0; g <= 4; g++) {
        int y = y0 - chart_h * g / 4;
        APPEND(off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1a1a1a'/>"
            "<text x='%d' y='%d' fill='#777' font-size='12' "
            "text-anchor='end'>%d%%</text>",
            x0, y, x0 + chart_w, y, x0 - 8, y + 4, g * 25);
    }

    int bar_gap = 10;
    int bar_w = (chart_w - bar_gap * (HODL_WAVE_BUCKETS - 1)) /
                HODL_WAVE_BUCKETS;
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[b].value / (double)hodl.total_value * 100.0 : 0.0;
        int bh = (int)(pct / 100.0 * chart_h);
        int x = x0 + b * (bar_w + bar_gap);
        int y = y0 - bh;
        APPEND(off, r, max,
            "<rect x='%d' y='%d' width='%d' height='%d' fill='%s' rx='3'>"
            "<title>%s: %.3f%%, %" PRId64 " UTXOs</title></rect>"
            "<text x='%d' y='%d' fill='#aaa' font-size='11' "
            "text-anchor='middle' transform='rotate(-35,%d,%d)'>%s</text>"
            "<text x='%d' y='%d' fill='#eee' font-size='12' "
            "text-anchor='middle'>%.2f%%</text>",
            x, y, bar_w, bh > 1 ? bh : 1, hodl.buckets[b].color,
            hodl.buckets[b].html_label, pct, hodl.buckets[b].count,
            x + bar_w / 2, y0 + 26, x + bar_w / 2, y0 + 26,
            hodl.buckets[b].html_label,
            x + bar_w / 2, y - 6, pct);
    }
    APPEND(off, r, max,
        "<text x='970' y='345' fill='#444' font-size='11' "
        "font-family='Georgia,serif' text-anchor='end'>"
        "Source: current transparent UTXO set</text></svg>");

    APPEND(off, r, max,
        "<div class='stats-row' style='margin-top:18px'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Current transparent UTXO value</div></div>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Older than 1 year</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>UTXOs counted</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Rows skipped</div></div>"
        "</div>",
        total_fmt, older_fmt, hodl.total_count, hodl.skipped_rows);

    APPEND(off, r, max,
        "<table class='txlist' style='max-width:1000px;margin:18px auto'>"
        "<tr><th>Age</th><th>UTXOs</th><th>Value</th><th>Share</th></tr>");
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        char val_fmt[64];
        zcl_format_zcl(val_fmt, sizeof(val_fmt), hodl.buckets[b].value);
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[b].value / (double)hodl.total_value * 100.0 : 0.0;
        APPEND(off, r, max,
            "<tr><td><span style='display:inline-block;width:11px;height:11px;"
            "background:%s;border-radius:2px;margin-right:8px'></span>%s</td>"
            "<td>%" PRId64 "</td><td>%s ZCL</td><td>%.3f%%</td></tr>",
            hodl.buckets[b].color, hodl.buckets[b].html_label,
            hodl.buckets[b].count, val_fmt, pct);
    }
    APPEND(off, r, max, "</table>");
    APPEND(off, r, max,
        "<p style='max-width:900px;margin:18px auto;color:#888;"
        "font-family:Georgia,serif;font-size:16px;line-height:1.7'>"
        "Source: current transparent UTXO set. Metric: UTXO age distribution."
        "</p></div>" EXPLORER_FOOTER);
    return off;
}

/* ── CSS Stylesheet ───────────────────────────────────────── */

size_t serve_css(uint8_t *r, size_t max)
{
    struct explorer_assets *assets = explorer_assets();
    /* Reload CSS from disk each time (allows live editing) */
    load_css();
    size_t off = 0;
    int n = snprintf((char *)r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/css; charset=utf-8\r\n"
        "Cache-Control: public, max-age=60\r\n"
        "Connection: close\r\n\r\n");
    if (n > 0) off = (size_t)n;
    if (off + assets->css_len < max) {
        memcpy(r + off, assets->css_cache, assets->css_len);
        off += assets->css_len;
    }
    return off;
}

/* ── Event Log Page ───────────────────────────────────────── */

size_t serve_events(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("Event Log — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, "events");

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>Event Log</h1>"
        "<p style='color:#888'>Live node events from the ring buffer. "
        "Auto-refreshes every 3 seconds.</p>"
        "<div style='margin:10px 0'>"
        "<label style='color:#aaa'>Show: </label>"
        "<select id='ev-count' style='background:#1a1a2e;color:#eee;border:1px solid #333;"
        "padding:4px 8px;border-radius:4px'>"
        "<option value='50'>50</option>"
        "<option value='100' selected>100</option>"
        "<option value='500'>500</option>"
        "<option value='2000'>2000</option>"
        "</select>"
        "<label style='color:#aaa;margin-left:16px'>Filter: </label>"
        "<input id='ev-filter' placeholder='type, peer, or data...' "
        "style='background:#1a1a2e;color:#eee;border:1px solid #333;"
        "padding:4px 8px;border-radius:4px;width:200px'>"
        "<span id='ev-status' style='color:#555;margin-left:16px;font-size:13px'>"
        "loading...</span>"
        "</div>"
        "<table class='block-table' style='font-size:13px'>"
        "<thead><tr>"
        "<th style='width:60px'>Seq</th>"
        "<th style='width:170px'>Time</th>"
        "<th style='width:180px'>Type</th>"
        "<th style='width:60px'>Peer</th>"
        "<th>Data</th>"
        "</tr></thead>"
        "<tbody id='ev-body'></tbody></table></div>");

    APPEND(off, response, max,
        "<script>"
        "const tbody=document.getElementById('ev-body'),"
        "sel=document.getElementById('ev-count'),"
        "flt=document.getElementById('ev-filter'),"
        "sts=document.getElementById('ev-status');"
        "function fmt(ts){"
        "const d=new Date(ts/1000);"
        "return d.toISOString().replace('T',' ').replace('Z','')}"
        "function cls(t){"
        "if(t.startsWith('val.'))return'color:#ff6b6b';"
        "if(t.startsWith('sync.'))return'color:#ffd93d';"
        "if(t.startsWith('peer.'))return'color:#6bcb77';"
        "if(t.startsWith('tcp.'))return'color:#4d96ff';"
        "if(t.startsWith('snap.'))return'color:#ff922b';"
        "if(t.startsWith('chain.'))return'color:#cc5de8';"
        "if(t.startsWith('tx.'))return'color:#66d9e8';"
        "if(t.startsWith('sys.'))return'color:#ff8787';"
        "return'color:#aaa'}"
        "function esc(s){const d=document.createElement('div');"
        "d.textContent=s;return d.innerHTML}"
        "async function refresh(){"
        "try{"
        "const r=await fetch('/api/events?count='+sel.value);"
        "const evs=await r.json();"
        "const f=flt.value.toLowerCase();"
        "let html='';"
        "for(let i=evs.length-1;i>=0;i--){"
        "const e=evs[i];"
        "if(f&&!(e.type+' '+e.peer+' '+e.data).toLowerCase().includes(f))continue;"
        "html+='<tr><td>'+e.seq+'</td>"
        "<td>'+fmt(e.ts)+'</td>"
        "<td style=\"'+cls(e.type)+'\">'+esc(e.type)+'</td>"
        "<td>'+(e.peer||'')+'</td>"
        "<td style=\"font-family:monospace;font-size:12px;word-break:break-all\">"
        "'+esc(e.data)+'</td></tr>'}"
        "tbody.innerHTML=html;"
        "sts.textContent=evs.length+' events ('+new Date().toLocaleTimeString()+')';"
        "}catch(e){sts.textContent='Error: '+e.message}}"
        "refresh();"
        "setInterval(refresh,3000);"
        "sel.onchange=refresh;"
        "flt.oninput=refresh;"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}

/* ── Names Page ──────────────────────────────────────────── */

size_t serve_names(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("ZCL Names — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, "names");

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>ZCL Names</h1>"
        "<p style='color:#888'>On-chain name registry (ZNAM protocol). "
        "Names map to .onion addresses, z-addresses, t-addresses, "
        "and multi-coin records.</p>"
        "<div style='margin:12px 0;display:flex;gap:8px;align-items:center'>"
        "<input id='name-search' placeholder='Resolve a name...' "
        "style='background:#1a1a2e;color:#eee;border:1px solid #333;"
        "padding:6px 12px;border-radius:4px;width:200px;font-size:14px'>"
        "<button onclick='resolve()' style='background:#33ff99;color:#000;"
        "border:none;padding:6px 16px;border-radius:4px;font-weight:600;"
        "cursor:pointer'>Resolve</button>"
        "<span id='resolve-result' style='color:#888;font-size:13px'></span>"
        "</div>"
        "<table class='block-table'>"
        "<thead><tr>"
        "<th>Name</th>"
        "<th>Type</th>"
        "<th>Target</th>"
        "<th>Owner</th>"
        "<th>Height</th>"
        "</tr></thead>"
        "<tbody id='names-body'><tr><td colspan='5' style='color:#555'>"
        "Loading...</td></tr></tbody></table></div>"
        "<script>"
        "async function load(){"
        "try{"
        "const r=await fetch('/api/names');"
        "const names=await r.json();"
        "const tb=document.getElementById('names-body');"
        "if(!names.length){tb.innerHTML='<tr><td colspan=5 style=\"color:#555\">"
        "No names registered yet</td></tr>';return}"
        "let h='';"
        "for(const n of names){"
        "h+='<tr><td style=\"color:#33ff99;font-weight:600\">'+n.name+'</td>"
        "<td>'+n.type+'</td>"
        "<td style=\"font-family:monospace;font-size:12px;word-break:break-all\">"
        "'+n.value+'</td>"
        "<td style=\"font-family:monospace;font-size:11px\">'+n.owner.slice(0,16)+'...</td>"
        "<td>'+n.reg_height+'</td></tr>'}"
        "tb.innerHTML=h"
        "}catch(e){document.getElementById('names-body').innerHTML="
        "'<tr><td colspan=5 style=\"color:#f66\">Error: '+e.message+'</td></tr>'}}"
        "load();"
        "async function resolve(){"
        "const n=document.getElementById('name-search').value.trim();"
        "const rs=document.getElementById('resolve-result');"
        "if(!n){rs.textContent='Enter a name';return}"
        "try{"
        "const r=await fetch('/api/name/'+encodeURIComponent(n));"
        "if(!r.ok){rs.innerHTML='<span style=\"color:#f66\">Not found</span>';return}"
        "const d=await r.json();"
        "rs.innerHTML='<span style=\"color:#33ff99\">'+d.name+'</span> &rarr; "
        "<span style=\"font-family:monospace;font-size:12px\">'+d.value+'</span> "
        "('+d.type+')';"
        "}catch(e){rs.textContent='Error: '+e.message}}"
        "document.getElementById('name-search').addEventListener('keyup',"
        "function(e){if(e.key==='Enter')resolve()});"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}

/* ── Market Page ─────────────────────────────────────────── */

size_t serve_market(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("ZCL Market — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, "market");

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>ZCL Market</h1>"
        "<p style='color:#888'>Decentralized file marketplace. "
        "Seeders announce files, downloaders pay in shielded ZCL per chunk.</p>"
        "<table class='block-table'>"
        "<thead><tr>"
        "<th>Filename</th>"
        "<th>Size</th>"
        "<th>Price/MB</th>"
        "<th>Chunks</th>"
        "<th>Last Seen</th>"
        "</tr></thead>"
        "<tbody id='market-body'><tr><td colspan='5' style='color:#555'>"
        "Loading...</td></tr></tbody></table></div>"
        "<script>"
        "async function load(){"
        "try{"
        "const r=await fetch('/api/market');"
        "const files=await r.json();"
        "const tb=document.getElementById('market-body');"
        "if(!files.length){tb.innerHTML='<tr><td colspan=5 style=\"color:#555\">"
        "No files available</td></tr>';return}"
        "let h='';"
        "for(const f of files){"
        "const sz=f.size_mb?f.size_mb.toFixed(1)+' MB':Math.round(f.size_bytes/1024)+' KB';"
        "const pr=f.price_per_mb_zcl?f.price_per_mb_zcl.toFixed(4)+' ZCL':'free';"
        "const t=f.last_seen?new Date(f.last_seen*1000).toLocaleString():'—';"
        "h+='<tr><td style=\"color:#33ff99\">'+f.filename+'</td>"
        "<td>'+sz+'</td><td>'+pr+'</td>"
        "<td>'+f.num_chunks+'</td><td>'+t+'</td></tr>'}"
        "tb.innerHTML=h"
        "}catch(e){document.getElementById('market-body').innerHTML="
        "'<tr><td colspan=5 style=\"color:#f66\">Error: '+e.message+'</td></tr>'}}"
        "load();setInterval(load,10000)"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}

/* ── Swaps Page ──────────────────────────────────────────── */

size_t serve_swaps(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("Atomic Swaps — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, "swaps");

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>Atomic Swaps</h1>"
        "<p style='color:#888'>HTLC cross-chain contracts (dcrdex-compatible). "
        "Supports ZCL, BTC, LTC, DOGE.</p>"
        "<div style='margin:10px 0'>"
        "<span id='chains' style='color:#555'>Loading chains...</span></div>"
        "<table class='block-table'>"
        "<thead><tr>"
        "<th>Swap ID</th>"
        "<th>Chain</th>"
        "<th>Role</th>"
        "<th>State</th>"
        "<th>Amount</th>"
        "<th>Locktime</th>"
        "<th>P2SH Address</th>"
        "</tr></thead>"
        "<tbody id='swaps-body'><tr><td colspan='7' style='color:#555'>"
        "Loading...</td></tr></tbody></table></div>"
        "<script>"
        "async function load(){"
        "try{"
        "const[sr,cr]=await Promise.all(["
        "fetch('/api/swaps'),fetch('/api/swap_chains')]);"
        "const swaps=await sr.json();"
        "const chains=await cr.json();"
        "document.getElementById('chains').innerHTML="
        "'Supported: '+chains.map(c=>"
        "'<span style=\"color:#33ff99;margin-right:8px\">'+c.ticker+'</span>').join('');"
        "const tb=document.getElementById('swaps-body');"
        "if(!swaps.length){tb.innerHTML='<tr><td colspan=7 style=\"color:#555\">"
        "No swaps yet</td></tr>';return}"
        "let h='';"
        "for(const s of swaps){"
        "const st=s.state==='pending'?'color:#ffd93d':s.state==='funded'?'color:#6bcb77':"
        "s.state==='redeemed'?'color:#33ff99':'color:#888';"
        "h+='<tr><td style=\"font-family:monospace;font-size:11px\">"
        "'+s.swap_id.slice(0,12)+'...</td>"
        "<td style=\"font-weight:600\">'+s.chain+'</td>"
        "<td>'+s.role+'</td>"
        "<td style=\"'+st+'\">'+s.state+'</td>"
        "<td>'+s.amount+' '+s.chain+'</td>"
        "<td>'+s.locktime+' blocks</td>"
        "<td style=\"font-family:monospace;font-size:11px\">"
        "'+s.p2sh_address.slice(0,16)+'...</td></tr>'}"
        "tb.innerHTML=h"
        "}catch(e){document.getElementById('swaps-body').innerHTML="
        "'<tr><td colspan=7 style=\"color:#f66\">Error: '+e.message+'</td></tr>'}}"
        "load()"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}

/* ── Messages Page ───────────────────────────────────────── */

size_t serve_messages(uint8_t *r, size_t max)
{
    size_t off = 0;
    char *response = (char *)r;

    APPEND(off, response, max, EXPLORER_HEADER("Messages — ZClassic23"));
    off += explorer_emit_nav(response + off, max - off, NULL);

    APPEND(off, response, max,
        "<div class='content'>"
        "<h1>Messages</h1>"
        "<p style='color:#888'>P2P encrypted messaging (ZMSG protocol). "
        "Send messages to peers by ID or by ZCL Name.</p>"
        "<table class='block-table'>"
        "<thead><tr>"
        "<th>Direction</th>"
        "<th>Channel</th>"
        "<th>From/To</th>"
        "<th>Message</th>"
        "<th>Time</th>"
        "</tr></thead>"
        "<tbody id='msg-body'><tr><td colspan='5' style='color:#555'>"
        "Loading...</td></tr></tbody></table></div>"
        "<script>"
        "async function load(){"
        "try{"
        "const r=await fetch('/api/messages');"
        "const msgs=await r.json();"
        "const tb=document.getElementById('msg-body');"
        "if(!msgs.length){tb.innerHTML='<tr><td colspan=5 style=\"color:#555\">"
        "No messages yet</td></tr>';return}"
        "let h='';"
        "for(const m of msgs){"
        "const dir=m.direction==='outbound'?"
        "'<span style=\"color:#4d96ff\">&#x2191; sent</span>':"
        "'<span style=\"color:#6bcb77\">&#x2193; received</span>';"
        "const who=m.direction==='outbound'?m.recipient:m.sender;"
        "const body=m.body.length>80?m.body.slice(0,80)+'...':m.body;"
        "const t=new Date(m.timestamp*1000).toLocaleString();"
        "h+='<tr><td>'+dir+'</td>"
        "<td>'+m.channel+'</td>"
        "<td style=\"font-size:12px\">'+who+'</td>"
        "<td>'+body+'</td>"
        "<td style=\"font-size:12px;color:#888\">'+t+'</td></tr>'}"
        "tb.innerHTML=h"
        "}catch(e){document.getElementById('msg-body').innerHTML="
        "'<tr><td colspan=5 style=\"color:#f66\">Error: '+e.message+'</td></tr>'}}"
        "load();setInterval(load,5000)"
        "</script>");

    APPEND(off, response, max, EXPLORER_FOOTER);
    return off;
}
