/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet view controller — MVC HTML views for GTK browser.
 * Reads directly from SQLite. No RPC. No ports. */

#include "controllers/wallet_view_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sqlite3.h>

static const char *g_datadir = NULL;

/* ── Shared CSS ───────────────────────────────────────────── */

#define WALLET_CSS \
    "body{font-family:-apple-system,'Segoe UI',Roboto,monospace;" \
    "background:#0c0c0c;color:#e8e8e8;max-width:960px;margin:0 auto;" \
    "padding:16px 20px;font-size:16px;line-height:1.5}" \
    "*{box-sizing:border-box}" \
    "a{color:#4db8ff;text-decoration:none}" \
    "a:hover{color:#80ccff;text-decoration:underline}" \
    "h1{color:#33ff99;font-size:28px;margin:0 0 4px;font-weight:800}" \
    "h2{color:#33ff99;font-size:20px;border-bottom:1px solid #222;" \
    "padding-bottom:6px;margin:24px 0 12px}" \
    ".subtitle{color:#666;font-size:13px;margin:0 0 16px}" \
    ".stats{display:flex;gap:10px;margin:12px 0;flex-wrap:wrap}" \
    ".stat{flex:1;min-width:160px;background:#141414;padding:14px;" \
    "border-radius:8px;text-align:center;border:1px solid #1e1e1e}" \
    ".stat .n{font-size:28px;color:#33ff99;font-weight:800;line-height:1.2}" \
    ".stat .l{font-size:11px;color:#888;text-transform:uppercase;" \
    "letter-spacing:1px;margin-top:2px}" \
    ".nav{display:flex;gap:8px;margin:14px 0;flex-wrap:wrap}" \
    ".nav a{background:#141414;padding:8px 16px;border-radius:6px;" \
    "border:1px solid #1e1e1e;font-size:14px;font-weight:600}" \
    ".nav a:hover{border-color:#33ff99;color:#33ff99}" \
    ".nav a.active{border-color:#33ff99;color:#33ff99;background:#0a1f0a}" \
    ".card{background:#141414;padding:14px 18px;border-radius:8px;" \
    "margin:8px 0;border:1px solid #1e1e1e;border-left:3px solid #33ff99}" \
    ".card .label{color:#888;font-size:12px}" \
    ".card .value{font-size:22px;color:#33ff99;font-weight:700}" \
    ".card .sub{color:#666;font-size:12px;margin-top:2px}" \
    "table{width:100%%;border-collapse:collapse;font-size:14px}" \
    "th{text-align:left;color:#888;padding:8px;border-bottom:1px solid #222;" \
    "font-size:12px;text-transform:uppercase;letter-spacing:0.5px}" \
    "td{padding:8px;border-bottom:1px solid #1a1a1a}" \
    "tr:hover{background:#161616}" \
    ".mono{font-family:'SF Mono','Fira Code',monospace;font-size:13px}" \
    ".hash{color:#4db8ff;font-family:'SF Mono',monospace;font-size:13px}" \
    ".zcl{color:#33ff99;font-weight:700}" \
    ".addr-box{background:#0a0a0a;padding:12px;border-radius:6px;" \
    "font-family:monospace;font-size:14px;color:#4db8ff;" \
    "word-break:break-all;text-align:center;margin:10px 0;" \
    "border:1px solid #222;user-select:all}" \
    "input,select{background:#1a1a1a;color:#e8e8e8;border:1px solid #333;" \
    "padding:10px 14px;font-family:inherit;font-size:15px;width:100%%;" \
    "border-radius:6px;margin:4px 0}" \
    "input:focus{border-color:#33ff99;outline:none}" \
    "button{background:#33ff99;color:#0c0c0c;border:none;padding:12px 24px;" \
    "font-size:16px;font-weight:700;border-radius:6px;cursor:pointer;" \
    "font-family:inherit;width:100%%}" \
    "button:hover{background:#44ffaa}" \
    ".pill{display:inline-block;padding:2px 8px;border-radius:10px;" \
    "font-size:11px;font-weight:700}" \
    ".pill-t{background:#1a2a1a;color:#33ff99}" \
    ".pill-z{background:#1a1a2a;color:#9999ff}" \
    "footer{text-align:center;color:#333;font-size:11px;margin-top:32px}"

