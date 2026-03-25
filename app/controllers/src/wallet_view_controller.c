/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet view controller — MVC HTML views for GTK browser.
 * Reads directly from SQLite. No RPC. No ports.
 *
 * Routes:
 *   /wallet           Dashboard (balance, stats, recent txs)
 *   /wallet/send      Send form with validation
 *   /wallet/receive   Receive addresses with visual encoding
 *   /wallet/history   Full transaction history
 *   /wallet/coins     UTXO and shielded note breakdown */

#include "controllers/wallet_view_internal.h"
#include "controllers/wallet_controller.h"

/* ── Dashboard (/wallet) ────────────────────────────────────── */

static size_t serve_dashboard(uint8_t *r, size_t max) {
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

    /* Power Node stats */
    int peers = wv_query_int(db, "SELECT count(*) FROM peers");
    int mempool = wv_query_int(db, "SELECT count(*) FROM mempool_entries");
    bool is_ready = synced || strstr(sync_raw, "idle");
    const char *node_status = synced ? "Synced" :
                              strstr(sync_raw, "idle") ? "Ready" : "Syncing";
    const char *node_color = is_ready ? "#34d399" : "#fbbf24";
    char peers_str[16], mempool_str[16];
    snprintf(peers_str, sizeof(peers_str), "%d", peers);
    snprintf(mempool_str, sizeof(mempool_str), "%d", mempool);

    /* Render full dashboard via TMPL_DASHBOARD */
    size_t off = wv_emit_header(r, max, "ZClassic23 Wallet", "/wallet");

    struct template_var vars[] = {
        { "sync_class",        sync_class },
        { "sync_label",        sync_label },
        { "balance",           bal_str },
        { "pct_color",         pct_color },
        { "pct",               pct_str },
        { "breakdown",         breakdown },
        { "privacy_card",      privacy_buf },
        { "token_cards",       token_buf },
        { "recent_txs",        tx_buf },
        { "backup_warning",    backup_buf },
        { "peers",             peers_str },
        { "mempool",           mempool_str },
        { "node_status_color", node_color },
        { "node_status",       node_status },
    };
    off += template_render(TMPL_DASHBOARD, vars, 14,
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

/* ── Send (/wallet/send) ────────────────────────────────────── */

static size_t serve_send(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();

    int64_t balance = 0;
    int64_t shielded_bal = 0;
    /* Contacts for autocomplete (max 20) */
    struct { char name[64]; char addr[256]; } contacts[20];
    int n_contacts = 0;
    /* ZSLP tokens held (max 10) */
    struct { char token_id[65]; char ticker[16]; int decimals; } tokens[10];
    int n_tokens = 0;
    if (db) {
        balance = wv_query_ground_truth_balance(db, NULL);
        shielded_bal = wv_query_shielded_balance(db, NULL);
        /* Load held tokens */
        {
            sqlite3_stmt *ts = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT hex(t.token_id), t.ticker, t.decimals "
                    "FROM zslp_tokens t "
                    "JOIN zslp_transfers tr ON tr.token_id = t.token_id "
                    "WHERE tr.to_addr IN (SELECT pubkey_hash FROM wallet_keys) "
                    "  AND tr.tx_type IN ('GENESIS','MINT','SEND') "
                    "GROUP BY t.token_id HAVING SUM(tr.amount) > 0 "
                    "ORDER BY SUM(tr.amount) DESC LIMIT 10",
                    -1, &ts, NULL) == SQLITE_OK) {
                while (sqlite3_step(ts) == SQLITE_ROW && n_tokens < 10) {
                    const char *tid = (const char *)sqlite3_column_text(ts, 0);
                    const char *tk = (const char *)sqlite3_column_text(ts, 1);
                    int dec = sqlite3_column_int(ts, 2);
                    if (tid && tk) {
                        snprintf(tokens[n_tokens].token_id, 65, "%s", tid);
                        snprintf(tokens[n_tokens].ticker, 16, "%s", tk);
                        tokens[n_tokens].decimals = dec;
                        n_tokens++;
                    }
                }
                sqlite3_finalize(ts);
            }
        }
        /* Load contacts */
        sqlite3_stmt *cs = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT name, address FROM contacts "
                "ORDER BY last_used DESC LIMIT 20",
                -1, &cs, NULL) == SQLITE_OK) {
            while (sqlite3_step(cs) == SQLITE_ROW && n_contacts < 20) {
                const char *cn = (const char *)sqlite3_column_text(cs, 0);
                const char *ca = (const char *)sqlite3_column_text(cs, 1);
                if (cn && ca) {
                    snprintf(contacts[n_contacts].name, 64, "%s", cn);
                    snprintf(contacts[n_contacts].addr, 256, "%s", ca);
                    n_contacts++;
                }
            }
            sqlite3_finalize(cs);
        }
        sqlite3_close(db);
    }

    size_t off = wv_emit_header(r, max, "Send — ZClassic23", "/wallet/send");

    char bal_fmt[32];
    zcl_format_zcl(bal_fmt, sizeof(bal_fmt), balance);

    /* Shielded note (raw HTML, empty if no shielded balance) */
    char shielded_note[256] = "";
    if (shielded_bal > 0) {
        char z_fmt[32];
        zcl_format_zcl(z_fmt, sizeof(z_fmt), shielded_bal);
        snprintf(shielded_note, sizeof(shielded_note),
            "<div style='color:#a78bfa;font-size:14px;margin-top:8px'>"
            "&#x1F512; %s ZCL private "
            "<span style='color:#666'>&mdash; send to zs1... address to use</span>"
            "</div>",
            z_fmt);
    }

    /* Currency selector (raw HTML, empty if no tokens) */
    char currency_selector[2048] = "";
    if (n_tokens > 0) {
        size_t cs_off = 0;
        cs_off += (size_t)snprintf(currency_selector + cs_off,
            sizeof(currency_selector) - cs_off,
            "<div class='form-group'>"
            "<label class='form-label' for='currency'>Currency</label>"
            "<select class='form-input' id='currency' name='currency' "
            "style='padding:12px 16px'>"
            "<option value='ZCL'>ZCL (ZClassic)</option>");
        for (int i = 0; i < n_tokens; i++) {
            char esc_tk[32], esc_tid[130];
            html_escape(esc_tk, sizeof(esc_tk), tokens[i].ticker);
            html_escape(esc_tid, sizeof(esc_tid), tokens[i].token_id);
            cs_off += (size_t)snprintf(currency_selector + cs_off,
                sizeof(currency_selector) - cs_off,
                "<option value='%s'>%s (ZSLP Token)</option>",
                esc_tid, esc_tk);
        }
        snprintf(currency_selector + cs_off,
            sizeof(currency_selector) - cs_off,
            "</select></div>");
    }

    /* Fee string */
    char fee_str[16];
    snprintf(fee_str, sizeof(fee_str), "%.4f", FEE_ZCL);

    /* Contacts datalist (raw HTML) */
    char contacts_html[4096];
    {
        size_t cl_off = 0;
        cl_off += (size_t)snprintf(contacts_html + cl_off,
            sizeof(contacts_html) - cl_off, "<datalist id='contacts'>");
        for (int i = 0; i < n_contacts; i++) {
            char esc_name[128], esc_addr[512];
            html_escape(esc_name, sizeof(esc_name), contacts[i].name);
            html_escape(esc_addr, sizeof(esc_addr), contacts[i].addr);
            cl_off += (size_t)snprintf(contacts_html + cl_off,
                sizeof(contacts_html) - cl_off,
                "<option value='%s' label='%s'>",
                esc_addr, esc_name);
        }
        snprintf(contacts_html + cl_off,
            sizeof(contacts_html) - cl_off, "</datalist>");
    }

    struct template_var send_vars[] = {
        { "spendable",         bal_fmt },
        { "shielded_note",     shielded_note },
        { "currency_selector", currency_selector },
        { "fee",               fee_str },
        { "contacts_datalist", contacts_html },
    };
    off += template_render(TMPL_SEND, send_vars,
        sizeof(send_vars) / sizeof(send_vars[0]),
        (char *)r + off, max - off);

    APPEND(off, r, max,
        "<script>"
        "var BAL=%.8f;"
        "function updateRemaining(){"
        "var a=parseFloat(document.getElementById('amt').value)||0;"
        "var r=document.getElementById('remaining');"
        "if(a>0&&a<=BAL){r.textContent='Remaining: '+(BAL-a-%.4f).toFixed(8)+' ZCL';"
        "r.style.color='#666';}"
        "else if(a>BAL){r.textContent='Insufficient funds';"
        "r.style.color='#f87171';}"
        "else{r.textContent='';}}"
        "function validateSend(){"
        "var a=document.getElementById('addr').value.trim();"
        "var m=document.getElementById('amt').value.trim();"
        "document.getElementById('addr-err').textContent='';"
        "document.getElementById('amt-err').textContent='';"
        "var minLen=a&&a.startsWith('zs1')?70:26;"
        "if(!a||a.length<minLen){"
        "document.getElementById('addr-err').textContent="
        "'Enter a valid address';return false;}"
        "if(!(/^(t[13]|zs1)/.test(a))){"
        "document.getElementById('addr-err').textContent="
        "'Must start with t1, t3, or zs1';return false;}"
        "if(!(/^[a-zA-Z0-9]+$/.test(a))){"
        "document.getElementById('addr-err').textContent="
        "'Invalid characters in address';return false;}"
        "var amt=parseFloat(m);"
        "if(isNaN(amt)||amt<=0){"
        "document.getElementById('amt-err').textContent="
        "'Enter an amount';return false;}"
        "if(amt+%.4f>BAL){"
        "document.getElementById('amt-err').textContent="
        "'Insufficient funds: need '+(amt+%.4f-BAL).toFixed(8)+' more ZCL';"
        "return false;}"
        "return true;}"
        /* Real-time address validation + privacy hint on input */
        "document.getElementById('addr').addEventListener('input',function(){"
        "var a=this.value.trim(),e=document.getElementById('addr-err');"
        "var ph=document.getElementById('privacy-hint');"
        "e.textContent='';"
        "if(ph){if(a.startsWith('zs1')&&a.length>10)"
        "{ph.innerHTML='&#x1F512; <span style=\"color:#a78bfa\">"
        "Private send</span>';}"
        "else if(a.startsWith('t1')||a.startsWith('t3'))"
        "{ph.innerHTML='&#x1F534; <span style=\"color:#fbbf24\">"
        "Visible on blockchain</span>';}"
        "else{ph.textContent='';}}"
        "if(!a)return;"
        "var ml=a.startsWith('zs1')?70:26;"
        "if(a.length>=ml&&/^(t[13]|zs1)/.test(a)&&/^[a-zA-Z0-9]+$/.test(a))"
        "{this.style.borderColor='#34d399';"
        "setTimeout(function(){document.getElementById('addr')"
        ".style.borderColor='';},1500);}});"
        "document.getElementById('addr').addEventListener('blur',function(){"
        "var a=this.value.trim(),e=document.getElementById('addr-err');"
        "e.textContent='';"
        "if(!a)return;"
        "var ml=a.startsWith('zs1')?70:26;"
        "if(a.length<ml){e.textContent='Address too short';return;}"
        "if(!(/^(t[13]|zs1)/.test(a))){e.textContent="
        "'Must start with t1, t3, or zs1';return;}"
        "if(!(/^[a-zA-Z0-9]+$/.test(a))){e.textContent="
        "'Invalid characters';return;}"
        "this.style.borderColor='#34d399';"
        "setTimeout(function(){document.getElementById('addr')"
        ".style.borderColor='';},1500);});"
        /* Loading overlay on submit */
        "document.getElementById('review-btn').addEventListener('click',"
        "function(e){if(!validateSend()){e.preventDefault();return;}"
        "this.disabled=true;this.textContent='Reviewing...';"
        "});"
        "</script>",
        (double)balance / (double)ZATOSHI_PER_ZCL, FEE_ZCL, FEE_ZCL, FEE_ZCL);

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── Receive (/wallet/receive) ──────────────────────────────── */

