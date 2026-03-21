/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZClassic wallet GUI — reads directly from SQLite. No RPC, no daemon.
 * Background thread queries DB so GTK main loop never blocks.
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

/* ── Two-layer wallet state ────────────────────────────────
 *
 * Batch layer: the fully indexed chain at some height N.
 *   Transparent = SUM(utxos) for our wallet_keys at height N
 *   This is a deterministic function of the immutable chain.
 *
 * Speed layer: wallet_utxos / wallet_sapling_notes track
 *   real-time changes from connect_block. These may be ahead
 *   of the batch layer during IBD.
 *
 * Display = MAX(batch, speed) with honest "as of height" label.
 * ──────────────────────────────────────────────────────────── */

struct wallet_data {
    _Atomic bool ready;

    /* Batch layer (from global UTXO set — deterministic) */
    int64_t batch_transparent;
    int64_t batch_height;

    /* Speed layer (from wallet-specific tables — may be ahead) */
    int64_t speed_transparent;
    int64_t speed_shielded;
    int64_t speed_height;

    /* Display values (best of both layers) */
    int64_t transparent;
    int64_t shielded;
    int64_t total;
    int64_t display_height;

    bool shielded_unverified;

    /* Chain stats */
    int64_t chain_height;
    int64_t utxo_count;
    int64_t addr_count;
    int64_t tx_count;

    bool db_open;
};

static struct wallet_data g_data = {0};

/* Forward declarations */
static GtkWidget *make_label(const char *text, const char *css_class,
                             bool selectable);
static void format_zcl(char *buf, size_t max, int64_t zatoshi);

/* ── Wallet data for history/receive ─────────────────────── */

static char g_wallet_address[128] = "";

struct tx_entry {
    char txid[65];
    int64_t amount;
    int height;
    char address[64];
};
#define MAX_TX_DISPLAY 50
static struct tx_entry g_tx_history[MAX_TX_DISPLAY];
static int g_tx_count = 0;

/* ── Background query thread ──────────────────────────────── */

static int64_t bg_query(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t val = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            val = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return val;
}

