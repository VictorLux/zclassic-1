/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service: bridges Tor dynhost to zclassic23 MVC controllers.
 * All .onion traffic flows through here. */

#include "net/onion_service.h"
#include "controllers/blog_controller.h"
#include "util/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sqlite3.h>

static char g_onion_address[128] = "";
static const char *g_datadir = NULL;
static time_t g_start_time = 0;

/* Simple global rate limiter: max 100 requests/second */
static _Atomic int64_t g_request_count = 0;
static _Atomic int64_t g_rate_window_start = 0;
#define MAX_REQUESTS_PER_SECOND 100

static bool rate_limit_check(void)
{
    int64_t now = (int64_t)time(NULL);
    int64_t window = atomic_load(&g_rate_window_start);
    if (now != window) {
        atomic_store(&g_rate_window_start, now);
        atomic_store(&g_request_count, 1);
        return true;
    }
    int64_t count = atomic_fetch_add(&g_request_count, 1);
    return count < MAX_REQUESTS_PER_SECOND;
}

/* ── Query node stats from SQLite ─────────────────────────── */

static void query_node_stats(int *out_height, int *out_peers)
{
    *out_height = 0;
    *out_peers = 0;
    if (!g_datadir) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", g_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return;
    /* 5s timeout — allows reads even during heavy block sync */
    sqlite3_busy_timeout(db, 5000);

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            *out_height = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM peers WHERE last_seen > strftime('%s','now') - 3600",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            *out_peers = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
}

/* ── Landing page: node dashboard + directory ─────────────── */

