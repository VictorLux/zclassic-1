/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zcl-browser: GTK Tor browser + block explorer for zclassic23.
 *
 * Defaults to the block explorer on the local node's .onion address,
 * falling back to the seed node's .onion, or zclnet.net clearnet.
 *
 * Build: make zcl-browser
 * Usage: zcl-browser [.onion-url]
 *        zcl-browser                             → auto-detect explorer
 *        zcl-browser zc23kenf...jnad.onion       → specific .onion */

#define _POSIX_C_SOURCE 200809L
#include <sys/time.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SEED_ONION "zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion"
/* No clearnet — .onion only */

static WebKitWebView *g_webview = NULL;
static GtkWidget *g_url_bar = NULL;
static GtkWidget *g_status_label = NULL;
static bool g_tor_available = false;
static char g_local_onion[128] = "";

/* ── Base64 encoder for RPC auth ──────────────────────────── */

static size_t b64enc(const char *in, size_t n, char *out, size_t mx) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < mx; i += 3) {
        unsigned v = ((unsigned char)in[i]) << 16;
        if (i + 1 < n) v |= ((unsigned char)in[i+1]) << 8;
        if (i + 2 < n) v |= ((unsigned char)in[i+2]);
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ── RPC query to local zclassic23 node ───────────────────── */

static char g_rpc_buf[65536];

static const char *rpc(const char *method) {
    g_rpc_buf[0] = '\0';
    const char *home = getenv("HOME");
    if (!home) return g_rpc_buf;

    char cookie[256] = "";
    char path[512];
    snprintf(path, sizeof(path), "%s/.zclassic-c23/.cookie", home);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(cookie, sizeof(cookie), f)) {
            char *nl = strchr(cookie, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    if (!cookie[0]) return g_rpc_buf;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return g_rpc_buf;
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(18232)
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return g_rpc_buf;
    }

    char body[256];
    int bl = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"method\":\"%s\",\"params\":[],\"id\":1}",
        method);
    char a64[512];
    b64enc(cookie, strlen(cookie), a64, sizeof(a64));
    char req[2048];
    int rl = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\nHost:127.0.0.1\r\n"
        "Authorization:Basic %s\r\n"
        "Content-Type:text/plain\r\n"
        "Content-Length:%d\r\nConnection:close\r\n\r\n%s",
        a64, bl, body);
    (void)write(fd, req, (size_t)rl);

    char raw[65536];
    size_t tot = 0;
    while (tot < sizeof(raw) - 1) {
        ssize_t n = read(fd, raw + tot, sizeof(raw) - 1 - tot);
        if (n <= 0) break;
        tot += (size_t)n;
    }
    raw[tot] = '\0';
    close(fd);

    char *j = strstr(raw, "\r\n\r\n");
    if (j) { j += 4; snprintf(g_rpc_buf, sizeof(g_rpc_buf), "%s", j); }
    return g_rpc_buf;
}

static const char *json_val(const char *json, const char *key,
                             char *out, size_t max) {
    out[0] = '\0';
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return out;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < max - 1) out[i++] = *p++;
        out[i] = '\0';
    } else {
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && i < max - 1) out[i++] = *p++;
        out[i] = '\0';
    }
    return out;
}

/* ── Try to read local .onion address ─────────────────────── */

static void discover_local_onion(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.zclassic-c23/tor_data/onion_service/hostname", home);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_local_onion, sizeof(g_local_onion), f)) {
            char *nl = strchr(g_local_onion, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
}

/* ── Build the explorer URL ───────────────────────────────── */

static const char *get_explorer_url(void) {
    /* Priority: local .onion → seed .onion */
    static char url[256];
    if (g_local_onion[0]) {
        snprintf(url, sizeof(url), "http://%s/explorer", g_local_onion);
        return url;
    }
    snprintf(url, sizeof(url), "http://%s/explorer", SEED_ONION);
    return url;
}

/* ── Dashboard (local node status) ────────────────────────── */

static void load_dashboard(void) {
    const char *info = rpc("getinfo");
    char height[32], peers[32], proto[32];
    json_val(info, "blocks", height, sizeof(height));
    json_val(info, "connections", peers, sizeof(peers));
    json_val(info, "protocolversion", proto, sizeof(proto));

    char html[16384];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:720px;margin:40px auto;padding:0 20px}"
        "h1{color:#00ff88} h2{color:#00cc66;margin-top:24px}"
        ".card{background:#1a1a1a;padding:15px;margin:10px 0;"
        "border-radius:8px;border-left:3px solid #00ff88}"
        ".val{color:#00ff88;font-size:20px}"
        ".label{color:#888;font-size:12px}"
        "a{color:#00aaff;text-decoration:none}"
        ".nav{display:flex;gap:10px;margin:16px 0;flex-wrap:wrap}"
        ".nav a{background:#1a1a1a;padding:10px 18px;border-radius:4px;"
        "border:1px solid #333;font-size:14px}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        "</style></head><body>"
        "<h1>ZClassic23 Node</h1>"
        "<div class='card'>"
        "<span class='label'>Block Height</span><br>"
        "<span class='val'>%s</span></div>"
        "<div class='card'>"
        "<span class='label'>Peers</span><br>"
        "<span class='val'>%s</span></div>"
        "<div class='nav'>"
        "<a href='%s'>Block Explorer</a>"
        "<a href='http://%s/store'>Token Store</a>"
        "<a href='http://%s/'>Network Directory</a>"
        "</div>"
        "%s%s%s"
        "</body></html>",
        height[0] ? height : "syncing...",
        peers[0] ? peers : "0",
        get_explorer_url(),
        g_local_onion[0] ? g_local_onion : SEED_ONION,
        g_local_onion[0] ? g_local_onion : SEED_ONION,
        g_local_onion[0] ? "<div class='card'><span class='label'>Your .onion</span><br><span style='color:#00aaff;font-size:12px;word-break:break-all'>" : "",
        g_local_onion[0] ? g_local_onion : "",
        g_local_onion[0] ? "</span></div>" : "");

    webkit_web_view_load_html(g_webview, html, NULL);
}

