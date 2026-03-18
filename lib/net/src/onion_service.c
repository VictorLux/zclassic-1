/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service: bridges Tor dynhost to zclassic23 MVC controllers.
 * All .onion traffic flows through here. */

#include "net/onion_service.h"
#include "controllers/blog_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sqlite3.h>

static char g_onion_address[128] = "";
static const char *g_datadir = NULL;

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

/* HTML-escape a string to prevent XSS. Writes at most max-1 bytes + null. */
static size_t html_escape(char *dst, size_t max, const char *src)
{
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 6 < max; i++) {
        switch (src[i]) {
        case '<':  w += (size_t)snprintf(dst + w, max - w, "&lt;"); break;
        case '>':  w += (size_t)snprintf(dst + w, max - w, "&gt;"); break;
        case '&':  w += (size_t)snprintf(dst + w, max - w, "&amp;"); break;
        case '"':  w += (size_t)snprintf(dst + w, max - w, "&quot;"); break;
        case '\'': w += (size_t)snprintf(dst + w, max - w, "&#39;"); break;
        default:   dst[w++] = src[i]; break;
        }
    }
    dst[w] = '\0';
    return w;
}

/* ── Landing page: directory of all .onion sites ──────────── */

static size_t serve_landing_page(uint8_t *response, size_t max)
{
    /* Discover registered .onion sites from chain */
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    size_t off = 0;
    int n = snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>ZClassic23 Network</title>"
        "<style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88;text-align:center;font-size:28px}"
        "h2{color:#00cc66;border-bottom:1px solid #333;padding-bottom:8px}"
        "input[type=text]{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:10px;width:100%%;font-family:monospace;font-size:16px;border-radius:4px}"
        ".site{background:#1a1a1a;padding:15px;margin:10px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        ".site a{color:#00aaff;text-decoration:none;font-size:16px}"
        ".site a:hover{text-decoration:underline}"
        ".site .desc{color:#888;font-size:13px;margin-top:5px}"
        ".stats{text-align:center;color:#666;margin:20px 0}"
        "footer{text-align:center;color:#333;margin-top:40px;font-size:11px}"
        "</style></head><body>"
        "<h1>ZClassic23 Network</h1>"
        "<p class='stats'>A new internet. Tor-only. No DNS. No cloud.</p>"
        "<form action='/search' method='get'>"
        "<input type='text' name='q' placeholder='Search .onion sites...' autofocus>"
        "</form>"
        "<h2>Directory</h2>");
    if (n > 0) off = (size_t)n;

    if (num_peers == 0) {
        n = snprintf((char *)response + off, max - off,
            "<div class='site'>"
            "<a href='http://%s/'>This node</a>"
            "<div class='desc'>Your local ZClassic23 node</div></div>",
            g_onion_address[0] ? g_onion_address : "localhost");
        if (n > 0) off += (size_t)n;
    }

    for (int i = 0; i < num_peers && off + 256 < max; i++) {
        n = snprintf((char *)response + off, max - off,
            "<div class='site'>"
            "<a href='http://%s/'>%s</a>"
            "<div class='desc'>Discovered at height %d</div></div>",
            peers[i].hostname, peers[i].hostname, peers[i].height);
        if (n > 0) off += (size_t)n;
    }

    /* Always show the seed node */
    n = snprintf((char *)response + off, max - off,
        "<div class='site'>"
        "<a href='http://zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion/'>"
        "zc23kenf...jnad.onion</a>"
        "<div class='desc'>rhett.dev — ZClassic23 seed node</div></div>"
        "<h2>Start Your Site</h2>"
        "<div class='site'>"
        "<div class='desc'>Every zclassic23 node can host a .onion site.<br>"
        "Put HTML in <code>{datadir}/blog/</code> and it's live.<br>"
        "Register it on-chain via ZSLP for others to discover.</div></div>"
        "<footer>Powered by ZClassic23 — pure C23 full node + Tor</footer>"
        "</body></html>");
    if (n > 0) off += (size_t)n;

    return off;
}

/* ── Search handler ───────────────────────────────────────── */

static size_t serve_search(const char *query, uint8_t *response, size_t max)
{
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    /* HTML-escape the query to prevent XSS */
    char safe_query[512];
    html_escape(safe_query, sizeof(safe_query), query ? query : "");

    size_t off = 0;
    int n = snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
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
    for (int i = 0; i < num_peers && off + 256 < max; i++) {
        if (query && query[0] &&
            !strstr(peers[i].hostname, query))
            continue;
        n = snprintf((char *)response + off, max - off,
            "<div class='site'><a href='http://%s/'>%s</a></div>",
            peers[i].hostname, peers[i].hostname);
        if (n > 0) off += (size_t)n;
        found++;
    }

    if (found == 0) {
        n = snprintf((char *)response + off, max - off,
            "<p style='color:#666'>No results.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf((char *)response + off, max - off, "</body></html>");
    if (n > 0) off += (size_t)n;
    return off;
}

/* ── Status endpoint (JSON API) ───────────────────────────── */

static time_t g_start_time = 0;

static size_t serve_status(uint8_t *response, size_t max)
{
    /* Gather node info */
    int height = 0;
    int peers = 0;

    if (g_datadir) {
        /* Query block height from SQLite */
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/node.db", g_datadir);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 1000);
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    height = sqlite3_column_int(s, 0);
                sqlite3_finalize(s);
            }
            s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT COUNT(*) FROM peers WHERE last_seen > strftime('%s','now') - 3600",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (sqlite3_step(s) == SQLITE_ROW)
                    peers = sqlite3_column_int(s, 0);
                sqlite3_finalize(s);
            }
            sqlite3_close(db);
        }
    }

    long uptime = 0;
    if (g_start_time > 0)
        uptime = (long)(time(NULL) - g_start_time);

    char body[512];
    int blen = snprintf(body, sizeof(body),
        "{\"height\":%d,\"peers\":%d,\"version\":\"0.1.0\",\"uptime\":%ld}",
        height, peers, uptime);
    if (blen < 0) blen = 0;

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n\r\n"
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
    /* TODO: when Tor is linked in, call dynhost_init() and register
     * onion_service_handle_request as the handler callback.
     * For now, the handler is available for the HTTP server fallback. */
    printf("Onion service layer initialized (datadir=%s)\n", datadir);
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