static size_t serve_landing_page(uint8_t *response, size_t max)
{
    /* Gather node info */
    int height = 0, peer_count = 0;
    query_node_stats(&height, &peer_count);

    long uptime = 0;
    if (g_start_time > 0)
        uptime = (long)(time(NULL) - g_start_time);

    /* Discover registered .onion sites from chain */
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    const char *onion = g_onion_address[0] ? g_onion_address : NULL;

    /* Build body into a temp buffer, then wrap with Content-Length */
    char body[32768];
    size_t off = 0;
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>ZClassic23 Node</title>"
        "<style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88;text-align:center;font-size:28px}"
        "h2{color:#00cc66;border-bottom:1px solid #333;padding-bottom:8px}"
        "input[type=text]{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:10px;width:100%%;font-family:monospace;font-size:16px;"
        "border-radius:4px;box-sizing:border-box}"
        ".dashboard{display:grid;grid-template-columns:1fr 1fr 1fr;"
        "gap:12px;margin:20px 0}"
        ".stat{background:#1a1a1a;padding:16px;border-radius:8px;"
        "text-align:center;border-top:2px solid #00ff88}"
        ".stat .val{color:#00ff88;font-size:24px;font-weight:bold}"
        ".stat .label{color:#888;font-size:12px;margin-top:4px}"
        ".onion-addr{background:#111;padding:10px;border-radius:4px;"
        "word-break:break-all;font-size:12px;text-align:center;"
        "color:#00aaff;margin:10px 0}"
        ".nav{display:flex;gap:12px;justify-content:center;margin:20px 0;"
        "flex-wrap:wrap}"
        ".nav a{background:#1a1a1a;color:#00aaff;padding:10px 20px;"
        "border-radius:4px;text-decoration:none;border:1px solid #333}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        ".site{background:#1a1a1a;padding:15px;margin:10px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        ".site a{color:#00aaff;text-decoration:none;font-size:16px}"
        ".site a:hover{text-decoration:underline}"
        ".site .desc{color:#888;font-size:13px;margin-top:5px}"
        ".tagline{text-align:center;color:#666;margin:10px 0}"
        "footer{text-align:center;color:#333;margin-top:40px;font-size:11px}"
        "</style></head><body>"
        "<h1>ZClassic23 Node</h1>"
        "<p class='tagline'>A new internet. Tor-only. No DNS. No cloud.</p>");
    if (n > 0) off = (size_t)n;

    /* Node .onion address */
    if (onion) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='onion-addr'>%s</div>", onion);
        if (n > 0) off += (size_t)n;
    }

    /* Dashboard stats — detect sync-in-progress */
    bool syncing = (height == 0 && uptime < 600) || (height > 0 && height < 100);
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='dashboard'>"
        "<div class='stat'><div class='val'>%s%d</div>"
        "<div class='label'>Block Height</div></div>"
        "<div class='stat'><div class='val'>%d</div>"
        "<div class='label'>Peers (1h)</div></div>"
        "<div class='stat'><div class='val'>%ldm</div>"
        "<div class='label'>Uptime</div></div>"
        "</div>",
        syncing ? "syncing... " : "", height, peer_count, uptime / 60);
    if (syncing) {
        n += snprintf(body + off + (n > 0 ? n : 0),
            sizeof(body) - off - (size_t)(n > 0 ? n : 0),
            "<p style='text-align:center;color:#ffaa00;font-size:14px'>"
            "Node is syncing the blockchain. Stats will update as blocks are indexed."
            "</p>");
    }
    if (n > 0) off += (size_t)n;

    /* Navigation */
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='nav'>"
        "<a href='/store'>Token Store</a>"
        "<a href='/status'>Status API</a>"
        "<a href='/blog'>Blog</a>"
        "<a href='/search'>Search Network</a>"
        "</div>");
    if (n > 0) off += (size_t)n;

    /* Search bar */
    n = snprintf(body + off, sizeof(body) - off,
        "<form action='/search' method='get'>"
        "<input type='text' name='q' placeholder='Search .onion sites...'>"
        "</form>");
    if (n > 0) off += (size_t)n;

    /* Peer directory */
    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Network Directory (%d peers)</h2>", num_peers);
    if (n > 0) off += (size_t)n;

    if (num_peers == 0 && onion) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'>"
            "<a href='http://%s/'>This node</a>"
            "<div class='desc'>Your local ZClassic23 node</div></div>",
            onion);
        if (n > 0) off += (size_t)n;
    }

    for (int i = 0; i < num_peers && off + 512 < sizeof(body); i++) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'>"
            "<a href='http://%s/'>%s</a>"
            "<div class='desc'>Discovered at height %d</div></div>",
            peers[i].hostname, peers[i].hostname, peers[i].height);
        if (n > 0) off += (size_t)n;
    }

    /* Seed node */
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='site'>"
        "<a href='http://zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion/'>"
        "zc23kenf...jnad.onion</a>"
        "<div class='desc'>rhett.dev — ZClassic23 seed node</div></div>"
        "<h2>Host Your Site</h2>"
        "<div class='site'>"
        "<div class='desc'>Every zclassic23 node is a .onion web server.<br>"
        "Put HTML in <code>{datadir}/blog/</code> and it's live.<br>"
        "Register on-chain via ZSLP for network discovery.</div></div>"
        "<footer>ZClassic23 v0.1.0 — pure C23 full node + Tor</footer>"
        "</body></html>");
    if (n > 0) off += (size_t)n;

    /* Wrap with HTTP headers including Content-Length */
    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

/* ── Search handler ───────────────────────────────────────── */

static size_t serve_search(const char *query, uint8_t *response, size_t max)
{
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    char safe_query[512];
    html_escape(safe_query, sizeof(safe_query), query ? query : "");

    char body[16384];
    size_t off = 0;
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><title>Search: %s</title>"
        "<style>body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88}a{color:#00aaff}"
        ".site{background:#1a1a1a;padding:15px;margin:10px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        "</style></head><body>"
        "<h1><a href='/' style='text-decoration:none'>ZClassic23</a> / Search</h1>"
        "<p>Results for: <b>%s</b></p>",
        safe_query, safe_query);
    if (n > 0) off = (size_t)n;

    int found = 0;
    for (int i = 0; i < num_peers && off + 256 < sizeof(body); i++) {
        if (query && query[0] &&
            !strstr(peers[i].hostname, query))
            continue;
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'><a href='http://%s/'>%s</a></div>",
            peers[i].hostname, peers[i].hostname);
        if (n > 0) off += (size_t)n;
        found++;
    }

    if (found == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No results.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off,
        "<p><a href='/'>Back to home</a></p></body></html>");
    if (n > 0) off += (size_t)n;

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

/* ── Status endpoint (JSON API) ───────────────────────────── */

