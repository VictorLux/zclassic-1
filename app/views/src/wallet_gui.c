/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic23 Wallet GUI
 *
 * Design: Single screen. Balance always visible. Receive/Send are
 * inline toggle panels. Activity shows human-readable history.
 * All data from SQLite via background thread — GTK never blocks.
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
#include <time.h>
#include "keys/key_io.h"

static char g_db_path[600] = "";

/* ── Data ──────────────────────────────────────────────── */

struct activity_row {
    int64_t value;
    int     height;
    int64_t block_time;
};
#define MAX_ACTIVITY 50

static struct {
    _Atomic bool ready;
    bool db_ok;
    int64_t transparent;
    int64_t shielded;
    int64_t chain_height;
    int64_t latest_block_time;
    char    address[128];
    struct activity_row activity[MAX_ACTIVITY];
    int     num_activity;
} g_d = {0};

/* ── Helpers ───────────────────────────────────────────── */

static int64_t qry(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

static void fmt_zcl(char *buf, size_t max, int64_t z, int decimals)
{
    int64_t whole = z / 100000000;
    int64_t frac = z % 100000000;
    if (frac < 0) frac = -frac;
    if (decimals == 3) {
        int64_t f3 = frac / 100000;
        snprintf(buf, max, "%" PRId64 ".%03" PRId64, whole, f3);
    } else {
        snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
        size_t len = strlen(buf);
        while (len > 2 && buf[len-1] == '0' && buf[len-2] != '.') len--;
        buf[len] = 0;
    }
}

static void fmt_ago(char *buf, size_t max, int64_t block_time)
{
    if (block_time <= 0) { snprintf(buf, max, "—"); return; }
    int64_t delta = (int64_t)time(NULL) - block_time;
    if (delta < 0) delta = 0;
    if (delta < 60) snprintf(buf, max, "just now");
    else if (delta < 3600) snprintf(buf, max, "%lld min ago", (long long)(delta/60));
    else if (delta < 86400) snprintf(buf, max, "%lld hours ago", (long long)(delta/3600));
    else snprintf(buf, max, "%lld days ago", (long long)(delta/86400));
}

/* ── Background thread ─────────────────────────────────── */

static void *bg_thread(void *arg)
{
    (void)arg;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READONLY, NULL)
        != SQLITE_OK || !db) {
        g_d.db_ok = false;
        atomic_store(&g_d.ready, true);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 200);
    g_d.db_ok = true;

    g_d.transparent = qry(db,
        "SELECT COALESCE(SUM(u.value),0) FROM utxos u"
        " INNER JOIN wallet_keys w ON u.address_hash=w.pubkey_hash");
    g_d.shielded = qry(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
        " WHERE spent_txid IS NULL");
    g_d.chain_height = qry(db,
        "SELECT COALESCE(MAX(height),0) FROM blocks WHERE status>=3");
    g_d.latest_block_time = qry(db,
        "SELECT COALESCE(MAX(time),0) FROM blocks WHERE status>=3");

    /* Address */
    g_d.address[0] = '\0';
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT pubkey_hash FROM wallet_keys LIMIT 1",
                -1, &s, NULL) == SQLITE_OK && s) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                const void *pkh = sqlite3_column_blob(s, 0);
                if (pkh && sqlite3_column_bytes(s, 0) == 20) {
                    struct tx_destination dest;
                    dest.type = DEST_KEY_ID;
                    memcpy(dest.id.key.id.data, pkh, 20);
                    const unsigned char pk[] = {0x1C, 0xB8};
                    const unsigned char sc[] = {0x1C, 0xBD};
                    encode_destination(&dest, pk, 2, sc, 2,
                                       g_d.address, sizeof(g_d.address));
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* Activity */
    g_d.num_activity = 0;
    {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT u.value, u.height, COALESCE(b.time,0) "
                "FROM utxos u INNER JOIN wallet_keys w "
                "ON u.address_hash=w.pubkey_hash "
                "LEFT JOIN blocks b ON b.height=u.height "
                "ORDER BY u.height DESC LIMIT ?",
                -1, &s, NULL) == SQLITE_OK && s) {
            sqlite3_bind_int(s, 1, MAX_ACTIVITY);
            while (sqlite3_step(s) == SQLITE_ROW &&
                   g_d.num_activity < MAX_ACTIVITY) {
                struct activity_row *r = &g_d.activity[g_d.num_activity++];
                r->value = sqlite3_column_int64(s, 0);
                r->height = sqlite3_column_int(s, 1);
                r->block_time = sqlite3_column_int64(s, 2);
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_close(db);
    atomic_store(&g_d.ready, true);
    return NULL;
}

static void start_bg(void)
{
    atomic_store(&g_d.ready, false);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, bg_thread, NULL);
    pthread_attr_destroy(&a);
}