static void *query_thread(void *arg)
{
    (void)arg;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(g_db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK || !db) {
        g_data.db_open = false;
        atomic_store(&g_data.ready, true);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 200);

    g_data.db_open = true;
    g_data.shielded_unverified = false;

    /* ── Batch layer: deterministic from global UTXO set ── */
    g_data.batch_transparent = bg_query(db,
        "SELECT COALESCE(SUM(u.value),0) FROM utxos u"
        " INNER JOIN wallet_keys w ON u.address_hash=w.pubkey_hash");
    /* Batch height = max height in the UTXO set */
    g_data.batch_height = bg_query(db,
        "SELECT COALESCE(MAX(height),0) FROM utxos");

    /* ── Speed layer: wallet-tracked state (may be ahead) ── */
    g_data.speed_transparent = bg_query(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_utxos"
        " WHERE spent_txid IS NULL");
    g_data.speed_shielded = bg_query(db,
        "SELECT COALESCE(SUM(value),0) FROM wallet_sapling_notes"
        " WHERE spent_txid IS NULL");
    /* Speed height = height of latest wallet transaction */
    g_data.speed_height = bg_query(db,
        "SELECT COALESCE(MAX(block_height),0) FROM wallet_transactions"
        " WHERE block_height IS NOT NULL");

    /* ── Resolve: use whichever layer is more current ── */
    if (g_data.batch_height >= g_data.speed_height &&
        g_data.batch_height > 0) {
        /* Batch layer is current — use deterministic data */
        g_data.transparent = g_data.batch_transparent;
        g_data.display_height = g_data.batch_height;
    } else {
        /* Speed layer is ahead (IBD not caught up yet) */
        g_data.transparent = g_data.speed_transparent;
        g_data.display_height = g_data.speed_height;
    }

    /* Shielded: derive from chain when possible.
     * A note is spent if its nullifier appears in sapling_spends.
     * Only count notes whose nullifier is NOT on chain. */
    int64_t chain_verified_shielded = bg_query(db,
        "SELECT COALESCE(SUM(n.value),0) FROM wallet_sapling_notes n"
        " WHERE NOT EXISTS ("
        "   SELECT 1 FROM sapling_spends ss"
        "   WHERE ss.nullifier = n.nullifier)");

    /* If chain-verified shielded < cache shielded, notes were spent
     * on chain. If they're equal, nullifiers might not match (derivation
     * bug) — in that case, only trust shielded when fully synced. */
    bool chain_covers_notes = (g_data.chain_height > 0 &&
        g_data.display_height >= g_data.chain_height - 10);

    if (chain_covers_notes && chain_verified_shielded < g_data.speed_shielded) {
        /* Chain says some notes are spent — use chain data */
        g_data.shielded = chain_verified_shielded;
    } else if (chain_covers_notes) {
        /* Fully synced — trust the verified number */
        g_data.shielded = chain_verified_shielded;
    } else {
        /* Not synced — show shielded as 0 with note about sync */
        g_data.shielded = 0;
        g_data.shielded_unverified = true;
    }

    g_data.total = g_data.transparent + g_data.shielded;

    /* ── Chain stats ── */
    g_data.chain_height = bg_query(db,
        "SELECT COALESCE(MAX(height),0) FROM blocks WHERE status>=3");
    g_data.utxo_count = bg_query(db, "SELECT COUNT(*) FROM utxos");
    g_data.addr_count = bg_query(db, "SELECT COUNT(*) FROM addresses");
    g_data.tx_count = bg_query(db, "SELECT COUNT(*) FROM transactions");

    /* ── Wallet address (first key → base58check via encode_destination) ── */
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
                    const unsigned char pk_pfx[] = {0x1C, 0xB8};
                    const unsigned char sc_pfx[] = {0x1C, 0xBD};
                    encode_destination(&dest, pk_pfx, 2, sc_pfx, 2,
                                       g_wallet_address,
                                       sizeof(g_wallet_address));
                }
            }
            sqlite3_finalize(s);
        }
    }

    /* ── Recent transactions (from wallet_utxos as proxy) ── */
    g_tx_count = 0;
    {
        sqlite3_stmt *s = NULL;
        /* wallet_transactions may be empty if not scanned.
         * Fall back to wallet_utxos which are populated from global UTXO join. */
        const char *sql =
            "SELECT hex(txid), value, height, '' "
            "FROM wallet_utxos "
            "ORDER BY height DESC LIMIT ?";
        /* Try wallet_transactions first */
        int64_t wtx_count = bg_query(db,
            "SELECT COUNT(*) FROM wallet_transactions");
        if (wtx_count > 0)
            sql = "SELECT hex(txid), amount, block_height, '' "
                  "FROM wallet_transactions "
                  "ORDER BY block_height DESC LIMIT ?";

        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && s) {
            sqlite3_bind_int(s, 1, MAX_TX_DISPLAY);
            while (sqlite3_step(s) == SQLITE_ROW &&
                   g_tx_count < MAX_TX_DISPLAY) {
                struct tx_entry *e = &g_tx_history[g_tx_count];
                const char *txid = (const char *)sqlite3_column_text(s, 0);
                if (txid)
                    snprintf(e->txid, sizeof(e->txid), "%s", txid);
                else
                    e->txid[0] = '\0';
                e->amount = sqlite3_column_int64(s, 1);
                e->height = sqlite3_column_int(s, 2);
                e->address[0] = '\0';
                g_tx_count++;
            }
            sqlite3_finalize(s);
        }
    }

    sqlite3_close(db);
    atomic_store(&g_data.ready, true);
    return NULL;
}

static void start_bg_query(void)
{
    atomic_store(&g_data.ready, false);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, query_thread, NULL);
    pthread_attr_destroy(&attr);
}

/* ── Format helpers ───────────────────────────────────────── */

static void format_zcl(char *buf, size_t max, int64_t zatoshi)
{
    int64_t whole = zatoshi / 100000000;
    int64_t frac = zatoshi % 100000000;
    if (frac < 0) frac = -frac;
    snprintf(buf, max, "%" PRId64 ".%08" PRId64, whole, frac);
    size_t len = strlen(buf);
    while (len > 2 && buf[len-1] == '0' && buf[len-2] != '.') len--;
    buf[len] = 0;
}

/* ── GTK widgets ──────────────────────────────────────────── */

static GtkWidget *lbl_transparent;
static GtkWidget *lbl_shielded;
static GtkWidget *lbl_total;
static GtkWidget *lbl_height;
static GtkWidget *lbl_utxos;
static GtkWidget *lbl_status;
static GtkWidget *lbl_addresses;
static GtkWidget *lbl_txcount;

/* Receive tab */
static GtkWidget *lbl_recv_address;

/* Transaction history */
static GtkWidget *tx_list_box;