#define WALLET_NAV(active) \
    "<div class='nav'>" \
    "<a href='/wallet'" active##_DASH ">Dashboard</a>" \
    "<a href='/wallet/send'" active##_SEND ">Send</a>" \
    "<a href='/wallet/receive'" active##_RECV ">Receive</a>" \
    "<a href='/wallet/history'" active##_HIST ">History</a>" \
    "<a href='/wallet/coins'" active##_COIN ">Coins</a>" \
    "<a href='/explorer'>Explorer</a>" \
    "<a href='/store'>Store</a>" \
    "</div>"

/* Active state macros */
#define DASH_DASH " class='active'"
#define DASH_SEND ""
#define DASH_RECV ""
#define DASH_HIST ""
#define DASH_COIN ""
#define SEND_DASH ""
#define SEND_SEND " class='active'"
#define SEND_RECV ""
#define SEND_HIST ""
#define SEND_COIN ""
#define RECV_DASH ""
#define RECV_SEND ""
#define RECV_RECV " class='active'"
#define RECV_HIST ""
#define RECV_COIN ""
#define HIST_DASH ""
#define HIST_SEND ""
#define HIST_RECV ""
#define HIST_HIST " class='active'"
#define HIST_COIN ""
#define COIN_DASH ""
#define COIN_SEND ""
#define COIN_RECV ""
#define COIN_HIST ""
#define COIN_COIN " class='active'"

#define HEADER(title) \
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n" \
    "Connection: close\r\n\r\n" \
    "<!DOCTYPE html><html><head><meta charset='utf-8'>" \
    "<title>" title " — ZClassic23</title>" \
    "<style>" WALLET_CSS "</style></head><body>" \
    "<h1>ZClassic23</h1><p class='subtitle'>Direct SQLite — no ports</p>"

#define FOOTER "</body></html>"

/* ── Helper: open DB ──────────────────────────────────────── */

static sqlite3 *open_db(void) {
    if (!g_datadir) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/node.db", g_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_busy_timeout(db, 3000);
    return db;
}

static int query_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) val = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

static int64_t query_int64(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int64_t val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) val = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

#define APPEND(off, buf, max, ...) do { \
    int _n = snprintf((char*)(buf) + (off), (max) - (off), __VA_ARGS__); \
    if (_n > 0 && (size_t)_n < (max) - (off)) (off) += (size_t)_n; \
} while(0)

/* ── Dashboard ────────────────────────────────────────────── */