/* ── Widgets ───────────────────────────────────────────── */

static GtkWidget *lbl_balance;
static GtkWidget *lbl_transparent, *lbl_shielded;
static GtkWidget *lbl_sync;
static GtkWidget *recv_panel, *lbl_addr, *btn_copy;
static GtkWidget *send_panel, *entry_to, *entry_amt, *lbl_send_msg;
static GtkWidget *activity_box;
static GtkWidget *lbl_status;

static void on_recv(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gtk_widget_set_visible(send_panel, FALSE);
    gtk_widget_set_visible(recv_panel, !gtk_widget_get_visible(recv_panel));
    if (gtk_widget_get_visible(recv_panel) && g_d.address[0])
        gtk_label_set_text(GTK_LABEL(lbl_addr), g_d.address);
}

static void on_copy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    GtkClipboard *c = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(c, g_d.address, -1);
    gtk_button_set_label(GTK_BUTTON(btn_copy), "✓ Copied");
    /* Reset label after 2s would need a timeout — keep it simple */
}

static void on_send_toggle(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gtk_widget_set_visible(recv_panel, FALSE);
    gtk_widget_set_visible(send_panel, !gtk_widget_get_visible(send_panel));
}

static void on_send_submit(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gtk_label_set_text(GTK_LABEL(lbl_send_msg),
        "Sending requires a running node (./zclassic23 -daemon)");
}

/* ── Update loop ───────────────────────────────────────── */

static gboolean tick(gpointer d)
{
    (void)d;
    if (!atomic_load(&g_d.ready)) return G_SOURCE_CONTINUE;

    if (!g_d.db_ok) {
        gtk_label_set_text(GTK_LABEL(lbl_balance), "No wallet");
        gtk_label_set_text(GTK_LABEL(lbl_status),
            g_db_path[0] ? g_db_path : "node.db not found");
        start_bg();
        return G_SOURCE_CONTINUE;
    }

    char buf[96];

    /* Balance — 3 decimal places for scanning */
    fmt_zcl(buf, sizeof(buf), g_d.transparent + g_d.shielded, 3);
    char bal[96];
    snprintf(bal, sizeof(bal), "%s ZCL", buf);
    gtk_label_set_text(GTK_LABEL(lbl_balance), bal);

    /* Full precision tooltip */
    fmt_zcl(buf, sizeof(buf), g_d.transparent + g_d.shielded, 8);
    snprintf(bal, sizeof(bal), "%s ZCL (exact)", buf);
    gtk_widget_set_tooltip_text(lbl_balance, bal);

    /* Transparent / Shielded sub-balances */
    fmt_zcl(buf, sizeof(buf), g_d.transparent, 3);
    snprintf(bal, sizeof(bal), "%s ZCL", buf);
    gtk_label_set_text(GTK_LABEL(lbl_transparent), bal);

    if (g_d.shielded > 0) {
        fmt_zcl(buf, sizeof(buf), g_d.shielded, 3);
        snprintf(bal, sizeof(bal), "%s ZCL", buf);
    } else {
        snprintf(bal, sizeof(bal), "—");
    }
    gtk_label_set_text(GTK_LABEL(lbl_shielded), bal);

    /* Sync indicator */
    int64_t block_age = (int64_t)time(NULL) - g_d.latest_block_time;
    if (g_d.latest_block_time == 0)
        snprintf(buf, sizeof(buf), "● No blocks");
    else if (block_age < 300)
        snprintf(buf, sizeof(buf), "● Synced");
    else if (block_age < 3600)
        snprintf(buf, sizeof(buf), "● %lld min behind", (long long)(block_age/60));
    else
        snprintf(buf, sizeof(buf), "● %lld hours behind", (long long)(block_age/3600));
    gtk_label_set_text(GTK_LABEL(lbl_sync), buf);

    /* Activity list */
    {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(activity_box));
        for (GList *l = ch; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(ch);

        if (g_d.num_activity == 0) {
            GtkWidget *empty = gtk_label_new("No transactions yet");
            gtk_style_context_add_class(
                gtk_widget_get_style_context(empty), "muted");
            gtk_container_add(GTK_CONTAINER(activity_box), empty);
        }

        for (int i = 0; i < g_d.num_activity; i++) {
            struct activity_row *r = &g_d.activity[i];

            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_container_set_border_width(GTK_CONTAINER(row), 8);

            /* Line 1: amount + relative time */
            GtkWidget *line1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            char amt[64];
            fmt_zcl(amt, sizeof(amt), r->value, 3);
            char amt_str[80];
            snprintf(amt_str, sizeof(amt_str), "↓  Received %s ZCL", amt);
            GtkWidget *la = gtk_label_new(amt_str);
            gtk_widget_set_halign(la, GTK_ALIGN_START);
            gtk_style_context_add_class(gtk_widget_get_style_context(la), "received");

            char ago[32];
            fmt_ago(ago, sizeof(ago), r->block_time);
            GtkWidget *lt = gtk_label_new(ago);
            gtk_widget_set_halign(lt, GTK_ALIGN_END);
            gtk_style_context_add_class(gtk_widget_get_style_context(lt), "muted");

            gtk_box_pack_start(GTK_BOX(line1), la, TRUE, TRUE, 0);
            gtk_box_pack_end(GTK_BOX(line1), lt, FALSE, FALSE, 0);

            /* Line 2: block height + confirmed */
            char detail[64];
            snprintf(detail, sizeof(detail),
                     "Block %d · confirmed", r->height);
            GtkWidget *ld = gtk_label_new(detail);
            gtk_widget_set_halign(ld, GTK_ALIGN_START);
            gtk_style_context_add_class(gtk_widget_get_style_context(ld), "detail");

            gtk_box_pack_start(GTK_BOX(row), line1, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), ld, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(activity_box), row);
        }
        gtk_widget_show_all(activity_box);
    }

    /* Status bar */
    snprintf(buf, sizeof(buf), "Block %" PRId64, g_d.chain_height);
    gtk_label_set_text(GTK_LABEL(lbl_status), buf);

    start_bg();
    return G_SOURCE_CONTINUE;
}

