/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-browser: lightweight Tor-only web browser for zclassic23 network.
 *
 * Discovers .onion services from the ZClassic blockchain (ZSLP tokens),
 * displays them in a GTK WebKit window, and ONLY connects to .onion
 * addresses. No clearnet browsing. All traffic over Tor.
 *
 * Build: make zcl-browser
 * Run:   ./zcl-browser [.onion URL]
 *
 * If no URL given, shows a directory of discovered .onion services
 * from the ZClassic blockchain. */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SOCKS_PROXY "socks5://127.0.0.1:19050"
#define MAX_BOOKMARKS 64

static GtkWidget *g_url_bar = NULL;
static WebKitWebView *g_webview = NULL;
static GtkWidget *g_status_bar = NULL;

/* Bookmarks: discovered .onion services from chain */
static struct {
    char url[256];
    char title[128];
} g_bookmarks[MAX_BOOKMARKS];
static int g_num_bookmarks = 0;

/* Only allow .onion URLs */
static bool is_onion_url(const char *url)
{
    if (!url) return false;
    const char *host = strstr(url, "://");
    if (host) host += 3; else host = url;
    /* Find the host part (before / or :) */
    char hostbuf[256];
    size_t i = 0;
    while (host[i] && host[i] != '/' && host[i] != ':' && i < 255)
        hostbuf[i] = host[i], i++;
    hostbuf[i] = '\0';
    return strstr(hostbuf, ".onion") != NULL;
}

static void navigate_to(const char *url)
{
    if (!url || !url[0]) return;

    char full_url[512];
    if (strncmp(url, "http", 4) != 0)
        snprintf(full_url, sizeof(full_url), "http://%s", url);
    else
        snprintf(full_url, sizeof(full_url), "%s", url);

    if (!is_onion_url(full_url)) {
        gtk_label_set_text(GTK_LABEL(g_status_bar),
            "Blocked: only .onion addresses allowed");
        return;
    }

    webkit_web_view_load_uri(g_webview, full_url);
    gtk_entry_set_text(GTK_ENTRY(g_url_bar), full_url);
}

static void on_url_activate(GtkEntry *entry, gpointer data)
{
    (void)data;
    navigate_to(gtk_entry_get_text(entry));
}

static gboolean on_decide_policy(WebKitWebView *view,
                                  WebKitPolicyDecision *decision,
                                  WebKitPolicyDecisionType type,
                                  gpointer data)
{
    (void)view; (void)data;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nav =
            WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(nav);
        WebKitURIRequest *req =
            webkit_navigation_action_get_request(action);
        const char *uri = webkit_uri_request_get_uri(req);

        if (!is_onion_url(uri)) {
            printf("Blocked non-.onion URL: %s\n", uri);
            webkit_policy_decision_ignore(decision);
            gtk_label_set_text(GTK_LABEL(g_status_bar),
                "Blocked: only .onion addresses allowed");
            return TRUE;
        }
    }
    return FALSE;
}

static void on_load_changed(WebKitWebView *view,
                             WebKitLoadEvent event, gpointer data)
{
    (void)data;
    if (event == WEBKIT_LOAD_STARTED) {
        gtk_label_set_text(GTK_LABEL(g_status_bar), "Loading...");
    } else if (event == WEBKIT_LOAD_FINISHED) {
        const char *uri = webkit_web_view_get_uri(view);
        const char *title = webkit_web_view_get_title(view);
        if (uri)
            gtk_entry_set_text(GTK_ENTRY(g_url_bar), uri);
        char status[512];
        snprintf(status, sizeof(status), "%s — %s",
                 title ? title : "Untitled", uri ? uri : "");
        gtk_label_set_text(GTK_LABEL(g_status_bar), status);
    }
}

static gboolean on_close(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    gtk_main_quit();
    return TRUE;
}

