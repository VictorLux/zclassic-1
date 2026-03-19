/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-browser: GTK block explorer for zclassic23.
 *
 * Architecture: custom zcl:// URI scheme routes ALL requests
 * (HTML, CSS, images) through in-process C handler calls.
 * No SOCKS. No ports. No network. Just function calls.
 *
 * Build: make zcl-browser
 * Usage: ./zcl-browser [/path] */

#define _POSIX_C_SOURCE 200809L
#include <sys/time.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "models/database.h"
#include "controllers/explorer_controller.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include <sqlite3.h>

/* Handler externs */
extern size_t explorer_handle_request(const char *method, const char *path,
    const uint8_t *body, size_t body_len, uint8_t *response, size_t max);
extern size_t onion_service_handle_request(const char *method, const char *path,
    const uint8_t *body, size_t body_len, uint8_t *response, size_t max);
extern const char *onion_service_start(const char *datadir);
extern void explorer_set_rpc(const char *, const char *, int);

/* ── Globals ──────────────────────────────────────────────── */

static WebKitWebView *g_webview = NULL;
static GtkWidget *g_url_bar = NULL;
static uint8_t g_response[1 << 20]; /* 1MB response buffer */

/* ── URI scheme handler ───────────────────────────────────── */
/* WebKit calls this for ALL zcl:// requests: HTML, CSS, etc. */

static void on_uri_scheme_request(WebKitURISchemeRequest *request,
                                    gpointer user_data)
{
    (void)user_data;
    const char *uri = webkit_uri_scheme_request_get_uri(request);

    /* Extract path from zcl://node/path */
    const char *path = "/explorer";
    if (uri) {
        const char *after = strstr(uri, "://");
        if (after) {
            after += 3;
            const char *slash = strchr(after, '/');
            if (slash) path = slash;
        }
    }

    /* Route to the right handler */
    size_t len = 0;
    if (strncmp(path, "/explorer", 9) == 0 ||
        strstr(path, "style.css") || strstr(path, "favicon"))
        len = explorer_handle_request("GET", path, NULL, 0,
                                       g_response, sizeof(g_response));
    if (len == 0)
        len = onion_service_handle_request("GET", path, NULL, 0,
                                            g_response, sizeof(g_response));

    /* 404 fallback */
    if (len == 0) {
        const char *html = "<html><body style='background:#0c0c0c;color:#e8e8e8;"
            "font-family:monospace;padding:40px;text-align:center'>"
            "<h1 style='color:#ff4444'>404</h1>"
            "<p><a href='/explorer' style='color:#4db8ff'>Explorer</a></p>"
            "</body></html>";
        GInputStream *s = g_memory_input_stream_new_from_data(
            g_strdup(html), (gssize)strlen(html), g_free);
        webkit_uri_scheme_request_finish(request, s,
            (gint64)strlen(html), "text/html");
        g_object_unref(s);
        return;
    }

    /* Strip HTTP headers, detect Content-Type */
    g_response[len < sizeof(g_response) - 1 ? len : sizeof(g_response) - 1] = '\0';
    const char *body = (const char *)g_response;
    const char *ctype = "text/html; charset=utf-8";
    const char *hdr_end = strstr((char *)g_response, "\r\n\r\n");
    if (hdr_end) {
        const char *ct = strstr((char *)g_response, "Content-Type: ");
        if (ct && ct < hdr_end) {
            ct += 14;
            static char ct_buf[128];
            size_t i = 0;
            while (ct[i] && ct[i] != '\r' && ct[i] != '\n' && i < 127) {
                ct_buf[i] = ct[i];
                i++;
            }
            ct_buf[i] = '\0';
            ctype = ct_buf;
        }
        body = hdr_end + 4;
    }

    size_t blen = strlen(body);
    GInputStream *stream = g_memory_input_stream_new_from_data(
        g_strndup(body, blen), (gssize)blen, g_free);
    webkit_uri_scheme_request_finish(request, stream, (gint64)blen, ctype);
    g_object_unref(stream);
}

/* ── Navigation helpers ───────────────────────────────────── */