/* ── Build UI ──────────────────────────────────────────── */

int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    snprintf(g_db_path, sizeof(g_db_path), "%s/node.db", datadir);
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "Cannot open display.\n");
        return 1;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: #0d1117; }"
        "label { color: #e6edf3; font-family: 'Inter','Segoe UI',sans-serif; }"
        ".bal { font-size: 42px; font-weight: 700; color: #e6edf3; }"
        ".sub-val { font-size: 16px; font-weight: 500; color: #e6edf3; }"
        ".sub-label { font-size: 12px; color: #8b949e; }"
        ".sync-ok { font-size: 12px; color: #3fb950; }"
        ".sync-warn { font-size: 12px; color: #d29922; }"
        ".hdr { font-size: 14px; font-weight: 600; color: #8b949e; }"
        ".muted { font-size: 12px; color: #484f58; }"
        ".detail { font-size: 11px; color: #30363d; }"
        ".received { font-size: 14px; font-weight: 600; color: #3fb950; }"
        ".addr { font-size: 15px; font-family: monospace; color: #58a6ff; }"
        ".status { font-size: 11px; color: #484f58; font-family: monospace; }"
        "separator { background-color: #21262d; min-height: 1px; }"
        "button { background: #21262d; color: #c9d1d9; border: 1px solid #30363d;"
        "  border-radius: 6px; padding: 8px 24px; font-size: 14px; }"
        "button:hover { background: #30363d; }"
        "entry { background: #0d1117; color: #c9d1d9; border: 1px solid #30363d;"
        "  border-radius: 6px; padding: 6px 12px; font-family: monospace; }"
        "list { background-color: #0d1117; }"
        "list row { background-color: #0d1117; padding: 0; }"
        "list row:hover { background-color: #161b22; }"
        , -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ZClassic23 Wallet");
    gtk_window_set_default_size(GTK_WINDOW(win), 480, 640);
    gtk_container_set_border_width(GTK_CONTAINER(win), 32);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), root);

    /* ── Header ── */
    {
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *t = gtk_label_new("ZClassic23");
        gtk_style_context_add_class(gtk_widget_get_style_context(t), "hdr");
        lbl_sync = gtk_label_new("● Connecting...");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sync), "sync-ok");
        gtk_box_pack_start(GTK_BOX(h), t, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(h), lbl_sync, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), h, FALSE, FALSE, 0);
    }

    /* ── Balance (centered) ── */
    lbl_balance = gtk_label_new("—");
    gtk_widget_set_halign(lbl_balance, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_balance), "bal");
    gtk_box_pack_start(GTK_BOX(root), lbl_balance, FALSE, FALSE, 20);

    /* ── Transparent / Shielded ── */
    {
        GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 40);
        gtk_widget_set_halign(cols, GTK_ALIGN_CENTER);

        GtkWidget *lcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *tl = gtk_label_new("Transparent");
        gtk_style_context_add_class(gtk_widget_get_style_context(tl), "sub-label");
        lbl_transparent = gtk_label_new("—");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_transparent), "sub-val");
        gtk_box_pack_start(GTK_BOX(lcol), tl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(lcol), lbl_transparent, FALSE, FALSE, 0);

        GtkWidget *rcol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *sl = gtk_label_new("Shielded");
        gtk_style_context_add_class(gtk_widget_get_style_context(sl), "sub-label");
        lbl_shielded = gtk_label_new("—");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_shielded), "sub-val");
        gtk_box_pack_start(GTK_BOX(rcol), sl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rcol), lbl_shielded, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(cols), lcol, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cols), rcol, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), cols, FALSE, FALSE, 8);
    }

    /* ── Buttons ── */
    {
        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
        GtkWidget *br = gtk_button_new_with_label("Receive");
        GtkWidget *bs = gtk_button_new_with_label("Send");
        g_signal_connect(br, "clicked", G_CALLBACK(on_recv), NULL);
        g_signal_connect(bs, "clicked", G_CALLBACK(on_send_toggle), NULL);
        gtk_box_pack_start(GTK_BOX(btns), br, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(btns), bs, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), btns, FALSE, FALSE, 16);
    }

    /* ── Receive panel ── */
    recv_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(recv_panel), 16);
    {
        GtkWidget *rl = gtk_label_new("Your Address");
        gtk_style_context_add_class(gtk_widget_get_style_context(rl), "sub-label");
        gtk_widget_set_halign(rl, GTK_ALIGN_START);
        lbl_addr = gtk_label_new("Loading...");
        gtk_label_set_selectable(GTK_LABEL(lbl_addr), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(lbl_addr), TRUE);
        gtk_widget_set_halign(lbl_addr, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_addr), "addr");
        btn_copy = gtk_button_new_with_label("Copy");
        gtk_widget_set_halign(btn_copy, GTK_ALIGN_START);
        g_signal_connect(btn_copy, "clicked", G_CALLBACK(on_copy), NULL);
        gtk_box_pack_start(GTK_BOX(recv_panel), rl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(recv_panel), lbl_addr, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(recv_panel), btn_copy, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(recv_panel, TRUE);
    gtk_box_pack_start(GTK_BOX(root), recv_panel, FALSE, FALSE, 0);

    /* ── Send panel ── */
    send_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(send_panel), 16);
    {
        entry_to = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_to), "t1... recipient address");
        GtkWidget *ab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        entry_amt = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry_amt), "0.00");
        GtkWidget *zl = gtk_label_new("ZCL");
        gtk_style_context_add_class(gtk_widget_get_style_context(zl), "sub-label");
        gtk_box_pack_start(GTK_BOX(ab), entry_amt, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(ab), zl, FALSE, FALSE, 0);
        GtkWidget *sb = gtk_button_new_with_label("Send");
        gtk_widget_set_halign(sb, GTK_ALIGN_START);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_send_submit), NULL);
        lbl_send_msg = gtk_label_new("");
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_send_msg), "muted");
        gtk_widget_set_halign(lbl_send_msg, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(send_panel), entry_to, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(send_panel), ab, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(send_panel), sb, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(send_panel), lbl_send_msg, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(send_panel, TRUE);
    gtk_box_pack_start(GTK_BOX(root), send_panel, FALSE, FALSE, 0);

    /* ── Separator ── */
    gtk_box_pack_start(GTK_BOX(root),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 12);

    /* ── Activity ── */
    {
        GtkWidget *al = gtk_label_new("Activity");
        gtk_widget_set_halign(al, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(al), "hdr");
        gtk_box_pack_start(GTK_BOX(root), al, FALSE, FALSE, 4);
    }
    {
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        activity_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(activity_box),
            GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(sw), activity_box);
        gtk_box_pack_start(GTK_BOX(root), sw, TRUE, TRUE, 0);
    }

    /* ── Status ── */
    lbl_status = gtk_label_new("");
    gtk_widget_set_halign(lbl_status, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status");
    gtk_box_pack_end(GTK_BOX(root), lbl_status, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    start_bg();
    g_timeout_add(1000, tick, NULL);
    gtk_main();
    return 0;
}

#else
#include <stdio.h>
int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    (void)argc; (void)argv; (void)datadir;
    fprintf(stderr, "GUI not available — built without GTK3.\n");
    return 1;
}
#endif