static size_t serve_dashboard(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    int tip = query_int(db, "SELECT MAX(height) FROM blocks");
    int peers = query_int(db, "SELECT count(*) FROM peers");
    int tokens = query_int(db, "SELECT count(*) FROM zslp_tokens");
    int64_t shielded = query_int64(db,
        "SELECT COALESCE(sum(value),0) FROM wallet_sapling_notes "
        "WHERE spent_txid IS NULL");
    int z_notes = query_int(db,
        "SELECT count(*) FROM wallet_sapling_notes WHERE spent_txid IS NULL");

    /* Transparent balance: match P2PKH scripts against wallet keys */
    int64_t transparent = 0;
    int t_utxos = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT count(*), COALESCE(sum(u.value),0) FROM utxos u "
            "WHERE length(u.script) = 25 "
            "AND substr(hex(u.script), 1, 6) = '76A914' "
            "AND EXISTS (SELECT 1 FROM wallet_keys wk "
            "WHERE wk.pubkey_hash = substr(u.script, 4, 20))",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            t_utxos = sqlite3_column_int(s, 0);
            transparent = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);
    }

    int tx_count = query_int(db, "SELECT count(*) FROM wallet_transactions");

    size_t off = 0;
    APPEND(off, r, max, HEADER("Wallet") WALLET_NAV(DASH));

    /* Balance card */
    APPEND(off, r, max,
        "<div class='card' style='border-left-color:#33ff99;padding:20px'>"
        "<div class='label'>Transparent Balance</div>"
        "<div style='font-size:36px;color:#33ff99;font-weight:800'>%.8f ZCL</div>"
        "<div class='sub'>%d UTXO%s at t1YRB...pUXrn</div>"
        "</div>",
        (double)transparent / 1e8, t_utxos, t_utxos == 1 ? "" : "s");

    if (shielded > 0) {
        APPEND(off, r, max,
            "<div class='card' style='border-left-color:#9999ff'>"
            "<div class='label'>Shielded (incoming notes)</div>"
            "<div style='font-size:20px;color:#9999ff;font-weight:700'>%.8f ZCL</div>"
            "<div class='sub'>%d note%s</div>"
            "</div>",
            (double)shielded / 1e8, z_notes, z_notes == 1 ? "" : "s");
    }

    /* Stats row */
    APPEND(off, r, max,
        "<div class='stats'>"
        "<div class='stat'><div class='n'>%d</div><div class='l'>Height</div></div>"
        "<div class='stat'><div class='n'>%d</div><div class='l'>Peers</div></div>"
        "<div class='stat'><div class='n'>%d</div><div class='l'>Tokens</div></div>"
        "<div class='stat'><div class='n'>%d</div><div class='l'>Txs</div></div>"
        "</div>",
        tip, peers, tokens, tx_count);

    /* Quick actions */
    APPEND(off, r, max,
        "<h2>Quick Actions</h2>"
        "<div class='stats'>"
        "<div class='stat'><a href='/wallet/send' style='color:#33ff99;"
        "font-size:18px;font-weight:700'>Send ZCL</a></div>"
        "<div class='stat'><a href='/wallet/receive' style='color:#4db8ff;"
        "font-size:18px;font-weight:700'>Receive</a></div>"
        "<div class='stat'><a href='/wallet/history' style='color:#e8e8e8;"
        "font-size:18px;font-weight:700'>History</a></div>"
        "</div>");

    /* Recent txs */
    APPEND(off, r, max, "<h2>Recent Activity</h2>");
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(txid), block_height FROM wallet_transactions "
            "ORDER BY block_height DESC LIMIT 5",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 300 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            if (!txid) continue;
            char st[18], lt[65];
            size_t tl = strlen(txid);
            snprintf(st, sizeof(st), "%.8s...%.4s",
                     txid, tl >= 4 ? txid + tl - 4 : txid);
            for (size_t i = 0; i < tl && i < 64; i++)
                lt[i] = (txid[i]>='A' && txid[i]<='F') ? (char)(txid[i]+32) : txid[i];
            lt[tl<64?tl:64] = '\0';
            APPEND(off, r, max,
                "<div style='padding:6px 0;border-bottom:1px solid #1a1a1a'>"
                "<a href='/explorer/tx/%s' class='hash'>%s</a>"
                " <span style='color:#555;font-size:12px'>h=%d</span></div>",
                lt, st, h);
        }
        sqlite3_finalize(s);
    }

    APPEND(off, r, max,
        "<footer>ZClassic23 — pure C23 full node + Tor</footer>" FOOTER);
    sqlite3_close(db);
    return off;
}

/* ── Send page ────────────────────────────────────────────── */

static size_t serve_send(uint8_t *r, size_t max) {
    size_t off = 0;
    APPEND(off, r, max, HEADER("Send") WALLET_NAV(SEND));

    APPEND(off, r, max,
        "<h2>Send ZCL</h2>"
        "<div class='card'>"
        "<label class='label'>To Address</label>"
        "<input type='text' placeholder='t1... or zs1...'>"
        "<label class='label' style='margin-top:12px'>Amount (ZCL)</label>"
        "<input type='text' placeholder='0.00000000'>"
        "<label class='label' style='margin-top:12px'>Fee</label>"
        "<input type='text' value='0.0001' style='color:#666'>"
        "<button style='margin-top:16px'>Send Transaction</button>"
        "<p style='color:#666;font-size:12px;margin-top:8px'>"
        "Transactions are signed locally and broadcast via P2P.</p>"
        "</div>");

    APPEND(off, r, max, FOOTER);
    return off;
}

