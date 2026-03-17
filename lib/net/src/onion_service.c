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

static char g_onion_address[128] = "";
static const char *g_datadir = NULL;

/* ── Landing page: directory of all .onion sites ──────────── */

static size_t serve_landing_page(uint8_t *response, size_t max)
{
    /* Discover registered .onion sites from chain */
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    int off = snprintf((char *)response, max,
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

    if (num_peers == 0) {
        off += snprintf((char *)response + off, max - (size_t)off,
            "<div class='site'>"
            "<a href='http://%s/'>This node</a>"
            "<div class='desc'>Your local ZClassic23 node</div></div>",
            g_onion_address[0] ? g_onion_address : "localhost");
    }

    for (int i = 0; i < num_peers && (size_t)off < max - 256; i++) {
        off += snprintf((char *)response + off, max - (size_t)off,
            "<div class='site'>"
            "<a href='http://%s/'>%s</a>"
            "<div class='desc'>Discovered at height %d</div></div>",
            peers[i].hostname, peers[i].hostname, peers[i].height);
    }

    /* Always show the seed node */
    off += snprintf((char *)response + off, max - (size_t)off,
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

    return (size_t)off;
}

/* ── Search handler ───────────────────────────────────────── */

static size_t serve_search(const char *query, uint8_t *response, size_t max)
{
    struct onion_peer peers[64];
    int num_peers = 0;
    if (g_datadir)
        num_peers = blog_discover_onion_peers(g_datadir, peers, 64);

    int off = snprintf((char *)response, max,
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
        query ? query : "", query ? query : "");

    int found = 0;
    for (int i = 0; i < num_peers && (size_t)off < max - 256; i++) {
        if (query && query[0] &&
            !strstr(peers[i].hostname, query))
            continue;
        off += snprintf((char *)response + off, max - (size_t)off,
            "<div class='site'><a href='http://%s/'>%s</a></div>",
            peers[i].hostname, peers[i].hostname);
        found++;
    }

    if (found == 0)
        off += snprintf((char *)response + off, max - (size_t)off,
            "<p style='color:#666'>No results.</p>");

    snprintf((char *)response + off, max - (size_t)off, "</body></html>");
    return strlen((char *)response);
}

/* ── Main request handler ─────────────────────────────────── */

size_t onion_service_handle_request(const char *method,
                                     const char *path,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint8_t *response,
                                     size_t response_max)
{
    (void)body; (void)body_len;

    if (!path) path = "/";

    /* Landing page / directory */
    if (strcmp(path, "/") == 0)
        return serve_landing_page(response, response_max);

    /* Search */
    if (strncmp(path, "/search", 7) == 0) {
        const char *q = strstr(path, "q=");
        return serve_search(q ? q + 2 : "", response, response_max);
    }

    /* Blog (static files from datadir) */
    if (strncmp(path, "/blog", 5) == 0 || strcmp(method, "GET") == 0) {
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
