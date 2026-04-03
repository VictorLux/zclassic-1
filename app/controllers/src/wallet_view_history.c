/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/wallet_view_internal.h"
#include "controllers/wallet_controller.h"

enum history_filter_mode {
    HISTORY_FILTER_ALL = 0,
    HISTORY_FILTER_SENT = 1,
    HISTORY_FILTER_RECV = 2,
};

static enum history_filter_mode history_filter_parse(const char *filter)
{
    if (filter && strcmp(filter, "sent") == 0)
        return HISTORY_FILTER_SENT;
    if (filter && strcmp(filter, "recv") == 0)
        return HISTORY_FILTER_RECV;
    return HISTORY_FILTER_ALL;
}

static const char *history_filter_name(enum history_filter_mode mode)
{
    switch (mode) {
    case HISTORY_FILTER_SENT: return "sent";
    case HISTORY_FILTER_RECV: return "recv";
    default: return "all";
    }
}

static void history_bind_filter_params(sqlite3_stmt *s, int start_index,
                                       enum history_filter_mode mode,
                                       const char *search_hex)
{
    int restrict_mode = mode == HISTORY_FILTER_ALL ? 0 : 1;
    int from_me = mode == HISTORY_FILTER_SENT ? 1 : 0;
    const char *search = (search_hex && search_hex[0]) ? search_hex : "";

    sqlite3_bind_int(s, start_index + 0, restrict_mode);
    sqlite3_bind_int(s, start_index + 1, from_me);
    sqlite3_bind_text(s, start_index + 2, search, -1, SQLITE_STATIC);
}

static int history_query_count(sqlite3 *db, enum history_filter_mode mode,
                               const char *search_hex)
{
    static const char *sql =
        "SELECT count(*) FROM wallet_transactions wt "
        "WHERE (?1 = 0 OR wt.from_me = ?2) "
        "AND (wt.from_me = 1 OR EXISTS ("
        "  SELECT 1 FROM wallet_utxos wu "
        "  WHERE wu.txid = wt.txid AND wu.value > 0"
        ")) "
        "AND (?3 = '' OR hex(wt.txid) LIKE '%' || ?3 || '%')";
    sqlite3_stmt *s = NULL;
    int count = 0;

    if (!db)
        return 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s)
        return 0;
    history_bind_filter_params(s, 1, mode, search_hex);
    if (sqlite3_step(s) == SQLITE_ROW)
        count = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return count;
}

/* ── History (/wallet/history) ──────────────────────────────── */

