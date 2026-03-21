/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic wallet GUI — single-window, no-tabs, always-visible balance.
 * Reads directly from SQLite. Background thread for all queries.
 *
 * Layout:
 *   ┌─ Title + Sync Status ─────────────────────┐
 *   │  BALANCE (always visible, large)           │
 *   │  [Receive] [Send] buttons                  │
 *   │  ─── inline panel (receive or send) ───    │
 *   │  Recent Activity (scrollable list)         │
 *   │  ─── status bar ──────────────────────     │
 *   └───────────────────────────────────────────┘
 */

#ifdef HAVE_GTK

#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <pthread.h>
#include "keys/key_io.h"

static char g_db_path[600] = "";

/* ── Wallet data (populated by background thread) ──────── */

struct utxo_entry {
    char txid_hex[65];
    int64_t value;
    int height;
    int vout;
};
#define MAX_UTXOS_DISPLAY 100

static struct {
    _Atomic bool ready;
    bool db_ok;

    /* Balance */
    int64_t transparent;
    int64_t shielded;
    int64_t total;

    /* Chain */
    int64_t chain_height;
    int64_t utxo_count;

    /* Address */
    char address[128];

    /* UTXOs (our wallet's) */
    struct utxo_entry utxos[MAX_UTXOS_DISPLAY];
    int num_utxos;
} g_data = {0};

/* ── Helpers ───────────────────────────────────────────── */

static int64_t db_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

static void fmt_zcl(char *buf, size_t max, int64_t zatoshi)
{
    int64_t whole = zatoshi / 100000000;
    int64_t frac = zatoshi % 100000000;
    if (frac < 0) frac = -frac;
    snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
    /* Trim trailing zeros (keep at least .XX) */
    size_t len = strlen(buf);
    while (len > 2 && buf[len-1] == '0' && buf[len-2] != '.') len--;
    buf[len] = 0;
}

/* ── Background query thread ───────────────────────────── */

static void *query_thread(void *arg)
{
    (void)arg;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READONLY, NULL)
        != SQLITE_OK || !db) {
        g_data.db_ok = false;
        atomic_store(&g_data.ready, true);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 200);
    g_data.db_ok = true;

    /* Balance: join global UTXOs with our wallet keys */
    g_data.transparent = db_int(db,
        "SELECT COALESCE(SUM(u.value),0) FROM utxos u"
        " INNER JOIN wallet_keys w ON u.address_hash=w.pubkey_hash");
    g_data.shielded = db_int(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
        " WHERE spent_txid IS NULL");
    g_data.total = g_data.transparent + g_data.shielded;

    /* Chain stats */
    g_data.chain_height = db_int(db,
        "SELECT COALESCE(MAX(height),0) FROM blocks WHERE status>=3");
    g_data.utxo_count = db_int(db, "SELECT COUNT(*) FROM utxos");

    /* Address: encode first wallet key */
    g_data.address[0] = '\0';
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT pubkey_hash FROM wallet_keys LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const void *pkh = sqlite3_column_blob(s, 0);
                int pkh_len = sqlite3_column_bytes(s, 0);
                if (pkh && pkh_len == 20) {
                    struct tx_destination dest;
                    dest.type = DEST_KEY_ID;
                    memcpy(dest.id.key.id.data, pkh, 20);
                    const unsigned char pk[] = {0x1C, 0xB8};
                    const unsigned char sc[] = {0x1C, 0xBD};
                    encode_destination(&dest, pk, 2, sc, 2,
                                       g_data.address,
                                       sizeof(g_data.address));
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* Our UTXOs from global join */
    g_data.num_utxos = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(u.txid), u.vout, u.value, u.height "
                "FROM utxos u INNER JOIN wallet_keys w "
                "ON u.address_hash=w.pubkey_hash "
                "ORDER BY u.height DESC LIMIT ?",
                -1, &s, NULL) == SQLITE_OK && s) {
            sqlite3_bind_int(s, 1, MAX_UTXOS_DISPLAY);
            while (sqlite3_step(s) == SQLITE_ROW &&
                   g_data.num_utxos < MAX_UTXOS_DISPLAY) {
                struct utxo_entry *e = &g_data.utxos[g_data.num_utxos];
                const char *txid = (const char *)sqlite3_column_text(s, 0);
                if (txid)
                    snprintf(e->txid_hex, sizeof(e->txid_hex), "%s", txid);
                else
                    e->txid_hex[0] = '\0';
                e->vout = sqlite3_column_int(s, 1);
                e->value = sqlite3_column_int64(s, 2);
                e->height = sqlite3_column_int(s, 3);
                g_data.num_utxos++;
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_close(db);
    atomic_store(&g_data.ready, true);
    return NULL;
}

static void start_query(void)
{
    atomic_store(&g_data.ready, false);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, query_thread, NULL);
    pthread_attr_destroy(&a);
}