/* ── Receive page ─────────────────────────────────────────── */

static size_t serve_receive(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    size_t off = 0;
    APPEND(off, r, max, HEADER("Receive") WALLET_NAV(RECV));

    APPEND(off, r, max,
        "<h2>Receive ZCL</h2>"
        "<div class='card'>"
        "<div class='label'>Your Transparent Address</div>"
        "<div class='addr-box'>t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn</div>"
        "<div class='sub'>Share this address to receive transparent ZCL.</div>"
        "</div>");

    /* Show z-addresses if available */
    if (db) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT address FROM wallet_sapling_keys "
                "WHERE address IS NOT NULL AND address != '' LIMIT 3",
                -1, &s, NULL) == SQLITE_OK) {
            int count = 0;
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *addr = (const char *)sqlite3_column_text(s, 0);
                if (!addr || !addr[0]) continue;
                if (count == 0)
                    APPEND(off, r, max,
                        "<div class='card' style='border-left-color:#9999ff'>"
                        "<div class='label'>Shielded Address (Sapling)</div>");
                APPEND(off, r, max,
                    "<div class='addr-box' style='color:#9999ff;font-size:11px'>%s</div>",
                    addr);
                count++;
            }
            if (count > 0)
                APPEND(off, r, max,
                    "<div class='sub'>Shielded addresses provide full privacy.</div></div>");
            sqlite3_finalize(s);
        }
        sqlite3_close(db);
    }

    APPEND(off, r, max, FOOTER);
    return off;
}

/* ── History page ─────────────────────────────────────────── */

static size_t serve_history(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    size_t off = 0;
    APPEND(off, r, max, HEADER("History") WALLET_NAV(HIST));

    APPEND(off, r, max,
        "<h2>Transaction History</h2>"
        "<table><tr><th>Tx</th><th>Height</th><th>Time</th></tr>");

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(wt.txid), wt.block_height, b.time "
            "FROM wallet_transactions wt "
            "LEFT JOIN blocks b ON wt.block_height = b.height "
            "ORDER BY wt.block_height DESC LIMIT 50",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 400 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t t = sqlite3_column_int64(s, 2);
            if (!txid) continue;

            char st[18], lt[65];
            size_t tl = strlen(txid);
            snprintf(st, sizeof(st), "%.8s...%.4s",
                     txid, tl >= 4 ? txid + tl - 4 : txid);
            for (size_t i = 0; i < tl && i < 64; i++)
                lt[i] = (txid[i]>='A'&&txid[i]<='F') ? (char)(txid[i]+32) : txid[i];
            lt[tl<64?tl:64] = '\0';

            char ts[32] = "";
            if (t > 0) {
                time_t tt = (time_t)t;
                struct tm tm;
                gmtime_r(&tt, &tm);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
            }

            APPEND(off, r, max,
                "<tr><td><a href='/explorer/tx/%s' class='hash'>%s</a></td>"
                "<td>%d</td><td style='color:#666'>%s</td></tr>",
                lt, st, h, ts);
        }
        sqlite3_finalize(s);
    }

    APPEND(off, r, max, "</table>" FOOTER);
    sqlite3_close(db);
    return off;
}

/* ── Coins page (UTXO analysis) ───────────────────────────── */