static gboolean update_display(gpointer data)
{
    (void)data;

    if (!atomic_load(&g_data.ready)) {
        /* Query still running — don't block */
        return G_SOURCE_CONTINUE;
    }

    if (!g_data.db_open) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "No database");
        start_bg_query(); /* retry */
        return G_SOURCE_CONTINUE;
    }

    char buf[80];

    format_zcl(buf, sizeof(buf), g_data.transparent);
    strcat(buf, " ZCL");
    gtk_label_set_text(GTK_LABEL(lbl_transparent), buf);

    if (g_data.shielded_unverified) {
        gtk_label_set_text(GTK_LABEL(lbl_shielded), "— (sync to verify)");
    } else {
        format_zcl(buf, sizeof(buf), g_data.shielded);
        strcat(buf, " ZCL");
        gtk_label_set_text(GTK_LABEL(lbl_shielded), buf);
    }

    format_zcl(buf, sizeof(buf), g_data.total);
    strcat(buf, " ZCL");
    gtk_label_set_text(GTK_LABEL(lbl_total), buf);

    /* Honest status: show exactly what height the balance reflects */
    bool fully_synced = (g_data.display_height >= g_data.chain_height - 1 &&
                         g_data.chain_height > 0);
    int64_t behind = g_data.chain_height - g_data.display_height;

    if (fully_synced) {
        snprintf(buf, sizeof(buf), "Block %" PRId64, g_data.chain_height);
        gtk_label_set_text(GTK_LABEL(lbl_height), buf);
        gtk_label_set_text(GTK_LABEL(lbl_status), "Synced");
    } else {
        snprintf(buf, sizeof(buf), "Block %" PRId64 " (%" PRId64 " behind)",
                 g_data.display_height, behind);
        gtk_label_set_text(GTK_LABEL(lbl_height), buf);
        snprintf(buf, sizeof(buf), "Balance as of %" PRId64,
                 g_data.display_height);
        gtk_label_set_text(GTK_LABEL(lbl_status), buf);
    }

    snprintf(buf, sizeof(buf), "UTXOs: %" PRId64, g_data.utxo_count);
    gtk_label_set_text(GTK_LABEL(lbl_utxos), buf);

    snprintf(buf, sizeof(buf), "Addrs: %" PRId64, g_data.addr_count);
    gtk_label_set_text(GTK_LABEL(lbl_addresses), buf);

    snprintf(buf, sizeof(buf), "Txns: %" PRId64, g_data.tx_count);
    gtk_label_set_text(GTK_LABEL(lbl_txcount), buf);

    /* Update receive address */
    if (g_wallet_address[0])
        gtk_label_set_text(GTK_LABEL(lbl_recv_address), g_wallet_address);
    else
        gtk_label_set_text(GTK_LABEL(lbl_recv_address), "(no address — run node first)");

    /* Update transaction history */
    {
        /* Clear existing rows */
        GList *children = gtk_container_get_children(GTK_CONTAINER(tx_list_box));
        for (GList *l = children; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);

        for (int i = 0; i < g_tx_count; i++) {
            struct tx_entry *e = &g_tx_history[i];
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(row), 4);

            /* Amount (green for received, red for sent) */
            char amt[32];
            format_zcl(amt, sizeof(amt), e->amount < 0 ? -e->amount : e->amount);
            char amt_str[48];
            snprintf(amt_str, sizeof(amt_str), "%s%s ZCL",
                     e->amount >= 0 ? "+" : "-", amt);
            GtkWidget *lbl_amt = make_label(amt_str, "balance", false);
            gtk_widget_set_size_request(lbl_amt, 160, -1);

            /* Height */
            char ht[32];
            snprintf(ht, sizeof(ht), "h=%d", e->height);
            GtkWidget *lbl_ht = make_label(ht, "status", false);

            /* Txid (truncated) */
            char txid_short[20];
            snprintf(txid_short, sizeof(txid_short), "%.16s...", e->txid);
            GtkWidget *lbl_tx = make_label(txid_short, "status", true);

            gtk_box_pack_start(GTK_BOX(row), lbl_amt, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), lbl_ht, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), lbl_tx, TRUE, TRUE, 0);

            gtk_container_add(GTK_CONTAINER(tx_list_box), row);
        }
        gtk_widget_show_all(tx_list_box);
    }

    /* Launch next background query */
    start_bg_query();

    return G_SOURCE_CONTINUE;
}

/* ── GTK construction ─────────────────────────────────────── */

static GtkWidget *make_label(const char *text, const char *css_class,
                             bool selectable)
{
    GtkWidget *lbl = gtk_label_new(text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(lbl), selectable);
    if (css_class) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(lbl);
        gtk_style_context_add_class(ctx, css_class);
    }
    return lbl;
}