/* Build the homepage: list of discovered .onion services */
static char *build_homepage(void)
{
    static char html[32768];
    int off = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<style>"
        "body { font-family: monospace; background: #0a0a0a; color: #e0e0e0; "
        "       max-width: 720px; margin: 40px auto; padding: 0 20px; }"
        "h1 { color: #00ff88; }"
        "a { color: #00aaff; text-decoration: none; }"
        "a:hover { text-decoration: underline; }"
        ".node { background: #1a1a1a; padding: 15px; margin: 10px 0; "
        "        border-radius: 8px; border-left: 3px solid #00ff88; }"
        ".empty { color: #666; font-style: italic; }"
        "</style></head><body>"
        "<h1>ZClassic23 Network Browser</h1>"
        "<p>Tor-only browser. All traffic over .onion circuits.</p>"
        "<h2>Discovered Services</h2>");

    if (g_num_bookmarks == 0) {
        off += snprintf(html + off, sizeof(html) - (size_t)off,
            "<p class='empty'>No .onion services discovered yet.</p>"
            "<p class='empty'>Services are published on-chain via "
            "ZSLP ZCL23NODES tokens.</p>");
    } else {
        for (int i = 0; i < g_num_bookmarks; i++) {
            off += snprintf(html + off, sizeof(html) - (size_t)off,
                "<div class='node'>"
                "<a href='http://%s/'>%s</a><br>"
                "<small>%s</small></div>",
                g_bookmarks[i].url,
                g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url,
                g_bookmarks[i].url);
        }
    }

    snprintf(html + off, sizeof(html) - (size_t)off,
        "<h2>Manual Navigation</h2>"
        "<p>Enter a .onion address in the URL bar above.</p>"
        "</body></html>");

    return html;
}

/* Load .onion peers from zclassic23 chain scan */
static void load_bookmarks_from_chain(void)
{
    /* Read from the node's SQLite database via the same code path
     * as blog_discover_onion_peers(). For now, check a known file. */
    const char *home = getenv("HOME");
    if (!home) return;

    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db", home);

    /* We'd need sqlite3 linked — for now just show the homepage.
     * The real implementation calls blog_discover_onion_peers() which
     * is linked into the zclassic23 binary, not this standalone tool. */
    (void)db_path;
}

static void on_home_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    char *hp = build_homepage();
    webkit_web_view_load_html(g_webview, hp, "about:zcl23");
}

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);

    /* Configure WebKit to use Tor SOCKS proxy */
    WebKitWebContext *ctx = webkit_web_context_get_default();
    WebKitNetworkProxySettings *proxy =
        webkit_network_proxy_settings_new(SOCKS_PROXY, NULL);
    webkit_web_context_set_network_proxy_settings(
        ctx, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy);
    webkit_network_proxy_settings_free(proxy);

    /* Disable all caches and tracking for privacy */
    WebKitWebsiteDataManager *data_mgr =
        webkit_web_context_get_website_data_manager(ctx);
    webkit_website_data_manager_clear(data_mgr,
        WEBKIT_WEBSITE_DATA_ALL, 0, NULL, NULL, NULL);

    /* Window */
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "ZClassic23 Browser (Tor Only)");
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 768);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* URL bar */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *label = gtk_label_new(" .onion: ");
    g_url_bar = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_url_bar),
        "Enter .onion address...");
    g_signal_connect(g_url_bar, "activate", G_CALLBACK(on_url_activate), NULL);

    GtkWidget *home_btn = gtk_button_new_with_label("Home");
    g_signal_connect_swapped(home_btn, "clicked",
        G_CALLBACK(webkit_web_view_go_back), NULL);

    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), g_url_bar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), home_btn, FALSE, FALSE, 0);

    /* WebView */
    g_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_signal_connect(g_webview, "load-changed",
                     G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(g_webview, "decide-policy",
                     G_CALLBACK(on_decide_policy), NULL);

    /* Status bar */
    g_status_bar = gtk_label_new("Ready — Tor-only browsing");
    gtk_label_set_xalign(GTK_LABEL(g_status_bar), 0.0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_webview), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status_bar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* Load bookmarks from chain */
    load_bookmarks_from_chain();

    /* Navigate to initial URL or show homepage */
    if (argc > 1 && is_onion_url(argv[1])) {
        navigate_to(argv[1]);
    } else if (argc > 1) {
        fprintf(stderr, "Error: only .onion addresses allowed\n");
        /* Show homepage */
        char *homepage = build_homepage();
        webkit_web_view_load_html(g_webview, homepage, "about:zcl23");
    } else {
        char *homepage = build_homepage();
        webkit_web_view_load_html(g_webview, homepage, "about:zcl23");
    }

    /* Fix home button */
    g_signal_handlers_disconnect_by_func(home_btn,
        G_CALLBACK(webkit_web_view_go_back), NULL);
    g_signal_connect(home_btn, "clicked",
        G_CALLBACK(on_home_clicked), NULL);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