/* ── GTK widgets ───────────────────────────────────────── */

static GtkWidget *lbl_balance;
static GtkWidget *lbl_sync;
static GtkWidget *lbl_status;
static GtkWidget *recv_panel;
static GtkWidget *lbl_address;
static GtkWidget *send_panel;
static GtkWidget *entry_to;
static GtkWidget *entry_amount;
static GtkWidget *lbl_send_status;
static GtkWidget *utxo_list;

/* ── Callbacks ─────────────────────────────────────────── */

static void on_receive_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    gboolean visible = gtk_widget_get_visible(recv_panel);
    gtk_widget_set_visible(recv_panel, !visible);
    gtk_widget_set_visible(send_panel, FALSE);
}

static void on_send_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    gboolean visible = gtk_widget_get_visible(send_panel);
    gtk_widget_set_visible(send_panel, !visible);
    gtk_widget_set_visible(recv_panel, FALSE);
}

static void on_copy_address(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, g_data.address, -1);
    gtk_label_set_text(GTK_LABEL(lbl_address),
                        "Copied to clipboard!");
    /* Reset after 2 seconds */
}

static void on_send_submit(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    const char *to = gtk_entry_get_text(GTK_ENTRY(entry_to));
    const char *amt = gtk_entry_get_text(GTK_ENTRY(entry_amount));
    if (!to || !to[0] || !amt || !amt[0]) {
        gtk_label_set_text(GTK_LABEL(lbl_send_status),
                            "Enter address and amount");
        return;
    }
    /* TODO: call sendtoaddress RPC on running node */
    char msg[256];
    snprintf(msg, sizeof(msg), "Send %s ZCL to %s — RPC not connected", amt, to);
    gtk_label_set_text(GTK_LABEL(lbl_send_status), msg);
}

/* ── Periodic update ───────────────────────────────────── */

