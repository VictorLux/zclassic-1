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
static void on_home(GtkWidget *b, gpointer d) {
    (void)b; (void)d; navigate("/");
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

    const char *labels[] = {"<", ">", "Home", "Explorer", "Store"};
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

    /* Initial page */
    navigate(argc > 1 ? argv[1] : "/explorer");

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