static size_t serve_receive(uint8_t *r, size_t max) {
    sqlite3 *db = wv_open_db();
    size_t off = wv_emit_header(r, max, "Receive — ZClassic23", "/wallet/receive");

    off += template_render(TMPL_RECEIVE_TABS, NULL, 0,
        (char *)r + off, max - off);
    off += template_render(TMPL_RECEIVE_ZPANE_OPEN, NULL, 0,
        (char *)r + off, max - off);

    int z_shown = 0;
    if (db) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT address FROM wallet_sapling_keys "
                "WHERE address IS NOT NULL AND length(address) > 0 "
                "ORDER BY rowid",
                -1, &s, NULL) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW && off + 512 < max) {
                const char *raw = (const char *)sqlite3_column_text(s, 0);
                if (!raw || !raw[0]) continue;
                char escaped[1024];
                html_escape(escaped, sizeof(escaped), raw);
                if (z_shown == 0) {
                    off = wv_emit_qr_svg(r, max, off, raw, 3);
                    APPEND(off, r, max,
                        "<div class='addr-display-sm' style='font-size:13px;"
                        "margin-top:12px'>%s</div>", escaped);
                } else if (z_shown == 1) {
                    APPEND(off, r, max,
                        "<details style='margin-top:8px'>"
                        "<summary style='color:#888;font-size:14px;"
                        "cursor:pointer'>Show all addresses</summary>"
                        "<div class='addr-display-sm' style='font-size:13px'>"
                        "%s</div>", escaped);
                } else {
                    APPEND(off, r, max,
                        "<div class='addr-display-sm' style='font-size:13px'>"
                        "%s</div>", escaped);
                }
                z_shown++;
            }
            sqlite3_finalize(s);
        }
        if (z_shown > 1)
            APPEND(off, r, max, "</details>");
        sqlite3_close(db);
        db = NULL;
    }

    if (z_shown == 0) {
        off += template_render(TMPL_RECEIVE_NO_ZADDR, NULL, 0,
            (char *)r + off, max - off);
    }

    off += template_render(TMPL_RECEIVE_ZPANE_CLOSE, NULL, 0,
        (char *)r + off, max - off);

    /* ── Public address pane ── */
    {
        /* Build QR SVG into temp buffer */
        char qr_buf[65536] = "";
        wv_emit_qr_svg((uint8_t *)qr_buf, sizeof(qr_buf), 0, PRIMARY_ADDR, 5);

        /* Build chunked address display */
        char chunk_buf[512];
        size_t ci = 0;
        const char *a = PRIMARY_ADDR;
        size_t alen = strlen(a);
        ci += (size_t)snprintf(chunk_buf + ci, sizeof(chunk_buf) - ci,
            "<div class='addr-display addr-chunked' "
            "style='margin-top:16px' id='t-addr'>"
            "<span class='hi'>%.4s</span>", a);
        for (size_t i = 4; i < alen && ci < sizeof(chunk_buf) - 80; i += 4) {
            size_t left = alen - i;
            if (left > 4) left = 4;
            ci += (size_t)snprintf(chunk_buf + ci, sizeof(chunk_buf) - ci,
                "<span class='sep'> </span>");
            if (i + left >= alen)
                ci += (size_t)snprintf(chunk_buf + ci, sizeof(chunk_buf) - ci,
                    "<span class='hi'>%.*s</span>", (int)left, a + i);
            else
                ci += (size_t)snprintf(chunk_buf + ci, sizeof(chunk_buf) - ci,
                    "%.*s", (int)left, a + i);
        }
        snprintf(chunk_buf + ci, sizeof(chunk_buf) - ci, "</div>");

        struct template_var tv[] = {
            { "qr_svg", qr_buf },
            { "chunked_addr", chunk_buf },
        };
        off += template_render(TMPL_RECEIVE_TPANE, tv, 2,
            (char *)r + off, max - off);
    }

    off += template_render(TMPL_RECEIVE_JS, NULL, 0,
        (char *)r + off, max - off);
    off += template_render(TMPL_RECEIVE_COPY_JS, NULL, 0,
        (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── History (/wallet/history) ──────────────────────────────── */

static size_t serve_history(uint8_t *r, size_t max, int page,
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

    /* Filter: all, sent, received — always use WHERE 1=1 base */
    const char *filter_clause = "";
    if (filter && strcmp(filter, "sent") == 0)
        filter_clause = " AND wt.from_me = 1";
    else if (filter && strcmp(filter, "recv") == 0)
        filter_clause = " AND wt.from_me = 0";

    /* Search by txid prefix */
    char search_clause[256] = "";
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
        if (safe_search[0])
            snprintf(search_clause, sizeof(search_clause),
                " AND hex(wt.txid) LIKE '%%%s%%'", safe_search);
    }

    /* Exclude ghost entries: must have a block, wallet_utxos, or be a known send */
    const char *ghost_filter =
        " AND (wt.block_height > 0 OR EXISTS "
        "(SELECT 1 FROM wallet_utxos wu WHERE wu.txid = wt.txid)"
        " OR wt.from_me = 1)";
    char count_sql[1024];
    snprintf(count_sql, sizeof(count_sql),
        "SELECT count(*) FROM wallet_transactions wt"
        " WHERE 1=1%s%s%s",
        filter_clause, ghost_filter, search_clause);
    int tx_count = wv_query_int(db, count_sql);

    int total_pages = (tx_count + per_page - 1) / per_page;
    if (page >= total_pages && total_pages > 0) page = total_pages - 1;

    /* Filter tabs with counts */
    const char *f = filter ? filter : "all";
    char all_sql[512], sent_sql[512], recv_sql[512];
    snprintf(all_sql, sizeof(all_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE 1=1%s", ghost_filter);
    snprintf(sent_sql, sizeof(sent_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE wt.from_me=1%s", ghost_filter);
    snprintf(recv_sql, sizeof(recv_sql),
        "SELECT count(*) FROM wallet_transactions wt WHERE wt.from_me=0%s", ghost_filter);
    int c_all = wv_query_int(db, all_sql);
    int c_sent = wv_query_int(db, sent_sql);
    int c_recv = wv_query_int(db, recv_sql);
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
    char history_sql[1024];
    snprintf(history_sql, sizeof(history_sql),
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
        "WHERE 1=1%s%s%s"
        "ORDER BY ht DESC LIMIT ? OFFSET ?",
        filter_clause, ghost_filter, search_clause);
    if (sqlite3_prepare_v2(db, history_sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, per_page);
        sqlite3_bind_int(s, 2, page * per_page);
        while (sqlite3_step(s) == SQLITE_ROW && off + 600 < max) {
            const char *txid = (const char *)sqlite3_column_text(s, 0);
            int h = sqlite3_column_int(s, 1);
            int64_t btime = sqlite3_column_int64(s, 2);
            int from_me = sqlite3_column_int(s, 3);
            (void)sqlite3_column_int64(s, 4); /* fee */
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
            /* Skip true ghost entries (no data at all) */
            if (display_val == 0 && !is_shield_op && h == 0) continue;

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

            /* Render card using template */
            char amt_s[32];
            snprintf(amt_s, sizeof(amt_s), "%.8f", (double)display_val / 1e8);

            if (is_shield_op) {
                struct template_var sv[] = {
                    { "txid",      esc_lower },
                    { "timestamp", esc_ts },
                    { "rel_time",  esc_rel },
                    { "conf_html", conf_html },
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

static size_t serve_coins(uint8_t *r, size_t max) {
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
        while (sqlite3_step(s) == SQLITE_ROW && ur + 500 < sizeof(utxo_rows)) {
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
            while (sqlite3_step(s) == SQLITE_ROW && nr + 400 < sizeof(note_rows)) {
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
            while (sqlite3_step(tok) == SQLITE_ROW) {
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

static size_t serve_shield(uint8_t *r, size_t max, const char *query) {
    /* Parse amount from query string */
    double amount = 0;
    if (query) {
        const char *amt = strstr(query, "amount=");
        if (amt) amount = strtod(amt + 7, NULL);
        /* ?all=1 — compute from balance (avoids leaking amount in URL) */
        if (strstr(query, "all=1")) {
            sqlite3 *sdb = wv_open_db();
            if (sdb) {
                int64_t bal = wv_query_ground_truth_balance(sdb, NULL);
                sqlite3_close(sdb);
                amount = (double)bal / (double)ZATOSHI_PER_ZCL - FEE_ZCL;
                if (amount < 0) amount = 0;
            }
        }
    }

    if (amount <= 0) {
        /* No amount specified — show amount input form */
        size_t off = wv_emit_header(r, max, "Secure — ZClassic23", "/wallet/shield");
        int64_t avail = 0;
        {
            sqlite3 *sdb = wv_open_db();
            if (sdb) {
                avail = wv_query_ground_truth_balance(sdb, NULL);
                sqlite3_close(sdb);
            }
        }
        char avail_str[32], max_str[32];
        zcl_format_zcl(avail_str, sizeof(avail_str), avail);
        snprintf(max_str, sizeof(max_str), "%.8f",
            (double)avail / (double)ZATOSHI_PER_ZCL - FEE_ZCL);
        struct template_var fv[] = {
            { "max_amount", max_str },
            { "available",  avail_str },
        };
        off += template_render(TMPL_SHIELD_AMOUNT_FORM, fv, 2,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    double fee = FEE_ZCL;
    double total_cost = amount + fee;

    size_t off = wv_emit_header(r, max, "Secure — ZClassic23", "/wallet/shield");

    /* Render shield confirmation using template */
    char amt_s[32], fee_s[32], tot_s[32];
    snprintf(amt_s, sizeof(amt_s), "%.8f", amount);
    snprintf(fee_s, sizeof(fee_s), "%.4f", fee);
    snprintf(tot_s, sizeof(tot_s), "%.4f", total_cost);

    struct template_var shield_vars[] = {
        { "amount", amt_s },
        { "fee",    fee_s },
        { "total",  tot_s },
    };
    off += template_render(TMPL_SHIELD_CONFIRM, shield_vars,
        sizeof(shield_vars) / sizeof(shield_vars[0]),
        (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── Shield Confirm (/wallet/shield/confirm POST) ──────────── */
/* Executes the shielding transaction via the node's z_sendmany. */

static size_t serve_shield_confirm(uint8_t *r, size_t max,
                                    const uint8_t *body, size_t body_len) {
    double amount = 0;
    char amount_str[32] = "";
    if (body && body_len > 0)
        wv_parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));
    if (amount_str[0])
        amount = strtod(amount_str, NULL);

    size_t off = wv_emit_header(r, max, "Securing — ZClassic23", "/wallet/shield");

    if (amount <= 0) {
        off += template_render(TMPL_SHIELD_INVALID, NULL, 0,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    /* Look up a z-address from the wallet to use as destination */
    char z_dest[256] = "";
    {
        sqlite3 *sdb = wv_open_db();
        if (sdb) {
            sqlite3_stmt *zs = NULL;
            if (sqlite3_prepare_v2(sdb,
                    "SELECT address FROM wallet_sapling_keys "
                    "WHERE address IS NOT NULL AND length(address) > 0 "
                    "ORDER BY rowid LIMIT 1",
                    -1, &zs, NULL) == SQLITE_OK && zs) {
                if (sqlite3_step(zs) == SQLITE_ROW) {
                    const char *a = (const char *)sqlite3_column_text(zs, 0);
                    if (a) snprintf(z_dest, sizeof(z_dest), "%s", a);
                }
                sqlite3_finalize(zs);
            }
            sqlite3_close(sdb);
        }
    }

    if (!z_dest[0]) {
        struct template_var ev[] = {{ "message",
            "No private address available. Is the node running?" }};
        off += template_render(TMPL_SHIELD_ERROR, ev, 1,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    /* Get funded t-address for z_sendmany. One fast RPC to zclassicd
     * listunspent — required because our SQLite doesn't track change
     * addresses from zclassicd transactions. This is the only RPC
     * besides z_sendmany itself. */
    char t_addr[128] = "";
    wv_get_funded_taddr(t_addr, sizeof(t_addr));

    if (!t_addr[0]) {
        struct template_var ev[] = {{ "message",
            "No public address found in wallet." }};
        off += template_render(TMPL_SHIELD_ERROR, ev, 1,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    /* Call zclassicd z_sendmany via RPC (Groth16 runs there) */
    char z_params[1024];
    snprintf(z_params, sizeof(z_params),
        "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}],1,%.8f]",
        t_addr, z_dest, amount, FEE_ZCL);

    char rpc_buf[4096] = "";
    int rpc_rc = wv_rpc_call("z_sendmany", z_params,
                                       rpc_buf, sizeof(rpc_buf));

    bool success = false;
    char opid_str[128] = "";
    char shield_err[256] = "";

    if (rpc_rc > 0) {
        /* Check for opid in result */
        char result_val[256] = "";
        zcl_json_extract_str(rpc_buf, "result", result_val, sizeof(result_val));
        if (strstr(result_val, "opid-")) {
            snprintf(opid_str, sizeof(opid_str), "%s", result_val);
            success = true;
        } else if (strstr(rpc_buf, "opid-")) {
            /* Extract opid from raw response */
            const char *op = strstr(rpc_buf, "opid-");
            size_t i = 0;
            while (op[i] && op[i] != '"' && op[i] != '}' && i < 127) {
                opid_str[i] = op[i]; i++;
            }
            opid_str[i] = '\0';
            success = true;
        } else {
            /* Error from zclassicd */
            zcl_json_extract_str(rpc_buf, "message", shield_err,
                                  sizeof(shield_err));
            if (!shield_err[0])
                snprintf(shield_err, sizeof(shield_err),
                    "zclassicd returned an error");
        }
    } else {
        snprintf(shield_err, sizeof(shield_err),
            "Could not connect to zclassicd (port %d). "
            "Start it with: zclassicd -daemon", ZCLASSICD_PORT);
    }

    if (success) {
        /* Track the pending shield operation for dashboard feedback */
        g_shield_pending_since = time(NULL);
        snprintf(g_shield_opid, sizeof(g_shield_opid), "%s", opid_str);
        g_shield_pending_amount = (int64_t)(amount * 1e8 + 0.5);
        /* Sync wallet tables immediately so balance is correct on return */
        wv_sync_wallet_from_zclassicd();
        g_balance_dirty = 1; /* Also recompute on next pulse */
        /* Build balance card */
        char bal_card[512] = "";
        {
            sqlite3 *sdb = wv_open_db();
            if (sdb) {
                int64_t new_t = wv_query_ground_truth_balance(sdb, NULL);
                int64_t new_z = wv_query_shielded_balance(sdb, NULL);
                char ts[32], zs[32], gs[32];
                snprintf(ts, sizeof(ts), "%.8f", (double)new_t / 1e8);
                snprintf(zs, sizeof(zs), "%.8f", (double)new_z / 1e8);
                snprintf(gs, sizeof(gs), "%.8f", (double)(new_t+new_z) / 1e8);
                struct template_var bv[] = {
                    {"total", gs}, {"transparent", ts}, {"shielded", zs}
                };
                template_render(TMPL_SHIELD_BALANCE_CARD, bv, 3,
                    bal_card, sizeof(bal_card));
                sqlite3_close(sdb);
            }
        }

        char amt_s[32];
        snprintf(amt_s, sizeof(amt_s), "%.8f", amount);
        struct template_var sv[] = {
            { "amount",       amt_s },
            { "opid",         opid_str },
            { "balance_card", bal_card },
        };
        off += template_render(TMPL_SHIELD_SUCCESS, sv, 3,
            (char *)r + off, max - off);
    } else {
        struct template_var ev[] = {{ "message",
            shield_err[0] ? shield_err : "Unknown error" }};
        off += template_render(TMPL_SHIELD_ERROR, ev, 1,
            (char *)r + off, max - off);
    }

    off += template_render(TMPL_BACK_TO_WALLET, NULL, 0,
        (char *)r + off, max - off);

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── Pulse endpoint (JSON) ──────────────────────────────────── */
/* Balance cache: recompute only when block height changes.
 * The ground-truth query has 3 JOINs — too heavy for 2-second polls. */

static struct {
    int height;
    int64_t balance, shielded, speed_bal;
    int t_utxos, z_notes;
} pulse_cache;

static size_t serve_pulse(uint8_t *r, size_t max) {
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

static size_t serve_send_review(uint8_t *r, size_t max,
                                 const uint8_t *body, size_t body_len) {
    size_t off = wv_emit_header(r, max, "Review — ZClassic23", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    wv_parse_form_field(body, body_len, "address", address, sizeof(address));
    wv_parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: checksum verification (Base58Check or Bech32) */
    bool addr_ok = wv_validate_zcl_address(address);
    const char *addr_err = NULL;
    if (!addr_ok) {
        size_t alen = strlen(address);
        if (alen < 26)
            addr_err = "Address too short.";
        else if (!(address[0] == 't' || (alen >= 3 && address[0] == 'z')))
            addr_err = "ZClassic addresses start with t1, t3, or zs1.";
        else
            addr_err = "Invalid address checksum. Check for typos.";
    }

    double amount = strtod(amount_str, NULL);
    const char *err_reason = !addr_ok
        ? (addr_err ? addr_err : "Invalid address.")
        : "Invalid amount";
    if (!addr_ok || amount <= 0) {
        struct template_var ev[] = {
            { "heading",   "Invalid Transaction" },
            { "message",   err_reason },
            { "back_url",  "/wallet" },
            { "back_label", "Back to Wallet" },
            { "retry_url", "/wallet/send" },
        };
        off += template_render(TMPL_VALIDATION_ERROR, ev, 5,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    bool is_shielded = (strncmp(address, "zs1", 3) == 0);
    double fee = FEE_ZCL;
    double total_deducted = amount + fee;

    /* Query balance */
    int64_t balance = 0;
    {
        sqlite3 *db = wv_open_db();
        if (db) {
            balance = wv_query_ground_truth_balance(db, NULL);
            sqlite3_close(db);
        }
    }
    double remaining = (double)balance / (double)ZATOSHI_PER_ZCL - total_deducted;

    char safe_addr[256];
    html_escape(safe_addr, sizeof(safe_addr), address);

    {
        char amt_s[32], fee_s[32], tot_s[32], rem_s[32];
        snprintf(amt_s, sizeof(amt_s), "%.8f", amount);
        snprintf(fee_s, sizeof(fee_s), "%.8f", fee);
        snprintf(tot_s, sizeof(tot_s), "%.8f", total_deducted);
        snprintf(rem_s, sizeof(rem_s), "%.8f", remaining);

        const char *pw = is_shielded
            ? "<div style='color:#fbbf24;font-size:13px;margin-top:4px'>"
              "&#x26A0; Your sending address is visible on-chain "
              "(t&#x2192;z send)</div>"
            : "";

        struct template_var rv[] = {
            { "address",         safe_addr },
            { "amount",          amt_s },
            { "fee",             fee_s },
            { "total",           tot_s },
            { "remaining",       rem_s },
            { "privacy_pill",    is_shielded ? "pill-private" : "pill-t" },
            { "privacy_label",   is_shielded ? "&#x1F512; Recipient private"
                                              : "&#x1F534; Public" },
            { "privacy_warning", pw },
        };
        off += template_render(TMPL_SEND_REVIEW, rv, 8,
            (char *)r + off, max - off);
    }

    /* Cancel / Confirm buttons */
    {
        char amt_s[32];
        snprintf(amt_s, sizeof(amt_s), "%.8f", amount);
        struct template_var bv[] = {
            { "address",   safe_addr },
            { "amount",    amt_s },
            { "btn_color", is_shielded ? "#a78bfa" : "#34d399" },
            { "btn_text",  is_shielded ? "#fff" : "#0c0c0c" },
        };
        off += template_render(TMPL_SEND_CONFIRM_BUTTONS, bv, 4,
            (char *)r + off, max - off);
    }

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── Send Confirm (/wallet/send/confirm POST) ──────────────── */

static size_t serve_send_confirm(uint8_t *r, size_t max,
                                  const uint8_t *body, size_t body_len) {
    size_t off = wv_emit_header(r, max, "Sending — ZClassic23", "/wallet/send");

    char address[128] = "", amount_str[32] = "";
    wv_parse_form_field(body, body_len, "address", address, sizeof(address));
    wv_parse_form_field(body, body_len, "amount", amount_str, sizeof(amount_str));

    /* Validate address: checksum verification */
    bool addr_ok = wv_validate_zcl_address(address);

    double amount = strtod(amount_str, NULL);
    if (!addr_ok || amount <= 0) {
        struct template_var ev[] = {
            { "heading",    "Invalid Transaction" },
            { "message",    !addr_ok ? "Invalid address" : "Invalid amount" },
            { "back_url",   "/wallet/send" },
            { "back_label", "Try Again" },
            { "retry_url",  "/wallet/send" },
        };
        off += template_render(TMPL_VALIDATION_ERROR, ev, 5,
            (char *)r + off, max - off);
        wv_emit_footer(r, max, &off);
        return off;
    }

    /* Execute send */
    bool is_shielded = (strncmp(address, "zs1", 3) == 0);
    char txid_result[128] = "";
    char error_msg[256] = "";
    int64_t amount_sat = (int64_t)(amount * 1e8 + 0.5);
    bool send_ok = false;

    if (is_shielded) {
        /* Shielded send: delegate to zclassicd z_sendmany.
         * Get the funded t-address from zclassicd listunspent
         * (our SQLite may be stale after change-address txs). */
        char t_from[128] = "";
        wv_get_funded_taddr(t_from, sizeof(t_from));
        if (t_from[0]) {
            char zp[1024];
            snprintf(zp, sizeof(zp),
                "[\"%s\",[{\"address\":\"%s\",\"amount\":%.8f}],1,%.8f]",
                t_from, address, amount, FEE_ZCL);
            char rb[4096] = "";
            if (wv_rpc_call("z_sendmany", zp, rb, sizeof(rb)) > 0) {
                char rv[256] = "";
                zcl_json_extract_str(rb, "result", rv, sizeof(rv));
                if (strstr(rv, "opid-") || strstr(rb, "opid-")) {
                    snprintf(txid_result, sizeof(txid_result), "%s",
                        rv[0] ? rv : "submitted");
                    send_ok = true;
                } else {
                    zcl_json_extract_str(rb, "message", error_msg,
                                          sizeof(error_msg));
                    if (!error_msg[0])
                        snprintf(error_msg, sizeof(error_msg),
                            "zclassicd returned an error");
                }
            } else {
                snprintf(error_msg, sizeof(error_msg),
                    "Could not connect to zclassicd (port %d)", ZCLASSICD_PORT);
            }
        } else {
            snprintf(error_msg, sizeof(error_msg),
                "No public address found in wallet");
        }
    } else {
        send_ok = wallet_direct_sendtoaddress(address, amount_sat,
            txid_result, sizeof(txid_result),
            error_msg, sizeof(error_msg));
    }

    if (send_ok) {
        /* Sync wallet tables immediately so balance is correct on return */
        wv_sync_wallet_from_zclassicd();
        g_balance_dirty = 1;
        char safe_addr[256], safe_txid[256];
        html_escape(safe_addr, sizeof(safe_addr), address);
        html_escape(safe_txid, sizeof(safe_txid), txid_result);

        bool is_opid = (strncmp(txid_result, "opid-", 5) == 0);
        char txid_html[512] = "";
        if (is_opid)
            snprintf(txid_html, sizeof(txid_html),
                "<div class='hash' style='word-break:break-all;"
                "color:#a78bfa;font-size:14px'>%s</div>"
                "<div style='color:#888;font-size:14px;margin-top:8px'>"
                "Funds will arrive after ~10 confirmations (~25 min)</div>",
                safe_txid);
        else
            snprintf(txid_html, sizeof(txid_html),
                "<a href='/explorer/tx/%s' class='hash' "
                "style='word-break:break-all'>%s</a>",
                safe_txid, safe_txid);

        wv_save_contact(address, address);

        char amt_s[32];
        snprintf(amt_s, sizeof(amt_s), "%.8f", amount);
        struct template_var sv[] = {
            { "heading",   is_opid ? "&#x1F512; Send to Private Address Started"
                                   : "Transaction Sent" },
            { "amount",    amt_s },
            { "address",   address },
            { "txid_html", txid_html },
        };
        off += template_render(TMPL_SEND_SUCCESS, sv, 4,
            (char *)r + off, max - off);
    } else {
        struct template_var ev[] = {
            { "heading", "Send Failed" },
            { "message", error_msg[0] ? error_msg : "Unknown error" },
        };
        off += template_render(TMPL_SEND_ERROR, ev, 2,
            (char *)r + off, max - off);
    }

    wv_emit_footer(r, max, &off);
    return off;
}

/* ── Transaction Detail (/wallet/tx/:txid) ──────────────────── */

static size_t serve_tx_detail(uint8_t *r, size_t max, const char *txid_hex) {
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
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT wt.block_height, wt.from_me, wt.fee, "
        "COALESCE(b.time, 0) "
        "FROM wallet_transactions wt "
        "LEFT JOIN blocks b ON wt.block_height = b.height "
        "WHERE hex(wt.txid) = '%s'", upper_txid);
    bool found = false;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
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
    snprintf(sql, sizeof(sql),
        "SELECT vout, value, hex(address_hash) "
        "FROM wallet_utxos WHERE hex(txid) = '%s' "
        "ORDER BY vout", upper_txid);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
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

/* ── Router ─────────────────────────────────────────────────── */

size_t wallet_view_handle_request(const char *method, const char *path,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *response, size_t response_max)
{
    (void)method;
    if (!path || !response || response_max == 0) return 0;

    /* JSON pulse endpoint — polled every 5s by dashboard JS */
    if (strcmp(path, "/api/wallet/pulse") == 0)
        return serve_pulse(response, response_max);

    if (strcmp(path, "/wallet") == 0 || strcmp(path, "/wallet/") == 0)
        return serve_dashboard(response, response_max);
    if (strcmp(path, "/wallet/send") == 0)
        return serve_send(response, response_max);
    if (strcmp(path, "/wallet/send/review") == 0)
        return serve_send_review(response, response_max, body, body_len);
    if (strcmp(path, "/wallet/send/confirm") == 0)
        return serve_send_confirm(response, response_max, body, body_len);
    if (strncmp(path, "/wallet/shield/confirm", 22) == 0)
        return serve_shield_confirm(response, response_max, body, body_len);
    if (strncmp(path, "/wallet/shield", 14) == 0) {
        const char *q = strchr(path, '?');
        return serve_shield(response, response_max, q);
    }
    if (strcmp(path, "/wallet/receive") == 0)
        return serve_receive(response, response_max);
    if (strncmp(path, "/wallet/history", 15) == 0) {
        int page = 0;
        const char *pq = strstr(path, "page=");
        if (pq) page = atoi(pq + 5);
        if (page < 0) page = 0;
        /* Parse filter param */
        const char *filt = NULL;
        char filt_buf[16] = "";
        const char *fp = strstr(path, "filter=");
        if (fp) {
            fp += 7;
            size_t fi = 0;
            while (fp[fi] && fp[fi] != '&' && fi < 15)
                { filt_buf[fi] = fp[fi]; fi++; }
            filt_buf[fi] = '\0';
            filt = filt_buf;
        }
        /* Parse search param */
        const char *srch = NULL;
        char srch_buf[65] = "";
        const char *sp = strstr(path, "q=");
        if (sp) {
            sp += 2;
            size_t si = 0;
            while (sp[si] && sp[si] != '&' && si < 64)
                { srch_buf[si] = sp[si]; si++; }
            srch_buf[si] = '\0';
            srch = srch_buf;
        }
        return serve_history(response, response_max, page, filt, srch);
    }
    if (strcmp(path, "/wallet/coins") == 0)
        return serve_coins(response, response_max);
    if (strncmp(path, "/wallet/tx/", 11) == 0) {
        const char *txid = path + 11;
        return serve_tx_detail(response, response_max, txid);
    }

    return 0;
}