static gboolean update_ui(gpointer data)
{
    (void)data;
    if (!atomic_load(&g_data.ready))
        return G_SOURCE_CONTINUE;

    if (!g_data.db_ok) {
        gtk_label_set_text(GTK_LABEL(lbl_balance), "No database");
        gtk_label_set_text(GTK_LABEL(lbl_sync), "");
        start_query();
        return G_SOURCE_CONTINUE;
    }

    /* Balance */
    char buf[80];
    fmt_zcl(buf, sizeof(buf), g_data.total);
    char bal[96];
    snprintf(bal, sizeof(bal), "%s ZCL", buf);
    gtk_label_set_text(GTK_LABEL(lbl_balance), bal);

    /* Sync status */
    snprintf(buf, sizeof(buf), "Block %" PRId64, g_data.chain_height);
    gtk_label_set_text(GTK_LABEL(lbl_sync), buf);

    /* Address (if receive panel is showing and not "copied") */
    if (g_data.address[0] &&
        gtk_widget_get_visible(recv_panel)) {
        const char *cur = gtk_label_get_text(GTK_LABEL(lbl_address));
        if (!cur || strstr(cur, "Copied") == NULL)
            gtk_label_set_text(GTK_LABEL(lbl_address), g_data.address);
    }

    /* Status bar */
    snprintf(buf, sizeof(buf), "%" PRId64 " UTXOs  │  Height %" PRId64,
             g_data.utxo_count, g_data.chain_height);
    gtk_label_set_text(GTK_LABEL(lbl_status), buf);

    /* UTXO list */
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(utxo_list));
        for (GList *l = children; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);

        for (int i = 0; i < g_data.num_utxos; i++) {
            struct utxo_entry *e = &g_data.utxos[i];
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(row), 3);

            char amt[48];
            fmt_zcl(amt, sizeof(amt), e->value);
            char amt_str[64];
            snprintf(amt_str, sizeof(amt_str), "+%s ZCL", amt);

            char ht[24];
            snprintf(ht, sizeof(ht), "h=%d", e->height);

            char txid[20];
            snprintf(txid, sizeof(txid), "%.16s…", e->txid_hex);

            GtkWidget *la = gtk_label_new(amt_str);
            gtk_widget_set_halign(la, GTK_ALIGN_START);
            gtk_widget_set_size_request(la, 180, -1);
            GtkStyleContext *ctx = gtk_widget_get_style_context(la);
            gtk_style_context_add_class(ctx, "received");

            GtkWidget *lh = gtk_label_new(ht);
            gtk_widget_set_halign(lh, GTK_ALIGN_START);
            gtk_style_context_add_class(gtk_widget_get_style_context(lh), "dim");

            GtkWidget *lt = gtk_label_new(txid);
            gtk_label_set_selectable(GTK_LABEL(lt), TRUE);
            gtk_widget_set_halign(lt, GTK_ALIGN_END);
            gtk_style_context_add_class(gtk_widget_get_style_context(lt), "dim");

            gtk_box_pack_start(GTK_BOX(row), la, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), lh, FALSE, FALSE, 0);
            gtk_box_pack_end(GTK_BOX(row), lt, FALSE, FALSE, 0);

            gtk_container_add(GTK_CONTAINER(utxo_list), row);
        }
        gtk_widget_show_all(utxo_list);
    }

    start_query();
    return G_SOURCE_CONTINUE;
}

/* ── Main ──────────────────────────────────────────────── */

