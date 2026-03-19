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

static WebKitWebView *g_webview = NULL;
static GtkWidget *g_url_bar = NULL;
static GtkWidget *g_status_label = NULL;

/* Response buffer — large enough for explorer pages */
static uint8_t g_response[1 << 20]; /* 1MB */

/* ── Direct in-process request handler ────────────────────── */

static void serve_path(const char *path) {
    if (!path || !path[0]) path = "/explorer";

    /* Try explorer first, then onion service */
    size_t len = 0;
    if (strncmp(path, "/explorer", 9) == 0 || strcmp(path, "/style.css") == 0) {
        len = explorer_handle_request("GET", path, NULL, 0,
                                       g_response, sizeof(g_response));
    }
    if (len == 0) {
        len = onion_service_handle_request("GET", path, NULL, 0,
                                            g_response, sizeof(g_response));
    }

    if (len == 0) {
        const char *err = "<html><body style='background:#0a0a0a;color:#e0e0e0;"
            "font-family:monospace;padding:40px'>"
            "<h1 style='color:#ff4444'>No Response</h1>"
            "<p>The node returned no data for this path.</p>"
            "<p><a href='/explorer' style='color:#00aaff'>Back to Explorer</a></p>"
            "</body></html>";
        webkit_web_view_load_html(g_webview, err, "http://localhost");
        return;
    }

    /* The response includes HTTP headers — strip them to get body */
    g_response[len < sizeof(g_response) - 1 ? len : sizeof(g_response) - 1] = '\0';
    const char *body = strstr((char *)g_response, "\r\n\r\n");
    if (body) {
        body += 4; /* skip past \r\n\r\n */
    } else {
        body = (const char *)g_response; /* no headers, use raw */
    }

    /* Load into WebKit with a base URI so relative links work */
    char base_uri[128];
    snprintf(base_uri, sizeof(base_uri), "http://localhost%s", path);
    webkit_web_view_load_html(g_webview, body, base_uri);

    /* Update URL bar */
    gtk_entry_set_text(GTK_ENTRY(g_url_bar), path);
    if (g_status_label)
        gtk_label_set_text(GTK_LABEL(g_status_label), "");
}

/* ── Navigation callbacks ─────────────────────────────────── */

static void on_url_activate(GtkEntry *e, gpointer d) {
    (void)d;
    const char *text = gtk_entry_get_text(e);
    if (!text || !text[0]) return;

    /* If it starts with /, treat as local path */
    if (text[0] == '/') {
        serve_path(text);
        return;
    }
    /* If it looks like a search query, redirect to explorer search */
    char path[512];
    snprintf(path, sizeof(path), "/explorer/search?q=%s", text);
    serve_path(path);
}

/* Intercept ALL navigation — handle in-process instead of network */
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

        if (uri) {
            /* Extract path from URI (strip http://localhost) */
            const char *path = uri;
            const char *after_scheme = strstr(uri, "://");
            if (after_scheme) {
                after_scheme += 3;
                const char *slash = strchr(after_scheme, '/');
                if (slash) path = slash;
                else path = "/explorer";
            }

            /* Block the WebKit navigation — we handle it ourselves */
            webkit_policy_decision_ignore(dec);

            /* Serve the path directly */
            serve_path(path);
            return TRUE;
        }
    }

    /* Allow non-navigation decisions (resources loaded from load_html) */
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

    /* Initialize the onion service layer (sets up datadir, etc.) */
    const char *home = getenv("HOME");
    if (home) {
        char datadir[512];
        snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23", home);
        onion_service_start(datadir);
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

    /* WebView — all navigation intercepted */
    g_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_signal_connect(g_webview, "decide-policy",
        G_CALLBACK(on_decide_policy), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_webview), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status_label, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Initial load: CLI arg or default to explorer */
    const char *initial = "/explorer";
    if (argc > 1) initial = argv[1];
    serve_path(initial);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
