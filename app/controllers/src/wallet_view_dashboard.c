/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/wallet_view_internal.h"
#include "controllers/wallet_controller.h"

/* ── Dashboard (/wallet) ────────────────────────────────────── */

size_t serve_dashboard(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();
    if (!db) {
        size_t off = wv_emit_header(r, max, "ZClassic23 Wallet", "/wallet");
        off += template_render(TMPL_LOADING, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    int tip = wv_effective_tip(db);

    /* Ground-truth transparent balance (P2PKH + P2SH change addresses) */
    int t_utxos = 0;
    int64_t transparent = wv_query_ground_truth_balance(db, &t_utxos);

    /* Shielded: verified notes minus spent nullifiers */
    int z_notes = 0;
    int64_t shielded = wv_query_shielded_balance(db, &z_notes);

    /* If a shield operation is pending, adjust displayed balances.
     * The blockchain hasn't confirmed yet, but the user expects to see
     * the funds moving from public -> private immediately. */
    if (g_shield_pending_amount > 0 && g_shield_pending_since > 0) {
        int64_t pending = g_shield_pending_amount;
        if (pending > transparent) pending = transparent;
        transparent -= pending;
        shielded += pending;
    }

    int64_t total_balance = transparent + shielded;

    const char *sync_raw = sync_state_name(sync_get_state());
    bool synced = (sync_get_state() == SYNC_AT_TIP);

    /* Map internal state names to user-friendly labels */
    const char *sync_label = synced ? "Synced" : "Syncing...";
    if (!synced) {
        if (strstr(sync_raw, "download")) sync_label = "Syncing blocks...";
        else if (strstr(sync_raw, "header")) sync_label = "Syncing headers...";
        else if (strstr(sync_raw, "connect")) sync_label = "Connecting...";
        else if (strstr(sync_raw, "idle")) sync_label = "Ready";
        else if (strstr(sync_raw, "scan")) sync_label = "Scanning...";
    }

    /* Sync badge CSS class */
    const char *sync_class = synced ? "pill-synced" :
        (strstr(sync_raw, "idle") ? "pill-ready" : "pill-syncing");

    /* Format balance with minimal decimals */
    char bal_str[32];
    double bal_f = (double)total_balance / (double)ZATOSHI_PER_ZCL;
    if (total_balance == 0)
        snprintf(bal_str, sizeof(bal_str), "0.00");
    else if (total_balance % 1000000 == 0)
        snprintf(bal_str, sizeof(bal_str), "%.2f", bal_f);
    else if (total_balance % 10000 == 0)
        snprintf(bal_str, sizeof(bal_str), "%.4f", bal_f);
    else
        snprintf(bal_str, sizeof(bal_str), "%.8f", bal_f);

    /* Privacy percentage */
    int pct = 0;
    if (total_balance > 0)
        pct = (int)(100 * shielded / total_balance);
    const char *pct_color = pct == 100 ? "#34d399" :
                            pct >= 50  ? "#a78bfa" :
                            pct > 0    ? "#fbbf24" : "#f87171";
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d", pct);

    /* Breakdown text */
    char breakdown[256] = "";
    if (transparent > 0 && shielded > 0) {
        char t_fmt[32], s_fmt[32];
        zcl_format_zcl(t_fmt, sizeof(t_fmt), transparent);
        zcl_format_zcl(s_fmt, sizeof(s_fmt), shielded);
        snprintf(breakdown, sizeof(breakdown),
            "%s public + %s private", t_fmt, s_fmt);
    } else if (shielded > 0) {
        snprintf(breakdown, sizeof(breakdown),
            "<span style='color:#34d399'>&#x1F512; All funds private</span>");
    } else if (transparent > 0) {
        char t_fmt[32];
        zcl_format_zcl(t_fmt, sizeof(t_fmt), transparent);
        snprintf(breakdown, sizeof(breakdown), "%s public", t_fmt);
    }
    /* Append sync note if still syncing */
    if (!synced && !strstr(sync_raw, "idle")) {
        size_t blen = strlen(breakdown);
        snprintf(breakdown + blen, sizeof(breakdown) - blen,
            "</span></div>"
            "<div class='sync-note'>"
            "Syncing &mdash; balance updating</div>"
            "<div style='display:none'><span>");
    }

    /* Privacy card (nudge / pending / done) */
    char privacy_buf[512] = "";
    {
        int shield_st = wv_shield_check_status();
        if (shield_st == 1) {
            char el_s[16];
            snprintf(el_s, sizeof(el_s), "%d",
                (int)(time(NULL) - g_shield_pending_since));
            struct template_var pv[] = { { "elapsed", el_s } };
            template_render(TMPL_SHIELD_PENDING, pv, 1,
                privacy_buf, sizeof(privacy_buf));
        } else if (shield_st == 2) {
            template_render(TMPL_SHIELD_DONE, NULL, 0,
                privacy_buf, sizeof(privacy_buf));
        } else if (transparent > 0) {
            char t_fmt[32];
            zcl_format_zcl(t_fmt, sizeof(t_fmt), transparent);
            struct template_var pv[] = { { "amount", t_fmt } };
            template_render(TMPL_PRIVACY_NUDGE, pv, 1,
                privacy_buf, sizeof(privacy_buf));
        }
    }

    /* Token cards */
    char token_buf[2048] = "";
    {
        size_t toff = 0;
        sqlite3_stmt *tok = NULL;
        int tok_count = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT t.ticker, t.name, t.decimals, SUM(tr.amount) as bal "
                "FROM zslp_tokens t "
                "JOIN zslp_transfers tr ON tr.token_id = t.token_id "
                "WHERE tr.to_addr IN (SELECT pubkey_hash FROM wallet_keys) "
                "  AND tr.tx_type IN ('GENESIS','MINT','SEND') "
                "GROUP BY t.token_id HAVING bal > 0 "
                "ORDER BY bal DESC LIMIT 5",
                -1, &tok, NULL) == SQLITE_OK) {
            while (sqlite3_step(tok) == SQLITE_ROW &&
                   toff + 300 < sizeof(token_buf)) {
                const char *ticker = (const char *)sqlite3_column_text(tok, 0);
                int decimals = sqlite3_column_int(tok, 2);
                int64_t bal = sqlite3_column_int64(tok, 3);
                if (!ticker || bal <= 0) continue;
                if (tok_count == 0)
                    toff += (size_t)snprintf(token_buf + toff,
                        sizeof(token_buf) - toff,
                        "<div style='margin:12px 0'>"
                        "<div class='section-header'>"
                        "<span>Tokens</span>"
                        "<a href='/wallet/coins'>View all</a></div>");
                double div = 1.0;
                for (int d = 0; d < decimals; d++) div *= 10.0;
                char esc_tk[32];
                html_escape(esc_tk, sizeof(esc_tk), ticker);
                toff += (size_t)snprintf(token_buf + toff,
                    sizeof(token_buf) - toff,
                    "<div class='tx-row'>"
                    "<div><span class='pill pill-z'>%s</span></div>"
                    "<div style='text-align:right;color:#a78bfa;"
                    "font-family:\"JetBrains Mono\",monospace;"
                    "font-weight:700'>%.*f</div></div>",
                    esc_tk, decimals, (double)bal / div);
                tok_count++;
            }
            sqlite3_finalize(tok);
        }
        if (tok_count > 0)
            snprintf(token_buf + toff, sizeof(token_buf) - toff, "</div>");
    }

    /* Recent transactions */
    char tx_buf[4096] = "";
    {
        size_t txoff = 0;
        int tx_shown = 0;
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT hex(wt.txid), wt.block_height, COALESCE(b.time,0), "
                "wt.from_me, "
                "COALESCE("
                "  (SELECT SUM(wu.value) FROM wallet_utxos wu "
                "    WHERE wu.txid = wt.txid),"
                "  (SELECT SUM(o.value) FROM tx_outputs o "
                "    WHERE o.txid = wt.txid AND o.address_hash IN "
                "    (SELECT pubkey_hash FROM wallet_keys)),"
                "  0), "
                "COALESCE("
                "  (SELECT SUM(wu2.value) FROM wallet_utxos wu2 "
                "    WHERE wu2.spent_txid = wt.txid), 0) "
                "FROM wallet_transactions wt "
                "LEFT JOIN blocks b ON wt.block_height = b.height "
                "ORDER BY wt.block_height DESC LIMIT 20",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW &&
                   txoff + 512 < sizeof(tx_buf) && tx_shown < 5) {
                const char *txid = (const char *)sqlite3_column_text(s, 0);
                int height = sqlite3_column_int(s, 1);
                int64_t btime = sqlite3_column_int64(s, 2);
                int from_me = sqlite3_column_int(s, 3);
                int64_t wallet_output = sqlite3_column_int64(s, 4);
                int64_t wallet_input = sqlite3_column_int64(s, 5);
                if (!txid) continue;
                int64_t display_amount = wallet_output;
                if (from_me && wallet_input > 0) {
                    display_amount = wallet_input - wallet_output;
                    if (display_amount < 0) display_amount = 0;
                }
                if (display_amount == 0 && height == 0) continue;
                if (display_amount == 0 && from_me) continue;

                bool is_recv = (from_me == 0);
                char rel_time[48], esc_rel[96];
                wv_format_relative_time(btime, rel_time, sizeof(rel_time));
                html_escape(esc_rel, sizeof(esc_rel), rel_time);
                char lower_tx[65];
                wv_txid_lower(txid, lower_tx, sizeof(lower_tx));
                int confs = (tip > 0 && height > 0) ? (tip - height + 1) : 0;
                char amt[32];
                zcl_format_zcl(amt, sizeof(amt), display_amount);
                char conf_str[32] = "";
                if (confs > 0)
                    snprintf(conf_str, sizeof(conf_str), "%d confs", confs);
                char link[80];
                snprintf(link, sizeof(link), "/wallet/tx/%s", lower_tx);

                struct template_var tv[] = {
                    { "link",            link },
                    { "direction_class", is_recv ? "recv" : "send" },
                    { "sign",            is_recv ? "+" : "-" },
                    { "amount",          amt },
                    { "time_style",      "" },
                    { "time_label",      esc_rel },
                    { "conf_label",      conf_str },
                };
                txoff += template_render(TMPL_TX_ROW, tv, 7,
                    tx_buf + txoff, sizeof(tx_buf) - txoff);
                tx_shown++;
            }
            sqlite3_finalize(s);

            /* Shielded notes */
            if (tx_shown < 5) {
                sqlite3_stmt *zs = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT n.value, n.block_height, n.address "
                        "FROM wallet_sapling_notes n "
                        "WHERE NOT EXISTS ("
                        "  SELECT 1 FROM sapling_spends ss"
                        "  WHERE ss.nullifier = n.nullifier) "
                        "ORDER BY n.block_height DESC LIMIT ?",
                        -1, &zs, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(zs, 1, 5 - tx_shown);
                    while (sqlite3_step(zs) == SQLITE_ROW && tx_shown < 5 &&
                           txoff + 512 < sizeof(tx_buf)) {
                        int64_t val = sqlite3_column_int64(zs, 0);
                        int nh = sqlite3_column_int(zs, 1);
                        int nc = (tip > 0 && nh > 0) ? (tip - nh + 1) : 0;
                        if (nc < 0) nc = 0;
                        char amt[32];
                        zcl_format_zcl(amt, sizeof(amt), val);
                        char nc_str[32];
                        if (nc == 0) snprintf(nc_str, sizeof(nc_str), "Pending");
                        else snprintf(nc_str, sizeof(nc_str), "%d confs", nc);

                        struct template_var tv[] = {
                            { "link",            "/wallet/coins" },
                            { "direction_class", "recv" },
                            { "sign",            "+" },
                            { "amount",          amt },
                            { "time_style",      " style='color:#a78bfa'" },
                            { "time_label",      "&#x1F512; Private" },
                            { "conf_label",      nc_str },
                        };
                        txoff += template_render(TMPL_TX_ROW, tv, 7,
                            tx_buf + txoff, sizeof(tx_buf) - txoff);
                        tx_shown++;
                    }
                    sqlite3_finalize(zs);
                }
            }

            if (tx_shown == 0) {
                snprintf(tx_buf, sizeof(tx_buf),
                    "<div class='empty-state'>%s</div>",
                    total_balance > 0 ? "Transaction history syncing..."
                                      : "No transactions yet");
            }
        }
    }

    /* Backup warning */
    char backup_buf[512] = "";
    if (total_balance > 0 && g_wv_datadir) {
        char bk_path[1024];
        snprintf(bk_path, sizeof(bk_path), "%s/wallet.backup", g_wv_datadir);
        if (access(bk_path, F_OK) != 0) {
            struct template_var bv[] = { { "address", PRIMARY_ADDR } };
            template_render(TMPL_BACKUP_WARNING, bv, 1,
                backup_buf, sizeof(backup_buf));
        }
    }

    /* Node status strip (compact, replaces old Power Node card) */
    int peers = wv_query_int(db, "SELECT count(*) FROM peers");
    bool is_ready = synced || strstr(sync_raw, "idle");
    const char *node_status = synced ? "Synced" :
                              strstr(sync_raw, "idle") ? "Ready" : "Syncing";
    const char *node_color = is_ready ? "#34d399" : "#fbbf24";
    char peers_str[16], height_str[16];
    snprintf(peers_str, sizeof(peers_str), "%d", peers);
    snprintf(height_str, sizeof(height_str), "%d", tip);

    char node_strip[512] = "";
    {
        struct template_var nv[] = {
            { "peers",        peers_str },
            { "height",       height_str },
            { "status",       node_status },
            { "status_color", node_color },
        };
        template_render(TMPL_NODE_STATUS_STRIP, nv, 4,
            node_strip, sizeof(node_strip));
    }

    /* Render full dashboard via TMPL_DASHBOARD */
    size_t off = wv_emit_header(r, max, "ZClassic23 Wallet", "/wallet");

    struct template_var vars[] = {
        { "sync_class",     sync_class },
        { "sync_label",     sync_label },
        { "balance",        bal_str },
        { "pct_color",      pct_color },
        { "pct",            pct_str },
        { "breakdown",      breakdown },
        { "privacy_card",   privacy_buf },
        { "token_cards",    token_buf },
        { "recent_txs",     tx_buf },
        { "backup_warning", backup_buf },
        { "node_strip",     node_strip },
    };
    off += template_render(TMPL_DASHBOARD, vars, 11,
        (char *)r + off, max - off);

    /* Dashboard live-update JS */
    APPEND(off, r, max,
        "<script>"
        "function fmt(z){var v=z/1e8;if(z===0)return'0.00';"
        "if(z%%1000000===0)return v.toFixed(2);"
        "if(z%%10000===0)return v.toFixed(4);return v.toFixed(8);}"
        "window._dashUpdate=function(d){"
        "var b=document.getElementById('bal');"
        "if(b){var n=fmt(d.balance+d.shielded)+' ZCL';"
        "if(b.textContent!==n){b.textContent=n;}}"
        "var s=document.getElementById('sync');"
        "if(s){var st=d.sync==='at_tip'?'Synced':"
        "d.sync==='idle'?'Ready':"
        "d.sync.indexOf('download')>=0?'Syncing blocks...':"
        "d.sync.indexOf('header')>=0?'Syncing headers...':"
        "d.sync.indexOf('connect')>=0?'Connecting...':"
        "d.sync.indexOf('scan')>=0?'Scanning...':"
        "d.sync;"
        "s.textContent=st;"
        "s.className='pill sync-badge '+"
        "(d.sync==='at_tip'?'pill-synced':"
        "d.sync==='idle'?'pill-ready':'pill-syncing');}"
        "var sn=document.querySelector('.sync-note');"
        "if(sn){if(d.sync==='at_tip')sn.style.display='none';"
        "else sn.style.display='';}"
        "var bd=document.getElementById('breakdown');"
        "if(bd){if(d.balance>0&&d.shielded>0)"
        "bd.textContent=fmt(d.balance)+' public + '+fmt(d.shielded)+' private';"
        "else if(d.shielded>0)bd.textContent='All funds private';"
        "else if(d.balance>0)bd.textContent=fmt(d.balance)+' public';"
        "else bd.textContent='';}"
        "var lk=document.getElementById('lock');"
        "if(lk){var tot=d.balance+d.shielded;"
        "var pct=tot>0?Math.round(100*d.shielded/tot):0;"
        "lk.style.width=pct+'%%';"
        "var c=pct===100?'#34d399':pct>=50?'#a78bfa':"
        "pct>0?'#fbbf24':'#f87171';"
        "lk.style.background=c;}"
        "var pm=document.getElementById('privacy-meter');"
        "if(pm){var pl=pm.querySelector('span');"
        "if(pl){var tot2=d.balance+d.shielded;"
        "var p2=tot2>0?Math.round(100*d.shielded/tot2):0;"
        "pl.textContent=p2+'%% private';}}};"
        "</script>");

    /* Pre-fill status bar so it's not blank on load */
    APPEND(off, r, max,
        "<script>document.addEventListener('DOMContentLoaded',function(){"
        "var h=document.getElementById('sb-h');"
        "if(h)h.textContent='Block %d';});</script>", tip);

    wv_emit_footer(r, max, &off);
    sqlite3_close(db);
    return off;
}

