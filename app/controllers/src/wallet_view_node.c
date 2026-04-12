/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/wallet_view_internal.h"
#include "controllers/wallet_controller.h"
#include "util/log_macros.h"

/* ── Node / Command Center (/wallet/node) ───────────────────── */

size_t serve_node(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();

    int tip = 0, peers = 0, mempool = 0, utxo_count = 0;
    int64_t supply = 0;
    if (db) {
        tip = wv_effective_tip(db);
        peers = wv_query_int(db, "SELECT count(*) FROM peers");
        mempool = wv_query_int(db, "SELECT count(*) FROM mempool_entries");
        utxo_count = wv_query_int(db, "SELECT count(*) FROM utxos");
        supply = wv_query_int64(db,
            "SELECT COALESCE(SUM(value),0) FROM utxos");
    }

    const char *sync_raw = sync_state_name(sync_get_state());
    bool synced = (sync_get_state() == SYNC_AT_TIP);
    const char *sync_label = synced ? "Synced" :
        (strstr(sync_raw, "idle") ? "Ready" : "Syncing...");
    const char *sync_class = synced ? "pill-synced" :
        (strstr(sync_raw, "idle") ? "pill-ready" : "pill-syncing");

    char height_s[20];
    if (format_with_commas(height_s, sizeof(height_s), tip) == 0)
        snprintf(height_s, sizeof(height_s), "%d", tip);

    char peers_s[16], mempool_s[16], utxo_s[16], supply_s[32];
    snprintf(peers_s, sizeof(peers_s), "%d", peers);
    snprintf(mempool_s, sizeof(mempool_s), "%d", mempool);
    snprintf(utxo_s, sizeof(utxo_s), "%d", utxo_count);
    snprintf(supply_s, sizeof(supply_s), "%.2f", (double)supply / 1e8);

    /* Difficulty from latest block */
    char diff_s[32] = "\xe2\x80\x94";
    if (db) {
        int64_t bits = wv_query_int64(db,
            "SELECT bits FROM blocks ORDER BY height DESC LIMIT 1");
        if (bits > 0) {
            double diff = explorer_difficulty_from_bits((uint32_t)bits);
            if (diff >= 1e9)
                snprintf(diff_s, sizeof(diff_s), "%.2fG", diff / 1e9);
            else if (diff >= 1e6)
                snprintf(diff_s, sizeof(diff_s), "%.2fM", diff / 1e6);
            else if (diff >= 1e3)
                snprintf(diff_s, sizeof(diff_s), "%.1fK", diff / 1e3);
            else
                snprintf(diff_s, sizeof(diff_s), "%.1f", diff);
        }
    }

    /* Tor .onion address detection */
    char tor_section[2048] = "";
    {
        char onion_path[512];
        const char *datadir = g_wv_datadir ? g_wv_datadir : "";
        snprintf(onion_path, sizeof(onion_path), "%s/onion/hostname", datadir);
        FILE *f = fopen(onion_path, "r");
        if (f) {
            char onion[128] = "";
            if (fgets(onion, sizeof(onion), f)) {
                char *nl = strchr(onion, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
            if (onion[0]) {
                char esc_onion[256];
                html_escape(esc_onion, sizeof(esc_onion), onion);
                struct template_var tv[] = {
                    { "tor_color",  "#34d399" },
                    { "tor_status", "Active" },
                    { "onion_addr", esc_onion },
                };
                template_render(TMPL_NODE_TOR, tv, 3,
                    tor_section, sizeof(tor_section));
            }
        }
        if (!tor_section[0])
            template_render(TMPL_NODE_NO_TOR, NULL, 0,
                tor_section, sizeof(tor_section));
    }

    /* Peer table */
    char peer_table[8192];
    size_t pt = 0;
    {
        int n = snprintf(peer_table, sizeof(peer_table),
            "<div class='overflow-x'>"
            "<table><tr><th>Address</th><th>Dir</th>"
            "<th>Version</th><th>Height</th></tr>");
        if (n > 0) pt = (size_t)n;

        sqlite3_stmt *ps = NULL;
        int peer_shown = 0;
        if (db && sqlite3_prepare_v2(db,
                "SELECT addr, subver, starting_height, inbound "
                "FROM peers ORDER BY starting_height DESC LIMIT 25",
                -1, &ps, NULL) == SQLITE_OK) {
            while (sqlite3_step(ps) == SQLITE_ROW &&
                   pt + 400 < sizeof(peer_table)) {
                const char *addr = (const char *)sqlite3_column_text(ps, 0);
                const char *subver = (const char *)sqlite3_column_text(ps, 1);
                int sh = sqlite3_column_int(ps, 2);
                int inbound = sqlite3_column_int(ps, 3);
                if (!addr) continue;

                char esc_addr[128], esc_sub[64], sh_s[16];
                html_escape(esc_addr, sizeof(esc_addr), addr);
                html_escape(esc_sub, sizeof(esc_sub),
                    subver ? subver : "unknown");
                snprintf(sh_s, sizeof(sh_s), "%d", sh);

                struct template_var tv[] = {
                    { "addr",      esc_addr },
                    { "dir_class", inbound ? "pill-z" : "pill-t" },
                    { "direction", inbound ? "In" : "Out" },
                    { "subver",    esc_sub },
                    { "height",    sh_s },
                };
                pt += template_render(TMPL_NODE_PEER_ROW, tv, 5,
                    peer_table + pt, sizeof(peer_table) - pt);
                peer_shown++;
            }
            sqlite3_finalize(ps);
        }
        if (peer_shown == 0) {
            int n2 = snprintf(peer_table + pt, sizeof(peer_table) - pt,
                "<tr><td colspan='4' style='color:#888;text-align:center;"
                "padding:16px'>Connecting to network...</td></tr>");
            if (n2 > 0) pt += (size_t)n2;
        }
        int n2 = snprintf(peer_table + pt, sizeof(peer_table) - pt,
            "</table></div>");
        if (n2 > 0) pt += (size_t)n2;
    }
    peer_table[pt] = '\0';

    size_t off = wv_emit_header(r, max, "Node — ZClassic23", "/wallet/node");

    struct template_var vars[] = {
        { "height",      height_s },
        { "peers",       peers_s },
        { "mempool",     mempool_s },
        { "difficulty",  diff_s },
        { "sync_class",  sync_class },
        { "sync_label",  sync_label },
        { "tor_section", tor_section },
        { "peer_table",  peer_table },
        { "utxo_count",  utxo_s },
        { "supply",      supply_s },
    };
    off += template_render(TMPL_NODE_PAGE, vars,
        sizeof(vars) / sizeof(vars[0]), (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    if (db) sqlite3_close(db);
    return off;
}

/* ── Router ─────────────────────────────────────────────────── */