size_t serve_history(uint8_t *r, size_t max, int page,
                            const char *filter, const char *search) {
    sqlite3 *db = wv_open_db();
    if (!db) {
        size_t off = wv_emit_header(r, max, "History — ZClassic23", "/wallet/history");
        off += template_render(TMPL_LOADING, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    int tip = wv_effective_tip(db);
    int per_page = 50;

    size_t off = wv_emit_header(r, max, "History — ZClassic23", "/wallet/history");

    enum history_filter_mode filter_mode = history_filter_parse(filter);

    /* Search by txid prefix */
    char safe_search[65] = "";
    if (search && search[0]) {
        size_t si = 0;
        for (size_t i = 0; search[i] && si < 64; i++) {
            char c = search[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))
                safe_search[si++] = c;
        }
        safe_search[si] = '\0';
    }

    int tx_count = history_query_count(db, filter_mode, safe_search);

    int total_pages = (tx_count + per_page - 1) / per_page;
    if (page >= total_pages && total_pages > 0) page = total_pages - 1;

    /* Filter tabs with counts */
    const char *f = history_filter_name(filter_mode);
    int c_all = history_query_count(db, HISTORY_FILTER_ALL, NULL);
    int c_sent = history_query_count(db, HISTORY_FILTER_SENT, NULL);
    int c_recv = history_query_count(db, HISTORY_FILTER_RECV, NULL);
    {
        char ca[16], cs[16], cr[16], cnt[16], pg[16], pgs[16];
        snprintf(ca, sizeof(ca), "%d", c_all);
        snprintf(cs, sizeof(cs), "%d", c_sent);
        snprintf(cr, sizeof(cr), "%d", c_recv);
        snprintf(cnt, sizeof(cnt), "%d", tx_count);
        snprintf(pg, sizeof(pg), "%d", page + 1);
        snprintf(pgs, sizeof(pgs), "%d", total_pages > 0 ? total_pages : 1);
        bool is_all = (strcmp(f, "all") == 0 || !filter);
        struct template_var hv[] = {
            { "all_active",   is_all ? "active" : "" },
            { "sent_active",  strcmp(f, "sent") == 0 ? "active" : "" },
            { "recv_active",  strcmp(f, "recv") == 0 ? "active" : "" },
            { "c_all",        ca },
            { "c_sent",       cs },
            { "c_recv",       cr },
            { "search",       safe_search },
            { "filter",       f },
            { "count",        cnt },
            { "count_plural", tx_count == 1 ? "" : "s" },
            { "page",         pg },
            { "pages",        pgs },
        };
        off += template_render(TMPL_HISTORY_HEADER, hv, 12,
            (char *)r + off, max - off);
    }

    /* Timeline view (tx-cards).
     * Use from_me to determine send vs receive.
     * Compute net value from wallet UTXOs for this txid. */
    sqlite3_stmt *s = NULL;
    static const char *history_sql =
        "SELECT hex(wt.txid), "
        /* Use UTXO height as fallback when wallet_transactions.block_height=0 */
        "COALESCE(NULLIF(wt.block_height,0),"
        "  (SELECT MAX(wu0.height) FROM wallet_utxos wu0 WHERE wu0.txid = wt.txid),"
        "  0) as ht, "
        "COALESCE(b.time,"
        "  (SELECT b2.time FROM blocks b2 WHERE b2.height = "
        "    (SELECT MAX(wu0b.height) FROM wallet_utxos wu0b WHERE wu0b.txid = wt.txid)),"
        "  0), "
        "wt.from_me, wt.fee, "
        /* Outputs to our wallet (received or change) */
        "COALESCE("
        "  (SELECT SUM(wu.value) FROM wallet_utxos wu WHERE wu.txid = wt.txid),"
        "  (SELECT SUM(o.value) FROM tx_outputs o "
        "    WHERE o.txid = wt.txid AND o.address_hash IN "
        "    (SELECT pubkey_hash FROM wallet_keys)),"
        "  0), "
        /* Inputs from our wallet (spent by this tx) */
        "COALESCE("
        "  (SELECT SUM(wu2.value) FROM wallet_utxos wu2 "
        "    WHERE wu2.spent_txid = wt.txid), 0) "
        "FROM wallet_transactions wt "
        "LEFT JOIN blocks b ON COALESCE(NULLIF(wt.block_height,0),"
        "  (SELECT MAX(wu0c.height) FROM wallet_utxos wu0c WHERE wu0c.txid = wt.txid)) "
        "  = b.height "
        "WHERE (?1 = 0 OR wt.from_me = ?2) "
        "AND (wt.from_me = 1 OR EXISTS ("
        "  SELECT 1 FROM wallet_utxos wu "
        "  WHERE wu.txid = wt.txid AND wu.value > 0"
        ")) "
        "AND (?3 = '' OR hex(wt.txid) LIKE '%' || ?3 || '%') "
        "ORDER BY ht DESC LIMIT ?4 OFFSET ?5";
    if (sqlite3_prepare_v2(db, history_sql, -1, &s, NULL) == SQLITE_OK) {
        history_bind_filter_params(s, 1, filter_mode, safe_search);
        sqlite3_bind_int(s, 4, per_page);
        sqlite3_bind_int(s, 5, page * per_page);
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int from_me = sqlite3_column_int(s, 3);
            int64_t fee = sqlite3_column_int64(s, 4);
            int64_t wallet_output = sqlite3_column_int64(s, 5);
            int64_t wallet_input = sqlite3_column_int64(s, 6);
            if (!txid) continue;
            /* Skip ghost entries (no data AND not a send we initiated) */
            if (wallet_output == 0 && wallet_input == 0 && h == 0 && !from_me)
                continue;

            bool is_recv = (from_me == 0);
            int64_t display_val;
            if (is_recv) {
                /* Received: show wallet outputs (amount received) */
                display_val = wallet_output;
            } else if (wallet_input > 0) {
                /* Sent: amount = inputs - change_outputs (includes fee) */
                display_val = wallet_input - wallet_output;
                if (display_val < 0) display_val = 0;
            } else {
                display_val = wallet_output;
            }
            /* Shield operations: from_me with no transparent output = secured */
            bool is_shield_op = (from_me && wallet_output == 0 &&
                                  wallet_input == 0 && display_val == 0);
            /* Skip zero-value entries (change outputs, empty notes) */
            if (display_val == 0 && !is_shield_op) continue;

            char short_tx[18], lower_tx[65], rel_time[48], ts[32];
            wv_txid_short(txid, short_tx, sizeof(short_tx));
            wv_txid_lower(txid, lower_tx, sizeof(lower_tx));
            wv_format_relative_time(btime, rel_time, sizeof(rel_time));
            wv_format_time(btime, ts, sizeof(ts));

            char esc_short[64], esc_lower[256], esc_rel[96], esc_ts[64];
            html_escape(esc_short, sizeof(esc_short), short_tx);
            html_escape(esc_lower, sizeof(esc_lower), lower_tx);
            html_escape(esc_rel, sizeof(esc_rel), rel_time);
            html_escape(esc_ts, sizeof(esc_ts), ts);

            int confs = (tip > 0 && h > 0) ? (tip - h + 1) : 0;
            if (confs < 0) confs = 0;

            /* Format height with commas */
            char h_fmt[20];
            {
                char tmp[20];
                int tl = snprintf(tmp, sizeof(tmp), "%d", h);
                int ci = 0, ti = 0;
                int digits_left = tl;
                for (int di = 0; di < tl && ci < (int)sizeof(h_fmt)-1; di++) {
                    h_fmt[ci++] = tmp[ti++];
                    digits_left--;
                    if (digits_left > 0 && digits_left % 3 == 0)
                        h_fmt[ci++] = ',';
                }
                h_fmt[ci] = '\0';
            }

            /* Build confirmation HTML snippet */
            char conf_html[256] = "";
            if (h > 0) {
                char confs_s[16];
                snprintf(confs_s, sizeof(confs_s), "%d", confs);
                struct template_var cv[] = {
                    { "block", h_fmt }, { "confs", confs_s }
                };
                template_render(TMPL_CONF_CONFIRMED, cv, 2,
                    conf_html, sizeof(conf_html));
            } else {
                template_render(TMPL_CONF_PENDING, NULL, 0,
                    conf_html, sizeof(conf_html));
            }

            /* Render card — use short format, better labels */
            char amt_s[32];
            zcl_format_zcl_short(amt_s, sizeof(amt_s), display_val);

            if (is_shield_op) {
                /* Show fee if known, otherwise just label */
                char fee_s[32] = "";
                if (fee > 0)
                    zcl_format_zcl_short(fee_s, sizeof(fee_s), fee);

                struct template_var sv[] = {
                    { "txid",      esc_lower },
                    { "fee",       fee_s },
                    { "timestamp", esc_ts },
                    { "rel_time",  esc_rel },
                };
                off += template_render(TMPL_HISTORY_SHIELD, sv, 4,
                    (char *)r + off, max - off);
            } else {
                struct template_var tv[] = {
                    { "txid",        esc_lower },
                    { "color",       is_recv ? "#34d399" : "#f87171" },
                    { "amount_class", is_recv ? "recv" : "send" },
                    { "sign",        is_recv ? "+" : "-" },
                    { "amount",      amt_s },
                    { "pill_class",  is_recv ? "pill-t" : "pill-send" },
                    { "pill_label",  is_recv ? "Received" : "Sent" },
                    { "timestamp",   esc_ts },
                    { "rel_time",    esc_rel },
                    { "conf_html",   conf_html },
                };
                off += template_render(TMPL_HISTORY_CARD, tv, 10,
                    (char *)r + off, max - off);
            }
        }
        sqlite3_finalize(s);
    }

    /* Shielded note activity — show recent note deposits */
    if (page == 0 && (strcmp(f, "all") == 0 || strcmp(f, "recv") == 0)) {
        sqlite3_stmt *ns = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT n.value, n.block_height, n.address, "
                "COALESCE(b.time, 0) "
                "FROM wallet_sapling_notes n "
                "LEFT JOIN blocks b ON n.block_height = b.height "
                "WHERE NOT EXISTS ("
                "  SELECT 1 FROM sapling_spends ss "
                "  WHERE ss.nullifier = n.nullifier) "
                "AND n.value > 0 "
                "ORDER BY n.block_height DESC LIMIT 10",
                -1, &ns, NULL) == SQLITE_OK) {

            bool header_shown = false;
            while (sqlite3_step(ns) == SQLITE_ROW && off + 600 < max) {
                int64_t val = sqlite3_column_int64(ns, 0);
                (void)sqlite3_column_int(ns, 1); /* block height */
                const char *addr = (const char *)sqlite3_column_text(ns, 2);
                int64_t ntime = sqlite3_column_int64(ns, 3);

                if (!header_shown) {
                    APPEND(off, r, max,
                        "<div class='section-header' style='margin-top:16px'>"
                        "<span>&#x1F512; Shielded Notes</span></div>");
                    header_shown = true;
                }

                char amt_s[32];
                zcl_format_zcl_short(amt_s, sizeof(amt_s), val);
                char rel[48], esc_rel[96];
                wv_format_relative_time(ntime, rel, sizeof(rel));
                html_escape(esc_rel, sizeof(esc_rel), rel);

                /* Short z-address label */
                char addr_short[24] = "";
                if (addr && strlen(addr) > 12)
                    snprintf(addr_short, sizeof(addr_short),
                        "%.8s...%.4s", addr, addr + strlen(addr) - 4);

                /* Is this one of my addresses? (all z-addresses in wallet are mine) */
                bool is_mine = (addr && addr[0]);

                APPEND(off, r, max,
                    "<div class='tx-card' style='border-left-color:#a78bfa'>"
                    "<div style='display:flex;justify-content:space-between;"
                    "align-items:baseline'>"
                    "<span class='tx-amount recv'>+%s ZCL</span>"
                    "<span class='pill pill-z'>%s</span></div>"
                    "<div class='tx-meta'>"
                    "<span style='color:#888;font-size:13px'>%s</span>"
                    "<span class='tx-time'>%s</span>"
                    "</div></div>",
                    amt_s,
                    is_mine ? "my z-addr" : "external",
                    addr_short,
                    esc_rel);
            }
            sqlite3_finalize(ns);
        }
    }

    /* Pagination (preserve filter + search) */
    if (total_pages > 1) {
        char newer[256] = "", older[256] = "";
        const char *qs = safe_search[0] ? safe_search : "";
        const char *qp = safe_search[0] ? "&amp;q=" : "";
        if (page > 0)
            snprintf(newer, sizeof(newer),
                "<a href='/wallet/history?page=%d&amp;filter=%s%s%s'>"
                "&larr; Newer</a>", page - 1, f, qp, qs);
        if (page < total_pages - 1)
            snprintf(older, sizeof(older),
                "<a href='/wallet/history?page=%d&amp;filter=%s%s%s'>"
                "Older &rarr;</a>", page + 1, f, qp, qs);
        struct template_var pv[] = {
            { "newer_link", newer },
            { "older_link", older },
        };
        off += template_render(TMPL_PAGINATION, pv, 2,
            (char *)r + off, max - off);
    }

    wv_emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Coins (/wallet/coins) — Full UTXO audit view ──────────── */