static void navigate(const char *path) {
    if (!path || !path[0]) path = "/explorer";
    char uri[512];
    snprintf(uri, sizeof(uri), "zcl://node%s", path);
    webkit_web_view_load_uri(g_webview, uri);
}

/* Intercept http:// links → redirect to zcl:// */
static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *dec,
                                   WebKitPolicyDecisionType type, gpointer d)
{
    (void)v; (void)d;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nav =
            WEBKIT_NAVIGATION_POLICY_DECISION(dec);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(nav);
        WebKitURIRequest *req = webkit_navigation_action_get_request(action);
        const char *uri = webkit_uri_request_get_uri(req);

        if (uri && (strncmp(uri, "http://", 7) == 0 ||
                    strncmp(uri, "https://", 8) == 0)) {
            const char *after = strstr(uri, "://");
            if (after) {
                after += 3;
                const char *slash = strchr(after, '/');
                const char *path = slash ? slash : "/explorer";
                webkit_policy_decision_ignore(dec);
                navigate(path);
                return TRUE;
            }
        }
    }
    webkit_policy_decision_use(dec);
    return TRUE;
}

/* Update URL bar when page finishes loading */
static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, gpointer d)
{
    (void)d;
    if (ev == WEBKIT_LOAD_COMMITTED) {
        const char *uri = webkit_web_view_get_uri(v);
        if (uri && g_url_bar) {
            /* Show path only, not zcl://node prefix */
            const char *after = strstr(uri, "://");
            if (after) {
                after += 3;
                const char *slash = strchr(after, '/');
                if (slash)
                    gtk_entry_set_text(GTK_ENTRY(g_url_bar), slash);
            }
        }
    }
}

/* ── Button callbacks ─────────────────────────────────────── */

static void on_back(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    if (webkit_web_view_can_go_back(g_webview))
        webkit_web_view_go_back(g_webview);
}
static void on_forward(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    if (webkit_web_view_can_go_forward(g_webview))
        webkit_web_view_go_forward(g_webview);
}
/* ── Wallet dashboard — direct SQLite, no RPC ─────────────── */