static size_t serve_coins(uint8_t *r, size_t max) {
    sqlite3 *db = open_db();
    if (!db) return 0;

    size_t off = 0;
    APPEND(off, r, max, HEADER("Coins") WALLET_NAV(COIN));

    APPEND(off, r, max, "<h2>Coin Analysis</h2>");

    /* Transparent UTXOs */
    APPEND(off, r, max,
        "<h2 style='font-size:16px'>Transparent UTXOs</h2>"
        "<table><tr><th>Txid</th><th>Vout</th>"
        "<th>Amount</th><th>Height</th></tr>");

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT hex(u.txid), u.vout, u.value, u.height FROM utxos u "
            "WHERE length(u.script) = 25 "
            "AND substr(hex(u.script), 1, 6) = '76A914' "
            "AND EXISTS (SELECT 1 FROM wallet_keys wk "
            "WHERE wk.pubkey_hash = substr(u.script, 4, 20)) "
            "ORDER BY u.value DESC",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 300 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int vout = sqlite3_column_int(s, 1);
            int64_t val = sqlite3_column_int64(s, 2);
            int h = sqlite3_column_int(s, 3);
            if (!txid) continue;
            char st[18];
            size_t tl = strlen(txid);
            snprintf(st, sizeof(st), "%.8s...%.4s",
                     txid, tl >= 4 ? txid + tl - 4 : txid);
            APPEND(off, r, max,
                "<tr><td class='hash'>%s</td><td>%d</td>"
                "<td class='zcl'>%.8f</td><td>%d</td></tr>",
                st, vout, (double)val / 1e8, h);
        }
        sqlite3_finalize(s);
    }
    APPEND(off, r, max, "</table>");

    /* Shielded notes */
    APPEND(off, r, max,
        "<h2 style='font-size:16px'>Shielded Notes</h2>"
        "<table><tr><th>Amount</th><th>Height</th><th>Status</th></tr>");

    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value, block_height, "
            "CASE WHEN spent_txid IS NULL THEN 'unspent' ELSE 'spent' END "
            "FROM wallet_sapling_notes ORDER BY block_height DESC",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 200 < max) {
            int64_t val = sqlite3_column_int64(s, 0);
            int h = sqlite3_column_int(s, 1);
            const char *status = (const char *)sqlite3_column_text(s, 2);
            APPEND(off, r, max,
                "<tr><td class='zcl'>%.8f</td><td>%d</td>"
                "<td><span class='pill %s'>%s</span></td></tr>",
                (double)val / 1e8, h,
                (status && status[0] == 'u') ? "pill-z" : "pill-t",
                status ? status : "?");
        }
        sqlite3_finalize(s);
    }
    APPEND(off, r, max, "</table>");

    /* Supply summary */
    APPEND(off, r, max,
        "<h2 style='font-size:16px'>Chain Supply</h2>"
        "<div class='card'>"
        "<div class='label'>Total Transparent Supply</div>"
        "<div class='value'>%.2f ZCL</div>"
        "<div class='sub'>%d UTXOs</div></div>",
        (double)query_int64(db, "SELECT COALESCE(sum(value),0) FROM utxos") / 1e8,
        query_int(db, "SELECT count(*) FROM utxos"));

    APPEND(off, r, max, FOOTER);
    sqlite3_close(db);
    return off;
}

/* ── Router ───────────────────────────────────────────────── */

void wallet_view_init(const char *datadir) {
    g_datadir = datadir;
}

size_t wallet_view_handle_request(const char *method, const char *path,
                                    const uint8_t *body, size_t body_len,
                                    uint8_t *response, size_t response_max)
{
    (void)method; (void)body; (void)body_len;
    if (!path) return 0;

    if (strcmp(path, "/wallet") == 0 || strcmp(path, "/wallet/") == 0)
        return serve_dashboard(response, response_max);
    if (strcmp(path, "/wallet/send") == 0)
        return serve_send(response, response_max);
    if (strcmp(path, "/wallet/receive") == 0)
        return serve_receive(response, response_max);
    if (strcmp(path, "/wallet/history") == 0)
        return serve_history(response, response_max);
    if (strcmp(path, "/wallet/coins") == 0)
        return serve_coins(response, response_max);

    return 0;
}