int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    snprintf(g_db_path, sizeof(g_db_path), "%s/node.db", datadir);

    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "Cannot open display.\n");
        return 1;
    }

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: #0d1117; }"
        "label { color: #c9d1d9; font-family: 'Inter', 'Segoe UI', sans-serif; }"
        ".balance { font-size: 36px; font-weight: 700; color: #58a6ff; }"
        ".title { font-size: 14px; font-weight: 600; color: #8b949e; }"
        ".sync { font-size: 12px; color: #3fb950; }"
        ".dim { font-size: 12px; color: #484f58; }"
        ".received { font-size: 14px; font-weight: 600; color: #3fb950; font-family: monospace; }"
        ".addr { font-size: 14px; font-family: monospace; color: #58a6ff; }"
        ".status { font-size: 11px; color: #484f58; }"
        ".action-btn { padding: 8px 24px; }"
        "separator { background-color: #21262d; min-height: 1px; }"
        "list { background-color: #0d1117; }"
        "list row { background-color: #0d1117; }"
        "list row:hover { background-color: #161b22; }"
        "entry { background-color: #161b22; color: #c9d1d9; border: 1px solid #30363d;"
        "  border-radius: 6px; padding: 6px 12px; font-family: monospace; }"
        , -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Window */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ZClassic23 Wallet");
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 600);
    gtk_container_set_border_width(GTK_CONTAINER(win), 24);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    /* Header: title + sync */
    {
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *t = gtk_label_new("ZClassic23");
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "title");
        lbl_sync = gtk_label_new("Connecting...");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sync), "sync");
        gtk_box_pack_start(GTK_BOX(hdr), t, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hdr), lbl_sync, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), hdr, FALSE, FALSE, 0);
    }

    /* Balance */
    lbl_balance = gtk_label_new("Loading...");
    gtk_widget_set_halign(lbl_balance, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_balance), "balance");
    gtk_box_pack_start(GTK_BOX(vbox), lbl_balance, FALSE, FALSE, 16);

    /* Action buttons */
    {
        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *btn_recv = gtk_button_new_with_label("Receive");
        GtkWidget *btn_send = gtk_button_new_with_label("Send");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_recv), "action-btn");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send), "action-btn");
        g_signal_connect(btn_recv, "clicked", G_CALLBACK(on_receive_clicked), NULL);
        g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(btns), btn_recv, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(btns), btn_send, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), btns, FALSE, FALSE, 0);
    }

    /* ── Receive panel (hidden by default) ── */
    recv_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(recv_panel), 12);
    {
        lbl_address = gtk_label_new("Loading...");
        gtk_label_set_selectable(GTK_LABEL(lbl_address), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(lbl_address), TRUE);
        gtk_widget_set_halign(lbl_address, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_address), "addr");
        gtk_box_pack_start(GTK_BOX(recv_panel), lbl_address, FALSE, FALSE, 0);

        GtkWidget *copy_btn = gtk_button_new_with_label("Copy Address");
        gtk_widget_set_halign(copy_btn, GTK_ALIGN_START);
        g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_address), NULL);
        gtk_box_pack_start(GTK_BOX(recv_panel), copy_btn, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(recv_panel, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), recv_panel, FALSE, FALSE, 8);

    /* ── Send panel (hidden by default) ── */
    send_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(send_panel), 12);
    {
        GtkWidget *to_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(to_box), gtk_label_new("To:"), FALSE, FALSE, 0);
        entry_to = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_to), "t1... address");
        gtk_box_pack_start(GTK_BOX(to_box), entry_to, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(send_panel), to_box, FALSE, FALSE, 0);

        GtkWidget *amt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(amt_box), gtk_label_new("Amount:"), FALSE, FALSE, 0);
        entry_amount = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_amount), "0.00");
        gtk_box_pack_start(GTK_BOX(amt_box), entry_amount, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(amt_box), gtk_label_new("ZCL"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(send_panel), amt_box, FALSE, FALSE, 0);

        GtkWidget *send_btn = gtk_button_new_with_label("Send");
        gtk_widget_set_halign(send_btn, GTK_ALIGN_START);
        g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_submit), NULL);
        gtk_box_pack_start(GTK_BOX(send_panel), send_btn, FALSE, FALSE, 0);

        lbl_send_status = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_send_status), "dim");
        gtk_box_pack_start(GTK_BOX(send_panel), lbl_send_status, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(send_panel, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), send_panel, FALSE, FALSE, 8);

    /* Separator */
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 12);

    /* Activity label */
    {
        GtkWidget *act = gtk_label_new("Recent Activity");
        gtk_widget_set_halign(act, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(act), "title");
        gtk_box_pack_start(GTK_BOX(vbox), act, FALSE, FALSE, 4);
    }

    /* UTXO list (scrollable) */
    {
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        utxo_list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(utxo_list),
            GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(scroll), utxo_list);
        gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    }

    /* Status bar */
    lbl_status = gtk_label_new("");
    gtk_widget_set_halign(lbl_status, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status");
    gtk_box_pack_end(GTK_BOX(vbox), lbl_status, FALSE, FALSE, 4);

    gtk_widget_show_all(win);
    start_query();
    g_timeout_add(500, update_ui, NULL);
    gtk_main();
    return 0;
}

#else

#include <stdio.h>
int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    (void)argc; (void)argv; (void)datadir;
    fprintf(stderr, "GUI not available — built without GTK3.\n"
                    "Install gtk3-devel and rebuild.\n");
    return 1;
}

#endif