static GtkWidget *make_row(const char *label_text, GtkWidget **value_label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *lbl = make_label(label_text, "dim-label", false);
    gtk_widget_set_size_request(lbl, 130, -1);
    *value_label = make_label("—", "balance", true);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), *value_label, TRUE, TRUE, 0);
    return box;
}

int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    snprintf(g_db_path, sizeof(g_db_path), "%s/node.db", datadir);
    fprintf(stderr, "ZClassic23 Wallet — %s\n", g_db_path);

    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "Cannot open display.\n");
        return 1;
    }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: #1a1a2e; }"
        "label { color: #e0e0e0; font-family: monospace; }"
        "label.title { font-size: 28px; font-weight: bold; color: #33ff99; }"
        "label.balance { font-size: 18px; font-weight: bold; color: #ffffff; }"
        "label.dim-label { font-size: 14px; color: #888888; }"
        "label.status { font-size: 11px; color: #555555; }"
        "label.total { font-size: 24px; font-weight: bold; color: #33ff99; }"
        "separator { background-color: #333355; min-height: 1px; }"
        "notebook tab { padding: 8px 16px; color: #888888; }"
        "notebook tab:checked { color: #33ff99; }"
        "list { background-color: #1a1a2e; }"
        "list row { background-color: #1a1a2e; border-bottom: 1px solid #222244; }"
        , -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "ZClassic23");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 480);
    gtk_container_set_border_width(GTK_CONTAINER(win), 28);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), outer);

    /* Title bar */
    GtkWidget *title = make_label("ZClassic23", "title", false);
    gtk_box_pack_start(GTK_BOX(outer), title, FALSE, FALSE, 8);

    /* Notebook with tabs */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(outer), notebook, TRUE, TRUE, 0);

    /* ── Tab 1: Balance ── */
    {
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);

        gtk_box_pack_start(GTK_BOX(vbox),
            make_row("Transparent", &lbl_transparent), FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(vbox),
            make_row("Shielded", &lbl_shielded), FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(vbox),
            gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 10);
        GtkWidget *total_row = make_row("Total", &lbl_total);
        gtk_style_context_add_class(
            gtk_widget_get_style_context(lbl_total), "total");
        gtk_box_pack_start(GTK_BOX(vbox), total_row, FALSE, FALSE, 4);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
            gtk_label_new("Balance"));
    }

    /* ── Tab 2: Receive ── */
    {
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 16);

        gtk_box_pack_start(GTK_BOX(vbox),
            make_label("Your Address", "dim-label", false), FALSE, FALSE, 4);
        lbl_recv_address = make_label("Loading...", "balance", true);
        gtk_label_set_line_wrap(GTK_LABEL(lbl_recv_address), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), lbl_recv_address, FALSE, FALSE, 4);

        gtk_box_pack_start(GTK_BOX(vbox),
            make_label("Click address to select, then Ctrl+C to copy",
                        "status", false), FALSE, FALSE, 8);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox,
            gtk_label_new("Receive"));
    }

    /* ── Tab 3: History ── */
    {
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        tx_list_box = gtk_list_box_new();
        gtk_container_add(GTK_CONTAINER(scroll), tx_list_box);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll,
            gtk_label_new("History"));
    }

    /* ── Status bar ── */
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    lbl_height = make_label("Height: —", "status", false);
    lbl_utxos = make_label("UTXOs: —", "status", false);
    lbl_addresses = make_label("Addrs: —", "status", false);
    lbl_txcount = make_label("Txns: —", "status", false);
    lbl_status = make_label("Loading...", "status", false);
    gtk_box_pack_start(GTK_BOX(sb), lbl_height, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), lbl_utxos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), lbl_addresses, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), lbl_txcount, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sb), lbl_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(outer), sb, FALSE, FALSE, 4);

    gtk_widget_show_all(win);
    gtk_window_present(GTK_WINDOW(win));

    /* Start first background query immediately */
    start_bg_query();
    /* Check for results every 500ms — never blocks GTK */
    g_timeout_add(500, update_display, NULL);

    gtk_main();
    return 0;
}

#else

#include <stdio.h>
int wallet_gui_main(int argc, char **argv, const char *datadir)
{
    (void)argc; (void)argv; (void)datadir;
    fprintf(stderr, "GUI not available — built without GTK3.\n"
                    "Use: ./zclassic23 -daemon [options]\n");
    return 1;
}

#endif
