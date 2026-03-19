/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-browser: GTK block explorer for zclassic23.
 *
 * No SOCKS. No ports. No external Tor process.
 * Calls onion_service_handle_request() and explorer_handle_request()
 * directly in-process. HTML rendered in WebKit GTK.
 *
 * Build: make zcl-browser
 * Usage: ./zcl-browser */

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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Link against the zclassic23 libraries for direct handler access */
extern size_t explorer_handle_request(const char *method, const char *path,
                                       const uint8_t *body, size_t body_len,
                                       uint8_t *response, size_t response_max);
extern size_t onion_service_handle_request(const char *method, const char *path,
                                            const uint8_t *body, size_t body_len,
                                            uint8_t *response, size_t response_max);
extern const char *onion_service_start(const char *datadir);

/* Explorer + database initialization */
#include "models/database.h"
#include "controllers/explorer_controller.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include <sqlite3.h>

static WebKitWebView *g_webview = NULL;
static GtkWidget *g_url_bar = NULL;
static GtkWidget *g_status_label = NULL;
/* No re-entrancy guard needed — zcl:// scheme handler is synchronous */

/* Response buffer — large enough for explorer pages */
static uint8_t g_response[1 << 20]; /* 1MB */

/* ── Custom URI scheme handler: zcl://path ────────────────── */
/* WebKit calls this for ALL requests (HTML, CSS, images, etc.)
 * when using the zcl:// scheme. This replaces load_html + decide-policy. */

static void on_uri_scheme_request(WebKitURISchemeRequest *request,
                                    gpointer user_data)
{
    (void)user_data;
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    /* URI format: zcl:///path or zcl://host/path */
    const char *path = "/explorer";
    if (uri) {
        const char *after = strstr(uri, "zcl://");
        if (after) {
            after += 6; /* skip "zcl://" */
            const char *slash = strchr(after, '/');
            if (slash) path = slash;
        }
    }
    printf("zcl:// request: %s\n", path);

    /* Call our C handlers directly */
    size_t len = 0;
    if (strncmp(path, "/explorer", 9) == 0 ||
        strstr(path, "style.css") ||
        strstr(path, "favicon")) {
        len = explorer_handle_request("GET", path, NULL, 0,
                                       g_response, sizeof(g_response));
    }
    if (len == 0) {
        len = onion_service_handle_request("GET", path, NULL, 0,
                                            g_response, sizeof(g_response));
    }

    if (len == 0) {
        const char *err = "<h1>Not Found</h1>";
        GInputStream *stream = g_memory_input_stream_new_from_data(
            g_strdup(err), (gssize)strlen(err), g_free);
        webkit_uri_scheme_request_finish(request, stream,
            (gint64)strlen(err), "text/html");
        g_object_unref(stream);
        return;
    }

    /* Strip HTTP headers to get body + detect content type */
    g_response[len < sizeof(g_response) - 1 ? len : sizeof(g_response) - 1] = '\0';
    const char *body = (const char *)g_response;
    const char *content_type = "text/html";
    const char *hdr_end = strstr((char *)g_response, "\r\n\r\n");
    if (hdr_end) {
        /* Extract Content-Type from headers */
        const char *ct = strstr((char *)g_response, "Content-Type: ");
        if (ct && ct < hdr_end) {
            ct += 14;
            static char ct_buf[128];
            size_t i = 0;
            while (ct[i] && ct[i] != '\r' && ct[i] != '\n' && i < 127)
                ct_buf[i] = ct[i], i++;
            ct_buf[i] = '\0';
            content_type = ct_buf;
        }
        body = hdr_end + 4;
    }

    size_t body_len = strlen(body);
    GInputStream *stream = g_memory_input_stream_new_from_data(
        g_strndup(body, body_len), (gssize)body_len, g_free);
    webkit_uri_scheme_request_finish(request, stream,
        (gint64)body_len, content_type);
    g_object_unref(stream);
}

/* Navigate to a path using the zcl:// scheme */
static void serve_path(const char *path) {
    if (!path || !path[0]) path = "/explorer";
    char uri[512];
    snprintf(uri, sizeof(uri), "zcl://node%s", path);
    webkit_web_view_load_uri(g_webview, uri);
    gtk_entry_set_text(GTK_ENTRY(g_url_bar), path);
}

/* ── Navigation callbacks ─────────────────────────────────── */

static void on_url_activate(GtkEntry *e, gpointer d) {
    (void)d;
    const char *text = gtk_entry_get_text(e);
    if (!text || !text[0]) return;

    if (text[0] == '/') {
        serve_path(text);
    } else {
        /* Treat as search query */
        char path[512];
        snprintf(path, sizeof(path), "/explorer/search?q=%s", text);
        serve_path(path);
    }
}

