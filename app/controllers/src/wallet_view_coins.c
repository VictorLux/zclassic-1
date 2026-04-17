/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/wallet_view_internal.h"
#include "controllers/wallet_controller.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

/* ── Coins (/wallet/coins) — Full UTXO audit view ──────────── */

size_t serve_coins(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();
    if (!db) {
        size_t off = wv_emit_header(r, max, "Coins — ZClassic23", "/wallet/coins");
        off += template_render(TMPL_LOADING, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    int tip = wv_effective_tip(db);
    size_t off = wv_emit_header(r, max, "Coins — ZClassic23", "/wallet/coins");

    /* Pre-render UTXO rows into buffer */
    char utxo_rows[16384];
    size_t ur = 0;
    int64_t t_total = 0;
    int t_count = 0;
    sqlite3_stmt *s = NULL;
    const char *coins_sql =
        "SELECT hex(wu.txid), wu.vout, wu.value, wu.height, "
        "  CASE WHEN wu.is_coinbase THEN 'Coinbase' ELSE 'Standard' END "
        "FROM wallet_utxos wu "
        "WHERE wu.spent_txid IS NULL "
        "ORDER BY wu.value DESC";
    if (sqlite3_prepare_v2(db, coins_sql, -1, &s, NULL) == SQLITE_OK) {
        while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && ur + 500 < sizeof(utxo_rows)) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int vout = sqlite3_column_int(s, 1);
            int64_t val = sqlite3_column_int64(s, 2);
            int h = sqlite3_column_int(s, 3);
            const char *stype = (const char *)sqlite3_column_text(s, 4);
            if (!txid) continue;

            char short_tx[18], lower_tx[65];
            wv_txid_short(txid, short_tx, sizeof(short_tx));
            wv_txid_lower(txid, lower_tx, sizeof(lower_tx));

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            int n = snprintf(utxo_rows + ur, sizeof(utxo_rows) - ur,
                "<tr>"
                "<td><a href='/explorer/tx/%s' class='hash'>"
                "%s:%d</a></td>"
                "<td><span class='pill pill-%s' style='font-size:13px'>"
                "%s</span></td>"
                "<td class='zcl'>%.8f</td>",
                lower_tx, short_tx, vout,
                stype && stype[0] == 'C' ? "pending" : "t",
                stype ? stype : "Standard",
                (double)val / 1e8);
            if (n > 0) ur += (size_t)n;
            if (h > 0)
                n = snprintf(utxo_rows + ur, sizeof(utxo_rows) - ur,
                    "<td>%d</td><td>%d</td>", h, confs);
            else
                n = snprintf(utxo_rows + ur, sizeof(utxo_rows) - ur,
                    "<td><span class='pill pill-pending'>Pending</span></td>"
                    "<td><span class='pill pill-pending'>Pending</span></td>");
            if (n > 0) ur += (size_t)n;
            n = snprintf(utxo_rows + ur, sizeof(utxo_rows) - ur, "</tr>");
            if (n > 0) ur += (size_t)n;
            t_total += val;
            t_count++;
        }
        sqlite3_finalize(s);
    }
    utxo_rows[ur] = '\0';

    /* Shielded notes */
    int z_notes = 0;
    int64_t z_total = wv_query_shielded_balance(db, &z_notes);

    /* Pre-render notes section */
    char notes_section[8192];
    size_t ns = 0;
    if (z_notes > 0) {
        char note_rows[6144];
        size_t nr = 0;
        s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT n.value, n.address, COUNT(*) as cnt, "
                "  MIN(n.block_height) as min_h, MAX(n.block_height) as max_h "
                "FROM wallet_sapling_notes n"
                " WHERE NOT EXISTS ("
                "   SELECT 1 FROM sapling_spends ss"
                "   WHERE ss.nullifier = n.nullifier)"
                " GROUP BY n.value, n.address"
                " ORDER BY n.value DESC",
                -1, &s, NULL) == SQLITE_OK) {
            while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && nr + 400 < sizeof(note_rows)) {
                int64_t val = sqlite3_column_int64(s, 0);
                const char *addr = (const char *)sqlite3_column_text(s, 1);
                int cnt = sqlite3_column_int(s, 2);
                int min_h = sqlite3_column_int(s, 3);
                int max_h = sqlite3_column_int(s, 4);
                char short_addr[18] = "\xe2\x80\x94";
                if (addr && addr[0] && strlen(addr) > 3)
                    wv_txid_short(addr + 3, short_addr, sizeof(short_addr));
                int n = snprintf(note_rows + nr, sizeof(note_rows) - nr,
                    "<tr>"
                    "<td class='zcl'>%.8f</td>"
                    "<td class='hash' style='color:#a78bfa'>zs1%s</td>"
                    "<td>%d</td>",
                    (double)val / 1e8, short_addr, cnt);
                if (n > 0) nr += (size_t)n;
                if (min_h > 0) {
                    if (min_h == max_h)
                        n = snprintf(note_rows + nr, sizeof(note_rows) - nr,
                            "<td>%d</td>", min_h);
                    else
                        n = snprintf(note_rows + nr, sizeof(note_rows) - nr,
                            "<td>%d\xe2\x80\x93%d</td>", min_h, max_h);
                } else {
                    n = snprintf(note_rows + nr, sizeof(note_rows) - nr,
                        "<td><span class='pill pill-pending'>Pending</span></td>");
                }
                if (n > 0) nr += (size_t)n;
                n = snprintf(note_rows + nr, sizeof(note_rows) - nr, "</tr>");
                if (n > 0) nr += (size_t)n;
            }
            sqlite3_finalize(s);
        }
        note_rows[nr] = '\0';

        char z_total_s[32], z_notes_s[16];
        snprintf(z_total_s, sizeof(z_total_s), "%.8f", (double)z_total / 1e8);
        snprintf(z_notes_s, sizeof(z_notes_s), "%d", z_notes);
        struct template_var nv[] = {
            { "note_rows", note_rows },
            { "z_total",   z_total_s },
            { "z_notes",   z_notes_s },
            { "z_plural",  z_notes == 1 ? "" : "s" },
        };
        ns = template_render(TMPL_COINS_NOTES_TABLE, nv, 4,
            notes_section, sizeof(notes_section));
    } else {
        int sapling_keys = wv_query_int(db,
            "SELECT count(*) FROM wallet_sapling_keys");
        char sk_s[16];
        snprintf(sk_s, sizeof(sk_s), "%d", sapling_keys);
        struct template_var nv[] = { { "sapling_keys", sk_s } };
        ns = template_render(TMPL_COINS_NO_NOTES, nv, 1,
            notes_section, sizeof(notes_section));
    }
    notes_section[ns] = '\0';

    /* Pre-render token section */
    char token_section[8192];
    size_t tks = 0;
    {
        sqlite3_stmt *tok = NULL;
        int token_count = 0;
        char token_rows[6144];
        size_t tr_off = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(t.token_id), t.ticker, t.name, t.decimals, "
                "  SUM(tr.amount) as balance "
                "FROM zslp_tokens t "
                "JOIN zslp_transfers tr ON tr.token_id = t.token_id "
                "WHERE tr.to_addr IN (SELECT pubkey_hash FROM wallet_keys) "
                "  AND tr.tx_type IN ('GENESIS','MINT','SEND') "
                "GROUP BY t.token_id "
                "HAVING balance > 0 "
                "ORDER BY balance DESC LIMIT 50",
                -1, &tok, NULL) == SQLITE_OK) {
            while (AR_STEP_ROW_READONLY(tok) == SQLITE_ROW) {
                const char *ticker = (const char *)sqlite3_column_text(tok, 1);
                const char *name = (const char *)sqlite3_column_text(tok, 2);
                int decimals = sqlite3_column_int(tok, 3);
                int64_t bal = sqlite3_column_int64(tok, 4);
                char esc_ticker[64] = "", esc_name[128] = "";
                if (ticker) html_escape(esc_ticker, sizeof(esc_ticker), ticker);
                if (name) html_escape(esc_name, sizeof(esc_name), name);
                double divisor = 1.0;
                for (int d = 0; d < decimals; d++) divisor *= 10.0;
                double disp = (double)bal / divisor;
                int n = snprintf(token_rows + tr_off,
                    sizeof(token_rows) - tr_off,
                    "<tr><td><span class='pill pill-z'>%s</span></td>"
                    "<td>%s</td>"
                    "<td class='zcl' style='color:#a78bfa'>%.*f</td></tr>",
                    esc_ticker[0] ? esc_ticker : "\xe2\x80\x94",
                    esc_name[0] ? esc_name : "\xe2\x80\x94",
                    decimals, disp);
                if (n > 0) tr_off += (size_t)n;
                token_count++;
            }
            sqlite3_finalize(tok);
        }
        token_rows[tr_off] = '\0';

        if (token_count > 0) {
            struct template_var tv[] = { { "token_rows", token_rows } };
            tks = template_render(TMPL_COINS_TOKENS, tv, 1,
                token_section, sizeof(token_section));
        } else {
            tks = template_render(TMPL_COINS_NO_TOKENS, NULL, 0,
                token_section, sizeof(token_section));
        }
    }
    token_section[tks] = '\0';

    /* Format numeric strings for template */
    char t_total_s[32], z_total_s[32], grand_s[32];
    char t_count_s[16], speed_bal_s[32], speed_utxos_s[16];
    char chain_supply_s[32], chain_utxos_s[16];
    int64_t grand = t_total + z_total;
    snprintf(t_total_s, sizeof(t_total_s), "%.8f", (double)t_total / 1e8);
    snprintf(z_total_s, sizeof(z_total_s), "%.8f", (double)z_total / 1e8);
    snprintf(grand_s, sizeof(grand_s), "%.8f", (double)grand / 1e8);
    snprintf(t_count_s, sizeof(t_count_s), "%d", t_count);

    int64_t speed_bal = wv_query_speed_balance(db);
    int speed_utxo_count = wv_query_int(db,
        "SELECT count(*) FROM wallet_utxos WHERE spent_txid IS NULL");
    snprintf(speed_bal_s, sizeof(speed_bal_s), "%.8f",
        (double)speed_bal / 1e8);
    snprintf(speed_utxos_s, sizeof(speed_utxos_s), "%d", speed_utxo_count);

    int64_t chain_supply = wv_query_int64(db,
        "SELECT COALESCE(SUM(value),0) FROM utxos");
    int chain_utxo_count = wv_query_int(db, "SELECT count(*) FROM utxos");
    snprintf(chain_supply_s, sizeof(chain_supply_s), "%.2f",
        (double)chain_supply / 1e8);
    snprintf(chain_utxos_s, sizeof(chain_utxos_s), "%d", chain_utxo_count);

    struct template_var vars[] = {
        { "parent_href",   "/wallet/node" },
        { "parent_label",  "Node" },
        { "current",       "Coin Audit" },
        { "utxo_rows",     utxo_rows },
        { "t_count",       t_count_s },
        { "t_plural",      t_count == 1 ? "" : "s" },
        { "t_total",       t_total_s },
        { "notes_section", notes_section },
        { "z_total",       z_total_s },
        { "grand_total",   grand_s },
        { "speed_bal",     speed_bal_s },
        { "speed_utxos",   speed_utxos_s },
        { "diag_status",   (speed_bal == t_total)
            ? "<span class='pill pill-t'>match</span>"
            : "<span class='pill pill-send'>stale</span>" },
        { "token_section", token_section },
        { "chain_supply",  chain_supply_s },
        { "chain_utxos",   chain_utxos_s },
    };
    off += template_render(TMPL_COINS_PAGE, vars,
        sizeof(vars) / sizeof(vars[0]), (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Shield (/wallet/shield?amount=X) ──────────────────────── */
/* One-click fund securing page.
 * The wallet auto-generates a private address and builds the transaction.
 * User just confirms the amount. */