/* ── Navigation callbacks ─────────────────────────────────── */

static bool is_onion(const char *url) {
    return url && strstr(url, ".onion") != NULL;
}

static void on_url_activate(GtkEntry *e, gpointer d) {
    (void)d;
    const char *url = gtk_entry_get_text(e);
    if (!url || !url[0]) return;
    char full[512];
    if (strncmp(url, "http", 4) != 0)
        snprintf(full, sizeof(full), "http://%s", url);
    else
        snprintf(full, sizeof(full), "%s", url);

    /* Only allow .onion addresses */
    if (!is_onion(full)) {
        gtk_label_set_text(GTK_LABEL(g_status_label),
            "Blocked: only .onion addresses allowed");
        return;
    }
    webkit_web_view_load_uri(g_webview, full);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, gpointer d) {
    (void)d;
    if (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED) {
        const char *uri = webkit_web_view_get_uri(v);
        if (uri) gtk_entry_set_text(GTK_ENTRY(g_url_bar), uri);
    }
    if (ev == WEBKIT_LOAD_STARTED && g_status_label)
        gtk_label_set_text(GTK_LABEL(g_status_label), "Loading...");
    if (ev == WEBKIT_LOAD_FINISHED && g_status_label)
        gtk_label_set_text(GTK_LABEL(g_status_label), "");
}

static void on_home(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    load_dashboard();
}

static void on_explorer(GtkWidget *b, gpointer d) {
    (void)b; (void)d;
    webkit_web_view_load_uri(g_webview, get_explorer_url());
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

/* Block non-.onion navigation attempts */
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
        if (uri && !strstr(uri, ".onion") &&
            strncmp(uri, "about:", 6) != 0 &&
            strncmp(uri, "data:", 5) != 0) {
            webkit_policy_decision_ignore(dec);
            if (g_status_label)
                gtk_label_set_text(GTK_LABEL(g_status_label),
                    "Blocked: .onion addresses only");
            return TRUE;
        }
    }
    webkit_policy_decision_use(dec);
    return TRUE;
}

/* ── Main ─────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    discover_local_onion();

    /* Tor SOCKS proxy detection */
    WebKitWebContext *ctx = webkit_web_context_get_default();
    int tor_ports[] = {19050, 9050, 0};
    for (int i = 0; tor_ports[i]; i++) {
        int tfd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ta = {
            .sin_family = AF_INET, .sin_port = htons((uint16_t)tor_ports[i])
        };
        inet_pton(AF_INET, "127.0.0.1", &ta.sin_addr);
        if (connect(tfd, (struct sockaddr *)&ta, sizeof(ta)) == 0) {
            close(tfd);
            char proxy[64];
            snprintf(proxy, sizeof(proxy), "socks5://127.0.0.1:%d",
                     tor_ports[i]);
            WebKitNetworkProxySettings *px =
                webkit_network_proxy_settings_new(proxy, NULL);
            webkit_web_context_set_network_proxy_settings(ctx,
                WEBKIT_NETWORK_PROXY_MODE_CUSTOM, px);
            webkit_network_proxy_settings_free(px);
            g_tor_available = true;
            printf("Tor SOCKS on port %d\n", tor_ports[i]);
            break;
        }
        close(tfd);
    }
    if (!g_tor_available)
        printf("No Tor SOCKS found — .onion browsing unavailable\n");

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
    GtkWidget *home_btn = gtk_button_new_with_label("Node");
    GtkWidget *exp_btn = gtk_button_new_with_label("Explorer");

    g_signal_connect(back_btn, "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwd_btn, "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(home_btn, "clicked", G_CALLBACK(on_home), NULL);
    g_signal_connect(exp_btn, "clicked", G_CALLBACK(on_explorer), NULL);

    g_url_bar = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_url_bar),
        "Enter .onion address or search...");
    g_signal_connect(g_url_bar, "activate",
        G_CALLBACK(on_url_activate), NULL);

    g_status_label = gtk_label_new("");

    gtk_box_pack_start(GTK_BOX(hbox), back_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), fwd_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), home_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), exp_btn, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(hbox), g_url_bar, TRUE, TRUE, 4);

    /* WebView — .onion only policy */
    g_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    g_signal_connect(g_webview, "load-changed",
        G_CALLBACK(on_load_changed), NULL);

    /* Block non-.onion navigation */
    g_signal_connect(g_webview, "decide-policy",
        G_CALLBACK(on_decide_policy), NULL);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g_webview), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status_label, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Initial load: CLI arg (.onion) → explorer (.onion) → dashboard */
    if (argc > 1 && is_onion(argv[1])) {
        char full[512];
        if (strncmp(argv[1], "http", 4) == 0)
            snprintf(full, sizeof(full), "%s", argv[1]);
        else
            snprintf(full, sizeof(full), "http://%s", argv[1]);
        webkit_web_view_load_uri(g_webview, full);
    } else if (g_tor_available) {
        webkit_web_view_load_uri(g_webview, get_explorer_url());
    } else {
        load_dashboard();
    }

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