static void serve_wallet_dashboard(void) {
    static char s_datadir[512];
    const char *home = getenv("HOME");
    if (!home) { navigate("/explorer"); return; }
    snprintf(s_datadir, sizeof(s_datadir), "%s/.zclassic-c23", home);

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", s_datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        navigate("/explorer");
        return;
    }
    sqlite3_busy_timeout(db, 3000);

    /* Query wallet data directly from SQLite */
    int t_keys = 0, z_keys = 0, tx_count = 0, peers = 0;
    int64_t shielded_bal = 0;
    int unspent_notes = 0;
    int tip_height = 0;
    char t_addr[64] = "";

    sqlite3_stmt *s = NULL;

    /* T-address count */
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM wallet_keys", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) t_keys = sqlite3_column_int(s, 0);
        sqlite3_finalize(s); s = NULL;
    }

    /* Z-address count */
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM wallet_sapling_keys", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) z_keys = sqlite3_column_int(s, 0);
        sqlite3_finalize(s); s = NULL;
    }

    /* Shielded balance */
    if (sqlite3_prepare_v2(db,
            "SELECT count(*), COALESCE(sum(value),0) FROM wallet_sapling_notes "
            "WHERE spent_txid IS NULL", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            unspent_notes = sqlite3_column_int(s, 0);
            shielded_bal = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s); s = NULL;
    }

    /* Transaction count */
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM wallet_transactions", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) tx_count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s); s = NULL;
    }

    /* Peers */
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM peers", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) peers = sqlite3_column_int(s, 0);
        sqlite3_finalize(s); s = NULL;
    }

    /* Tip height */
    if (sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) tip_height = sqlite3_column_int(s, 0);
        sqlite3_finalize(s); s = NULL;
    }

    /* First t-address */
    if (sqlite3_prepare_v2(db,
            "SELECT address FROM wallet_keys WHERE address IS NOT NULL LIMIT 1",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *a = (const char *)sqlite3_column_text(s, 0);
            if (a) snprintf(t_addr, sizeof(t_addr), "%s", a);
        }
        sqlite3_finalize(s); s = NULL;
    }

    /* Build HTML */
    char html[16384];
    int n = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><style>"
        "body{font-family:monospace;background:#0c0c0c;color:#e8e8e8;"
        "max-width:900px;margin:0 auto;padding:20px}"
        "h1{color:#33ff99;margin:0 0 4px}h2{color:#33ff99;font-size:20px;"
        "border-bottom:2px solid #1a1a1a;padding-bottom:8px;margin:28px 0 12px}"
        ".stats{display:flex;gap:12px;margin:16px 0;flex-wrap:wrap}"
        ".stat{flex:1;min-width:180px;background:#141414;padding:16px;"
        "border-radius:10px;border:1px solid #1e1e1e;text-align:center}"
        ".stat .num{font-size:32px;color:#33ff99;font-weight:800}"
        ".stat .lbl{font-size:12px;color:#888;text-transform:uppercase;"
        "letter-spacing:1px;margin-top:4px}"
        ".card{background:#141414;padding:16px 20px;border-radius:10px;"
        "border-left:4px solid #33ff99;margin:10px 0;border:1px solid #1e1e1e;"
        "border-left:4px solid #33ff99}"
        ".addr{font-size:13px;color:#4db8ff;word-break:break-all;"
        "background:#0a0a0a;padding:10px;border-radius:6px;margin:8px 0}"
        "a{color:#4db8ff;text-decoration:none}"
        ".nav{display:flex;gap:12px;margin:16px 0;flex-wrap:wrap}"
        ".nav a{background:#141414;padding:10px 18px;border-radius:8px;"
        "border:1px solid #1e1e1e;font-size:16px;font-weight:600}"
        ".nav a:hover{border-color:#33ff99;color:#33ff99}"
        "</style></head><body>"
        "<h1>ZClassic23 Wallet</h1>"
        "<p style='color:#666;margin:0 0 16px'>Direct SQLite — no ports, no RPC</p>"

        "<div class='stats'>"
        "<div class='stat'><div class='num'>%.8f</div><div class='lbl'>Shielded ZCL</div></div>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%d</div><div class='lbl'>Peers</div></div>"
        "</div>"

        "<div class='nav'>"
        "<a href='/explorer'>Explorer</a>"
        "<a href='/explorer/tokens'>Tokens</a>"
        "<a href='/store'>Store</a>"
        "</div>"

        "<h2>Wallet</h2>"
        "<div class='card'>"
        "<div style='color:#888;font-size:13px'>Shielded Balance</div>"
        "<div style='font-size:28px;color:#33ff99;font-weight:800'>%.8f ZCL</div>"
        "<div style='color:#666;font-size:13px;margin-top:4px'>%d unspent notes</div>"
        "</div>"

        "<div class='card'>"
        "<div style='color:#888;font-size:13px'>Keys</div>"
        "<div style='font-size:18px'>%d transparent &middot; %d shielded</div>"
        "</div>"

        "<div class='card'>"
        "<div style='color:#888;font-size:13px'>Transactions</div>"
        "<div style='font-size:18px'>%d wallet transactions</div>"
        "</div>",

        (double)shielded_bal / 1e8, tip_height, peers,
        (double)shielded_bal / 1e8, unspent_notes,
        t_keys, z_keys, tx_count);

    size_t off = (size_t)n;

    /* Recent transactions */
    n = snprintf(html + off, sizeof(html) - off,
        "<h2>Recent Transactions</h2>");
    off += (size_t)n;

    if (sqlite3_prepare_v2(db,
            "SELECT hex(txid), block_height FROM wallet_transactions "
            "ORDER BY block_height DESC LIMIT 10",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && off + 256 < sizeof(html)) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            if (!txid) continue;
            char short_tx[18];
            size_t tlen = strlen(txid);
            if (tlen >= 16)
                snprintf(short_tx, sizeof(short_tx), "%.8s...%.4s", txid, txid + tlen - 4);
            else
                snprintf(short_tx, sizeof(short_tx), "%s", txid);

            /* Lowercase for links */
            char ltxid[65];
            for (size_t i = 0; i < tlen && i < 64; i++)
                ltxid[i] = (txid[i] >= 'A' && txid[i] <= 'F') ? (char)(txid[i]+32) : txid[i];
            ltxid[tlen < 64 ? tlen : 64] = '\0';

            n = snprintf(html + off, sizeof(html) - off,
                "<div class='card' style='padding:10px 16px'>"
                "<a href='/explorer/tx/%s' style='font-family:monospace;font-size:14px'>%s</a>"
                " <span style='color:#666;font-size:13px'>height %d</span></div>",
                ltxid, short_tx, h);
            if (n > 0) off += (size_t)n;
        }
        sqlite3_finalize(s);
    }

    snprintf(html + off, sizeof(html) - off, "</body></html>");
    sqlite3_close(db);

    webkit_web_view_load_html(g_webview, html, NULL);
    gtk_entry_set_text(GTK_ENTRY(g_url_bar), "/wallet");
}