/* ── Transaction Detail (/wallet/tx/:txid) ──────────────────── */

size_t serve_tx_detail(uint8_t *r, size_t max, const char *txid_hex) {
    sqlite3 *db = wv_open_db();
    if (!db) {
        size_t off = wv_emit_header(r, max, "Transaction — ZClassic23", "/wallet/history");
        off += template_render(TMPL_LOADING, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    int tip = wv_effective_tip(db);
    size_t off = wv_emit_header(r, max, "Transaction — ZClassic23", "/wallet/history");

    /* Sanitize txid: only hex chars, max 64 */
    char safe_txid[65] = "";
    {
        size_t si = 0;
        for (size_t i = 0; txid_hex && txid_hex[i] && si < 64; i++) {
            char c = txid_hex[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))
                safe_txid[si++] = c;
        }
        safe_txid[si] = '\0';
    }

    if (strlen(safe_txid) < 64) {
        off += template_render(TMPL_TX_INVALID, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        sqlite3_close(db);
        return off;
    }

    /* Convert to uppercase for BLOB comparison */
    char upper_txid[65];
    for (int i = 0; i < 64; i++)
        upper_txid[i] = (safe_txid[i] >= 'a' && safe_txid[i] <= 'f')
            ? (char)(safe_txid[i] - 32) : safe_txid[i];
    upper_txid[64] = '\0';

    /* Lookup wallet transaction */
    int block_height = 0, from_me = 0;
    int64_t fee = 0, btime = 0;

    sqlite3_stmt *s = NULL;
    const char *tx_lookup_sql =
        "SELECT wt.block_height, wt.from_me, wt.fee, "
        "COALESCE(b.time, 0) "
        "FROM wallet_transactions wt "
        "LEFT JOIN blocks b ON wt.block_height = b.height "
        "WHERE hex(wt.txid) = ?";
    bool found = false;
    if (sqlite3_prepare_v2(db, tx_lookup_sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, upper_txid, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            block_height = sqlite3_column_int(s, 0);
            from_me = sqlite3_column_int(s, 1);
            fee = sqlite3_column_int64(s, 2);
            btime = sqlite3_column_int64(s, 3);
            found = true;
        }
        sqlite3_finalize(s);
    }

    if (!found) {
        off += template_render(TMPL_TX_NOT_FOUND, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        sqlite3_close(db);
        return off;
    }

    int confs = (tip > 0 && block_height > 0) ? (tip - block_height + 1) : 0;
    if (confs < 0) confs = 0;
    bool is_recv = (from_me == 0);
    int conf_pct = confs >= 6 ? 100 : (confs * 100 / 6);

    char rel_time[48], abs_time[32];
    wv_format_relative_time(btime, rel_time, sizeof(rel_time));
    wv_format_time(btime, abs_time, sizeof(abs_time));

    char esc_rel[96], esc_abs[64];
    html_escape(esc_rel, sizeof(esc_rel), rel_time);
    html_escape(esc_abs, sizeof(esc_abs), abs_time);

    /* Fee row (only for outgoing with fee) */
    char fee_row[128] = "";
    if (fee > 0 && !is_recv)
        snprintf(fee_row, sizeof(fee_row),
            "<div class='lbl'>Fee</div>"
            "<div class='val zcl'>%.8f ZCL</div>",
            (double)fee / 1e8);

    /* Pre-render wallet outputs */
    char outputs_section[4096];
    size_t os = 0;
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT vout, value, hex(address_hash) "
            "FROM wallet_utxos WHERE hex(txid) = ? "
            "ORDER BY vout",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, upper_txid, -1, SQLITE_STATIC);
        bool header_shown = false;
        while (sqlite3_step(s) == SQLITE_ROW && os + 300 < sizeof(outputs_section)) {
            if (!header_shown) {
                int n = snprintf(outputs_section + os,
                    sizeof(outputs_section) - os,
                    "<h3>Wallet Outputs</h3>");
                if (n > 0) os += (size_t)n;
                header_shown = true;
            }
            int vout = sqlite3_column_int(s, 0);
            int64_t val = sqlite3_column_int64(s, 1);
            int n = snprintf(outputs_section + os,
                sizeof(outputs_section) - os,
                "<div class='utxo-row'>"
                "<span class='mono' style='color:#888'>:%d</span>"
                "<span class='zcl'>%.8f ZCL</span>"
                "</div>",
                vout, (double)val / 1e8);
            if (n > 0) os += (size_t)n;
        }
        sqlite3_finalize(s);
    }
    outputs_section[os] = '\0';

    /* Format template variables */
    char confs_s[16], conf_pct_s[8], block_h_s[16];
    snprintf(confs_s, sizeof(confs_s), "%d", confs);
    snprintf(conf_pct_s, sizeof(conf_pct_s), "%d", conf_pct);
    snprintf(block_h_s, sizeof(block_h_s), "%d", block_height);

    struct template_var vars[] = {
        { "parent_href",     "/wallet/history" },
        { "parent_label",    "History" },
        { "current",         "Transaction" },
        { "pill_class",      is_recv ? "pill-t" : "pill-pending" },
        { "direction",       is_recv ? "Received" : "Sent" },
        { "color",           is_recv ? "#34d399" : "#f87171" },
        { "heading",         is_recv ? "Incoming Transaction"
                                     : "Outgoing Transaction" },
        { "rel_time",        esc_rel },
        { "abs_time",        esc_abs },
        { "confs",           confs_s },
        { "conf_plural",     confs == 1 ? "" : "s" },
        { "conf_status",     confs >= 6 ? "Confirmed" : "Pending" },
        { "conf_pct",        conf_pct_s },
        { "conf_color",      confs >= 6 ? "#34d399"
                             : confs >= 1 ? "#fbbf24" : "#f87171" },
        { "txid",            safe_txid },
        { "block_height",    block_h_s },
        { "fee_row",         fee_row },
        { "outputs_section", outputs_section },
    };
    off += template_render(TMPL_TX_DETAIL, vars,
        sizeof(vars) / sizeof(vars[0]), (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

/* ── Node / Command Center (/wallet/node) ───────────────────── */