static struct {
    int height;
    int64_t balance, shielded, speed_bal;
    int t_utxos, z_notes;
} pulse_cache;

size_t serve_pulse(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();
    int height = 0, peers = 0, mempool = 0;
    int64_t balance = 0, shielded = 0, speed_bal = 0;
    int t_utxos = 0, z_notes = 0;

    /* Check if a pending shield operation completed */
    if (g_shield_opid[0]) {
        int ss = wv_shield_check_status();
        if (ss == 2 || ss == -1)
            g_balance_dirty = 1; /* shield done — force recompute */
    }

    if (db) {
        height = wv_effective_tip(db);
        peers = wv_query_int(db,  "SELECT count(*) FROM peers");
        mempool = wv_query_int(db, "SELECT count(*) FROM mempool_entries");

        if (g_balance_dirty) {
            /* Wallet changed — sync from zclassicd then recompute */
            sqlite3_close(db);
            wv_sync_wallet_from_zclassicd();
            db = wv_open_db();
            if (!db) return 0;
        }

        if (height != pulse_cache.height || pulse_cache.height == 0 ||
            g_balance_dirty) {
            g_balance_dirty = 0;
            /* Recompute balances */
            balance = wv_query_ground_truth_balance(db, &t_utxos);
            shielded = wv_query_shielded_balance(db, &z_notes);
            speed_bal = wv_query_speed_balance(db);

            pulse_cache.height = height;
            pulse_cache.balance = balance;
            pulse_cache.shielded = shielded;
            pulse_cache.speed_bal = speed_bal;
            pulse_cache.t_utxos = t_utxos;
            pulse_cache.z_notes = z_notes;
        } else {
            /* Same height — serve cached balances, only refresh peers/mempool */
            balance = pulse_cache.balance;
            shielded = pulse_cache.shielded;
            speed_bal = pulse_cache.speed_bal;
            t_utxos = pulse_cache.t_utxos;
            z_notes = pulse_cache.z_notes;
        }
        sqlite3_close(db);
    }

    /* Adjust pulse balances for pending shield operation */
    if (g_shield_pending_amount > 0 && g_shield_pending_since > 0) {
        int64_t pending = g_shield_pending_amount;
        if (pending > balance) pending = balance;
        balance -= pending;
        shielded += pending;
    }

    const char *sync = sync_state_name(sync_get_state());

    struct wv_pulse p = {
        .height = height, .balance = balance, .shielded = shielded,
        .speed_balance = speed_bal, .t_utxos = t_utxos, .z_notes = z_notes,
        .peers = peers, .mempool = mempool
    };
    snprintf(p.sync, sizeof(p.sync), "%s", sync ? sync : "idle");
    return wv_render_pulse(r, max, &p);
}

/* ── Send Review (/wallet/send/review POST) ─────────────────── */
/* Intermediate confirmation step: show details before executing. */