static void on_home(GtkWidget *b, gpointer d) {
    (void)b; (void)d; serve_wallet_dashboard();
}
static void on_explorer(GtkWidget *b, gpointer d) {
    (void)b; (void)d; navigate("/explorer");
}
static void on_store(GtkWidget *b, gpointer d) {
    (void)b; (void)d; navigate("/store");
}

static void on_url_activate(GtkEntry *e, gpointer d) {
    (void)d;
    const char *text = gtk_entry_get_text(e);
    if (!text || !text[0]) return;
    if (text[0] == '/')
        navigate(text);
    else {
        char path[512];
        snprintf(path, sizeof(path), "/explorer/search?q=%s", text);
        navigate(path);
    }
}

/* ── Initialization ───────────────────────────────────────── */

static void init_explorer(void)
{
    const char *home = getenv("HOME");
    if (!home) return;

    static char datadir[512];
    static struct node_db ndb;
    snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23", home);

    /* Open database — try migrations, fall back to raw open */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    if (!node_db_open(&ndb, db_path)) {
        if (sqlite3_open_v2(db_path, &ndb.db,
                SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
            ndb.open = true;
            sqlite3_busy_timeout(ndb.db, 5000);
        }
    }

    if (!ndb.open) return;

    explorer_set_state(NULL, NULL, NULL, &ndb, datadir);

    /* Read RPC cookie for explorer proxy */
    char cookie_path[1024], cookie[256] = "";
    snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", datadir);
    FILE *f = fopen(cookie_path, "r");
    if (f) {
        if (fgets(cookie, sizeof(cookie), f)) {
            char *nl = strchr(cookie, '\n');
            if (nl) *nl = '\0';
            char *colon = strchr(cookie, ':');
            if (colon) {
                *colon = '\0';
                explorer_set_rpc(cookie, colon + 1, 18232);
            }
        }
        fclose(f);
    }

    onion_service_start(datadir);
}

/* ── Main ─────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();
    init_explorer();

    /* Register zcl:// scheme before creating WebView */
    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "zcl",
        on_uri_scheme_request, NULL, NULL);
    WebKitSecurityManager *sec = webkit_web_context_get_security_manager(ctx);
    webkit_security_manager_register_uri_scheme_as_local(sec, "zcl");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sec, "zcl");

    /* Window */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ZClassic23");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 800);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Toolbar */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    const char *labels[] = {"<", ">", "Wallet", "Explorer", "Store"};
    GCallback cbs[] = {
        G_CALLBACK(on_back), G_CALLBACK(on_forward),
        G_CALLBACK(on_home), G_CALLBACK(on_explorer), G_CALLBACK(on_store)
    };
    for (int i = 0; i < 5; i++) {
        GtkWidget *btn = gtk_button_new_with_label(labels[i]);
        g_signal_connect(btn, "clicked", cbs[i], NULL);
        gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 2);
    }

    g_url_bar = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_url_bar),
        "Search blocks, txs, addresses...");
    g_signal_connect(g_url_bar, "activate", G_CALLBACK(on_url_activate), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_url_bar, TRUE, TRUE, 4);

    /* WebView */
    g_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_signal_connect(g_webview, "decide-policy",
        G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(g_webview, "load-changed",
        G_CALLBACK(on_load_changed), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_webview), TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Initial page — wallet dashboard by default */
    if (argc > 1 && argv[1][0] == '/')
        navigate(argv[1]);
    else
        serve_wallet_dashboard();

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