/* Intercept http:// and https:// navigations → redirect to zcl:// */
static gboolean on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *dec,
                                   WebKitPolicyDecisionType type, gpointer d) {
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
            /* Extract path from http URL and redirect to zcl:// */
            const char *after = strstr(uri, "://");
            if (after) {
                after += 3;
                const char *slash = strchr(after, '/');
                const char *path = slash ? slash : "/explorer";
                webkit_policy_decision_ignore(dec);
                serve_path(path);
                return TRUE;
            }
        }
    }
    webkit_policy_decision_use(dec);
    return TRUE;
}

static void on_home(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    serve_path("/");
}

static void on_explorer(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    serve_path("/explorer");
}

static void on_store(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    serve_path("/store");
}

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

/* ── Main ─────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* Initialize crypto + chain params (needed by explorer) */
    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();

    /* Open the node database and initialize explorer */
    const char *home = getenv("HOME");
    static char s_datadir[512];
    static struct node_db s_ndb;
    if (home) {
        snprintf(s_datadir, sizeof(s_datadir), "%s/.zclassic-c23", home);

        /* Open node.db for block/tx/utxo queries.
         * Use node_db_open which runs migrations — if it fails (e.g. schema
         * mismatch), fall back to raw sqlite3_open for read-only explorer. */
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/node.db", s_datadir);
        if (!node_db_open(&s_ndb, db_path)) {
            /* Migration failed — try raw open for read-only access */
            if (sqlite3_open_v2(db_path, &s_ndb.db,
                    SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
                s_ndb.open = true;
                sqlite3_busy_timeout(s_ndb.db, 5000);
                printf("Explorer: opened %s (read-only, skip migrations)\n",
                       db_path);
            }
        }
        if (s_ndb.open) {
            explorer_set_state(NULL, NULL, NULL, &s_ndb, s_datadir);

            /* Set RPC credentials from cookie file for explorer proxy */
            extern void explorer_set_rpc(const char *, const char *, int);
            char cookie_path[1024], cookie[256] = "";
            snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", s_datadir);
            FILE *cf = fopen(cookie_path, "r");
            if (cf) {
                if (fgets(cookie, sizeof(cookie), cf)) {
                    char *nl = strchr(cookie, '\n');
                    if (nl) *nl = '\0';
                    char *colon = strchr(cookie, ':');
                    if (colon) {
                        *colon = '\0';
                        explorer_set_rpc(cookie, colon + 1, 18232);
                        printf("Explorer: RPC auth from cookie (port 18232)\n");
                    }
                }
                fclose(cf);
            }
            printf("Explorer: ready with %s\n", db_path);
        } else {
            printf("Explorer: no database at %s\n", db_path);
        }

        onion_service_start(s_datadir);
    }

    /* Window */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ZClassic23 Explorer");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 800);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Toolbar */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *back_btn = gtk_button_new_with_label("<");
    GtkWidget *fwd_btn = gtk_button_new_with_label(">");
    GtkWidget *home_btn = gtk_button_new_with_label("Home");
    GtkWidget *exp_btn = gtk_button_new_with_label("Explorer");
    GtkWidget *store_btn = gtk_button_new_with_label("Store");

    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwd_btn, "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(home_btn, "clicked", G_CALLBACK(on_home), NULL);
    g_signal_connect(exp_btn, "clicked", G_CALLBACK(on_explorer), NULL);
    g_signal_connect(store_btn, "clicked", G_CALLBACK(on_store), NULL);

    g_url_bar = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_url_bar),
        "Search blocks, transactions, addresses...");
    g_signal_connect(g_url_bar, "activate",
        G_CALLBACK(on_url_activate), NULL);

    g_status_label = gtk_label_new("");

    gtk_box_pack_start(GTK_BOX(hbox), back_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), fwd_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), home_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), exp_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), store_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), g_url_bar, TRUE, TRUE, 4);

    /* Register zcl:// URI scheme so ALL requests (HTML, CSS, images)
     * go through our in-process handler — no network, no SOCKS. */
    WebKitWebContext *wctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(wctx, "zcl",
        on_uri_scheme_request, NULL, NULL);

    /* Allow zcl:// to access local resources */
    WebKitSecurityManager *sec = webkit_web_context_get_security_manager(wctx);
    webkit_security_manager_register_uri_scheme_as_local(sec, "zcl");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sec, "zcl");

    g_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());

    /* Catch http:// links and redirect to zcl:// for in-process serving */
    g_signal_connect(g_webview, "decide-policy",
        G_CALLBACK(on_decide_policy), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_webview), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status_label, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Initial load */
    const char *initial = (argc > 1) ? argv[1] : "/explorer";
    serve_path(initial);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