static size_t serve_status(uint8_t *response, size_t max)
{
    int height = 0, peers = 0;
    query_node_stats(&height, &peers);

    long uptime = 0;
    if (g_start_time > 0)
        uptime = (long)(time(NULL) - g_start_time);

    /* Query extra stats from SQLite */
    int64_t last_block_time = 0, tx_count = 0;
    if (g_datadir) {
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/node.db", g_datadir);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 5000);
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT time FROM blocks ORDER BY height DESC LIMIT 1",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    last_block_time = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT count(*) FROM transactions",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    tx_count = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            sqlite3_close(db);
        }
    }

    int64_t now = (int64_t)time(NULL);
    int64_t last_block_age = (last_block_time > 0) ? now - last_block_time : -1;
    bool is_syncing = (height == 0 && uptime < 600) ||
                      (last_block_age > 600 && uptime > 300);

    const char *onion = g_onion_address[0] ? g_onion_address : NULL;

    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\"height\":%d"
        ",\"peers\":%d"
        ",\"version\":\"0.1.0\""
        ",\"uptime\":%ld"
        ",\"syncing\":%s"
        ",\"last_block_age\":%lld"
        ",\"transactions\":%lld"
        "%s%s%s"
        "}",
        height, peers, uptime,
        is_syncing ? "true" : "false",
        (long long)last_block_age,
        (long long)tx_count,
        onion ? ",\"onion\":\"" : "",
        onion ? onion : "",
        onion ? "\"" : "");
    if (blen < 0) blen = 0;

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n"
        "%s", blen, body);
}

/* ── Main request handler ─────────────────────────────────── */

size_t onion_service_handle_request(const char *method,
                                     const char *path,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint8_t *response,
                                     size_t response_max)
{
    if (!path) path = "/";

    /* Rate limit: 100 requests/second across all circuits */
    if (!rate_limit_check()) {
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 429 Too Many Requests\r\n"
            "Content-Type: text/html\r\nConnection: close\r\n"
            "Retry-After: 1\r\n\r\n"
            "<h1>429 Too Many Requests</h1>");
    }

    /* JSON status endpoint */
    if (strcmp(path, "/status") == 0)
        return serve_status(response, response_max);

    /* Landing page / directory */
    if (strcmp(path, "/") == 0)
        return serve_landing_page(response, response_max);

    /* Search */
    if (strncmp(path, "/search", 7) == 0) {
        const char *q = strstr(path, "q=");
        return serve_search(q ? q + 2 : "", response, response_max);
    }

    /* Explorer — block explorer */
    if (strncmp(path, "/explorer", 9) == 0) {
        extern size_t explorer_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t);
        size_t n = explorer_handle_request(method, path, body, body_len,
                                           response, response_max);
        if (n > 0) return n;
    }

    /* Store — ZSLP token commerce */
    if (strncmp(path, "/store", 6) == 0 && g_datadir) {
        extern size_t store_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t, const char *);
        return store_handle_request(method, path, body, body_len,
                                    response, response_max, g_datadir);
    }

    /* Blog (static files from datadir) */
    if (strncmp(path, "/blog", 5) == 0) {
        if (g_datadir)
            return blog_serve(g_datadir, path, (char *)response, response_max);
    }

    /* 404 */
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body style='background:#0a0a0a;color:#e0e0e0;font-family:monospace;"
        "padding:40px'><h1 style='color:#ff4444'>404 Not Found</h1>"
        "<p><a href='/' style='color:#00aaff'>Back to directory</a></p>"
        "</body></html>");
}

/* ── Lifecycle ────────────────────────────────────────────── */

const char *onion_service_start(const char *datadir)
{
    g_datadir = datadir;
    g_start_time = time(NULL);
    return g_onion_address[0] ? g_onion_address : NULL;
}

void onion_service_stop(void)
{
    g_datadir = NULL;
}

const char *onion_service_get_address(void)
{
    return g_onion_address[0] ? g_onion_address : NULL;
}

void onion_service_set_address(const char *address)
{
    if (address) {
        snprintf(g_onion_address, sizeof(g_onion_address), "%s", address);
    } else {
        g_onion_address[0] = '\0';
    }
}

bool onion_service_register_site(const char *title, const char *description)
{
    (void)title; (void)description;
    /* TODO: build ZSLP GENESIS/SEND tx with .onion + title + description */
    return false;
}

int onion_service_search(const char *query,
                          struct onion_site *results, size_t max)
{
    (void)query; (void)results; (void)max;
    /* TODO: scan chain for ZSLP tokens matching query */
    return 0;
}
