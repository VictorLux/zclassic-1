/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet HTML templates — Mustache-style {{var}} substitution.
 * Controller fills struct template_var[], calls template_render().
 * {{key}} = HTML-escaped, {{{key}}} = raw (for pre-escaped HTML). */

#ifndef ZCL_VIEWS_WALLET_TEMPLATES_H
#define ZCL_VIEWS_WALLET_TEMPLATES_H

/* ── Dashboard ────────────────────────────────────────────── */

static const char TMPL_DASHBOARD[] =
    "<div style='text-align:center;padding:24px 0 16px'>"
    "<span id='sync' class='pill {{{sync_class}}} sync-badge'>{{{sync_label}}}</span>"
    "<div id='bal' class='balance' style='margin-top:8px'>"
    "{{{balance}}} ZCL</div>"

    /* Privacy meter */
    "<div id='privacy-meter' style='margin:8px auto;max-width:280px'>"
    "<div style='display:flex;justify-content:space-between;"
    "font-size:14px;margin-bottom:4px'>"
    "<span style='color:{{{pct_color}}}'>{{{pct}}}% private</span>"
    "<a href='/wallet/coins' style='color:#888;font-size:14px'>"
    "Details</a></div>"
    "<div style='height:6px;background:#1e1e1e;border-radius:3px;"
    "overflow:hidden'>"
    "<div id='lock' style='height:100%;width:{{{pct}}}%;background:{{{pct_color}}};"
    "border-radius:3px;transition:width .5s'></div>"
    "</div></div>"

    /* Breakdown line */
    "<div id='bal-details' style='text-align:center;margin:4px 0'>"
    "<span id='breakdown' class='balance-sub' style='font-size:14px'>"
    "{{{breakdown}}}</span></div>"
    "</div>"

    /* Action buttons */
    "<div class='actions'>"
    "<a href='/wallet/send' class='btn-secondary'"
    " style='display:flex;align-items:center;justify-content:center'>Send</a>"
    "<a href='/wallet/receive' class='btn-primary'"
    " style='display:flex;align-items:center;justify-content:center'>Receive</a>"
    "</div>"

    /* Privacy nudge / shield status (injected raw) */
    "{{{privacy_card}}}"

    /* ZSLP tokens (injected raw) */
    "{{{token_cards}}}"

    /* Recent transactions */
    "<div class='section-header'>"
    "<span>Recent</span>"
    "<a href='/wallet/history'>View all</a></div>"
    "{{{recent_txs}}}"

    /* Backup warning (injected raw) */
    "{{{backup_warning}}}"

    /* Power Node stats */
    "<div class='section-header'>"
    "<span>Power Node</span></div>"
    "<div style='display:flex;gap:8px;margin:4px 0'>"
    "<div class='stat' style='flex:1;padding:10px'>"
    "<div class='n' style='font-size:16px'>{{{peers}}}</div>"
    "<div class='l'>Peers</div></div>"
    "<div class='stat' style='flex:1;padding:10px'>"
    "<div class='n' style='font-size:16px'>{{{mempool}}}</div>"
    "<div class='l'>Mempool</div></div>"
    "<div class='stat' style='flex:1;padding:10px'>"
    "<div class='n' style='font-size:16px;color:{{{node_status_color}}}'>"
    "{{{node_status}}}</div>"
    "<div class='l'>Status</div></div>"
    "</div>";

/* ── Send Form ────────────────────────────────────────────── */

static const char TMPL_SEND[] =
    "<div style='text-align:center;padding:16px 0'>"
    "<div style='color:#34d399;font-size:24px;font-weight:700'>"
    "{{{spendable}}} ZCL</div>"
    "<div style='color:#888;font-size:14px;margin-top:2px'>"
    "Spendable balance</div>"
    "{{{shielded_note}}}"
    "</div>"

    "<form id='send-form' method='POST' action='zcl://node/wallet/send/review' "
    "onsubmit='return validateSend()' autocomplete='off'>"
    "<div class='form-group'>"
    "<label class='form-label' for='addr'>To</label>"
    "<input class='form-input' type='text' id='addr' name='address' "
    "placeholder='Recipient address (t1... or zs1...)' required "
    "list='contacts'>"
    "<div id='addr-err' class='form-error'></div>"
    "<div id='privacy-hint' style='font-size:14px;margin-top:4px;"
    "height:16px'></div></div>"

    /* Currency selector (injected raw — empty if no tokens) */
    "{{{currency_selector}}}"

    "<div class='form-group'>"
    "<label class='form-label' for='amt'>Amount</label>"
    "<div style='display:flex;gap:8px;align-items:center'>"
    "<input class='form-input' type='text' id='amt' name='amount' "
    "inputmode='decimal' style='flex:1' placeholder='0.00' required "
    "oninput='updateRemaining()'>"
    "<button type='button' class='send-max' "
    "onclick='document.getElementById(\"amt\").value="
    "(BAL-{{{fee}}}).toFixed(8);updateRemaining()'>Max</button></div>"
    "<div id='remaining' class='remaining' "
    "style='color:#888;font-size:14px;margin:4px 0'></div>"
    "<div id='amt-err' class='form-error'></div>"
    "<div style='color:#888;font-size:14px;margin:4px 0'>"
    "Network fee: <span style='color:#f59e0b'>{{{fee}}} ZCL</span>"
    "</div></div>"
    "<button type='submit' class='btn-primary' style='margin-top:16px' "
    "id='review-btn'>Review Send</button>"
    "</form>"
    "{{{contacts_datalist}}}";

/* ── Shield Confirm ───────────────────────────────────────── */

static const char TMPL_SHIELD_CONFIRM[] =
    "<div class='card' style='border-left-color:#a78bfa;padding:20px;"
    "background:linear-gradient(135deg,#141414,#1a1a2a)'>"
    "<div style='text-align:center'>"
    "<div style='font-size:14px;color:#888;margin-bottom:8px'>"
    "&#x1F512; Securing</div>"
    "<div style='font-size:40px;color:#a78bfa;font-weight:800'>"
    "{{{amount}}} ZCL</div>"
    "<div style='color:#888;font-size:14px;margin-top:8px'>"
    "Fee: {{{fee}}} ZCL &middot; Total: {{{total}}} ZCL</div>"
    "</div></div>"

    "<div class='card'>"
    "<div style='color:#888;font-size:14px;line-height:1.6'>"
    "<div style='margin-bottom:8px'>"
    "<span style='color:#34d399;font-weight:700'>Step 1:</span> "
    "Your public ZCL moves to a private address (~2.5 min).</div>"
    "<div style='margin-bottom:8px'>"
    "<span style='color:#a78bfa;font-weight:700'>Step 2:</span> "
    "Funds are spendable immediately. For maximum privacy, wait ~6 hours "
    "so timing analysis cannot link back to the public source.</div>"
    "<div>"
    "<span style='color:#60a5fa;font-weight:700'>Step 3:</span> "
    "Spend from your private balance with no on-chain link to your identity.</div>"
    "</div></div>"

    "<div style='display:flex;gap:10px;margin:16px 0'>"
    "<a href='/wallet' class='btn-secondary' "
    "style='flex:1;text-align:center;text-decoration:none;"
    "display:flex;align-items:center;justify-content:center'>Cancel</a>"
    "<form method='POST' action='zcl://node/wallet/shield/confirm' "
    "style='flex:2;margin:0'>"
    "<input type='hidden' name='amount' value='{{{amount}}}'>"
    "<button type='submit' class='btn-primary' "
    "style='background:#a78bfa;color:#fff'"
    " id='shield-btn'>Confirm</button></form></div>"
    "<div id='shield-loading' class='loading-overlay' style='display:none'>"
    "<div class='spinner'></div>"
    "<p>Securing funds...</p></div>"
    "<script>"
    "document.getElementById('shield-btn').addEventListener('click',"
    "function(e){e.preventDefault();this.disabled=true;"
    "document.getElementById('shield-loading').style.display='flex';"
    "this.form.submit();});"
    "</script>";

/* ── Empty/Loading State ──────────────────────────────────── */

static const char TMPL_LOADING[] =
    "<div class='empty-state' style='padding:48px 0'>"
    "<div style='font-size:40px;margin-bottom:12px'>&#x23F3;</div>"
    "<div style='color:#e2e2e2;font-size:18px;font-weight:600'>"
    "Wallet Loading</div>"
    "<div style='margin-top:8px'>"
    "The database is not yet available.</div>"
    "</div>";

/* ── Privacy Nudge Card ───────────────────────────────────── */

static const char TMPL_PRIVACY_NUDGE[] =
    "<div class='privacy-card' style='display:flex;align-items:center;"
    "gap:12px;text-align:left'>"
    "<div style='flex:1'>"
    "<div class='title' style='margin:0'>{{{amount}}} ZCL publicly visible</div>"
    "<div class='desc' style='margin:0;margin-top:2px'>"
    "Make private to unlink from your address</div></div>"
    "<a class='btn' href='/wallet/shield?all=1' "
    "style='white-space:nowrap;padding:10px 16px'>Secure All</a>"
    "</div>";

/* ── Shield In Progress ───────────────────────────────────── */

static const char TMPL_SHIELD_PENDING[] =
    "<div class='privacy-card' style='display:flex;align-items:center;"
    "gap:12px;text-align:left;border-color:#a78bfa'>"
    "<div class='spinner' style='width:24px;height:24px;"
    "border:3px solid #333;border-top-color:#a78bfa;"
    "border-radius:50%;flex-shrink:0'></div>"
    "<div style='flex:1'>"
    "<div class='title' style='margin:0;color:#a78bfa'>"
    "&#x1F512; Securing funds...</div>"
    "<div class='desc' style='margin:0;margin-top:2px'>"
    "Your funds are being made private ({{{elapsed}}} sec ago). "
    "Confirms in ~2.5 min.</div></div></div>";

/* ── Shield Complete ──────────────────────────────────────── */

static const char TMPL_SHIELD_DONE[] =
    "<div class='privacy-card' style='display:flex;align-items:center;"
    "gap:12px;text-align:left;border-color:#34d399'>"
    "<div style='font-size:24px;flex-shrink:0'>&#x2705;</div>"
    "<div style='flex:1'>"
    "<div class='title' style='margin:0;color:#34d399'>"
    "Funds secured!</div>"
    "<div class='desc' style='margin:0;margin-top:2px'>"
    "Your ZCL is now private.</div></div></div>";

/* ── Backup Warning ───────────────────────────────────────── */

static const char TMPL_BACKUP_WARNING[] =
    "<div class='card' style='border-left-color:#f87171;margin:16px 0'>"
    "<div style='display:flex;align-items:center;gap:8px'>"
    "<span style='font-size:20px'>&#x26A0;</span>"
    "<div>"
    "<div style='color:#f87171;font-weight:700;font-size:14px'>"
    "Wallet Not Backed Up</div>"
    "<div style='color:#888;font-size:14px'>"
    "If you lose this device, your funds are gone forever.</div>"
    "</div></div>"
    "<div style='margin-top:10px'>"
    "<code style='font-size:13px;color:#999'>"
    "zcl-rpc dumpprivkey {{address}}</code></div>"
    "</div>";

/* ── Recent Transaction Row ───────────────────────────────── */

static const char TMPL_TX_ROW[] =
    "<a href='{{{link}}}' style='text-decoration:none;"
    "color:inherit;display:block'>"
    "<div class='tx-row'>"
    "<div>"
    "<span class='tx-amount {{{direction_class}}}'>{{{sign}}}{{{amount}}}</span>"
    "<span style='color:#888;font-size:14px;"
    "margin-left:6px'>ZCL</span></div>"
    "<div class='tx-meta'>"
    "<span class='tx-time'{{{time_style}}}>{{{time_label}}}</span>"
    "<span class='tx-conf'>{{{conf_label}}}</span>"
    "</div></div></a>";

/* ── History Transaction Card (normal) ─────────────────────── */

static const char TMPL_HISTORY_CARD[] =
    "<a href='/wallet/tx/{{{txid}}}' style='text-decoration:none;color:inherit'>"
    "<div class='tx-card' style='border-left-color:{{{color}}}'>"
    "<div style='display:flex;justify-content:space-between;"
    "align-items:baseline'>"
    "<span class='tx-amount {{{amount_class}}}'>{{{sign}}}{{{amount}}} ZCL</span>"
    "<span class='pill {{{pill_class}}}'>{{{pill_label}}}</span></div>"
    "<div class='tx-meta'>"
    "<span class='tx-time' title='{{timestamp}}'>{{{rel_time}}}</span>"
    "{{{conf_html}}}"
    "</div></div></a>";

/* ── History Shield Card ──────────────────────────────────── */

static const char TMPL_HISTORY_SHIELD[] =
    "<a href='/wallet/tx/{{{txid}}}' style='text-decoration:none;color:inherit'>"
    "<div class='tx-card' style='border-left-color:#a78bfa'>"
    "<div style='display:flex;justify-content:space-between;"
    "align-items:baseline'>"
    "<span style='color:#a78bfa;font-weight:700'>"
    "&#x1F512; Funds Secured</span>"
    "<span class='pill pill-private'>Secured</span></div>"
    "<div class='tx-meta'>"
    "<span class='tx-time' title='{{timestamp}}'>{{{rel_time}}}</span>"
    "{{{conf_html}}}"
    "</div></div></a>";

/* ── Receive: empty z-address state ──────────────────────── */

static const char TMPL_RECEIVE_NO_ZADDR[] =
    "<div class='empty-state'>"
    "<div style='color:#a78bfa;font-size:14px'>"
    "No private addresses yet</div>"
    "<div style='color:#888;font-size:14px;margin-top:4px'>"
    "Generate one: <code>zcl-rpc z_getnewaddress</code></div>"
    "</div>";

/* ── Confirmation HTML snippet ────────────────────────────── */

static const char TMPL_CONF_CONFIRMED[] =
    "<span class='tx-conf'>Block {{{block}}} &middot; {{{confs}}} confs</span>";

static const char TMPL_CONF_PENDING[] =
    "<span class='tx-conf pill pill-pending' style='font-size:13px'>Pending</span>";

/* ── Coins Page (/wallet/coins) ───────────────────────────── */

static const char TMPL_COINS_PAGE[] =
    "<h2>Your Coins</h2>"
    "<div style='color:#6b7280;font-size:13px;margin-bottom:16px'>"
    "Every coin in your wallet, verified against the blockchain.</div>"

    /* Public UTXOs */
    "<h3>Public UTXOs (Chain-Verified)</h3>"
    "<div class='overflow-x'>"
    "<table><tr><th>Transaction</th><th>Type</th>"
    "<th>Amount</th><th>Height</th><th>Conf</th></tr>"
    "{{{utxo_rows}}}"
    "<tr class='total-row'>"
    "<td colspan='2'>Total ({{{t_count}}} UTXO{{{t_plural}}})</td>"
    "<td class='zcl'>{{{t_total}}}</td>"
    "<td></td><td></td></tr></table></div>"

    /* Private notes */
    "<h3>Private Notes</h3>"
    "{{{notes_section}}}"

    /* Grand total stats */
    "<div class='stats' style='margin-top:20px'>"
    "<div class='stat'>"
    "<div class='n'>{{{t_total}}}</div>"
    "<div class='l'>Public</div></div>"
    "<div class='stat' style='border-color:#a78bfa'>"
    "<div class='n' style='color:#a78bfa'>{{{z_total}}}</div>"
    "<div class='l'>Private</div></div>"
    "<div class='stat' style='border-color:#f59e0b'>"
    "<div class='n' style='color:#f59e0b'>{{{grand_total}}}</div>"
    "<div class='l'>Total</div></div>"
    "</div>"

    /* Diagnostics */
    "<details style='margin-top:16px'>"
    "<summary style='color:#999;font-size:13px;font-weight:600;"
    "text-transform:uppercase;letter-spacing:.05em;cursor:pointer'>"
    "Diagnostics &#x25BE;</summary>"
    "<div class='overflow-x'>"
    "<table><tr><th>Source</th><th>Balance</th>"
    "<th>UTXOs</th><th>Status</th></tr>"
    "<tr><td>Chain UTXO set (chain-verified)</td>"
    "<td class='zcl'>{{{t_total}}}</td><td>{{{t_count}}}</td>"
    "<td><span class='pill pill-t'>verified</span></td></tr>"
    "<tr><td>Cached balance</td>"
    "<td class='zcl'>{{{speed_bal}}}</td><td>{{{speed_utxos}}}</td>"
    "<td>{{{diag_status}}}</td></tr>"
    "</table></div></details>"

    /* ZSLP tokens */
    "{{{token_section}}}"

    /* Chain supply */
    "<h3>Chain Supply</h3>"
    "<div class='stats'>"
    "<div class='stat'>"
    "<div class='n'>{{{chain_supply}}}</div>"
    "<div class='l'>UTXO Supply (ZCL)</div></div>"
    "<div class='stat'>"
    "<div class='n'>{{{chain_utxos}}}</div>"
    "<div class='l'>Total UTXOs</div></div>"
    "</div>";

/* Notes table (when notes exist) */
static const char TMPL_COINS_NOTES_TABLE[] =
    "<div class='overflow-x'>"
    "<table><tr><th>Amount</th>"
    "<th>Address</th><th>Count</th><th>Height Range</th></tr>"
    "{{{note_rows}}}"
    "<tr class='total-row'>"
    "<td class='zcl'>{{{z_total}}}</td>"
    "<td></td>"
    "<td>{{{z_notes}}} note{{{z_plural}}}</td>"
    "<td></td></tr></table></div>";

/* No notes message */
static const char TMPL_COINS_NO_NOTES[] =
    "<div class='card' style='border-left-color:#f59e0b'>"
    "<div class='label' style='color:#f59e0b'>"
    "No private notes found</div>"
    "<div style='color:#888;font-size:13px'>"
    "{{{sapling_keys}}} Sapling keys in wallet. "
    "Run <code style='color:#f59e0b'>rescanwallet</code> "
    "to scan the chain for notes belonging to these keys."
    "</div></div>";

/* ZSLP tokens table (when tokens exist) */
static const char TMPL_COINS_TOKENS[] =
    "<h3>ZSLP Tokens</h3>"
    "<div class='overflow-x'><table>"
    "<tr><th>Token</th><th>Name</th>"
    "<th>Balance</th></tr>"
    "{{{token_rows}}}"
    "</table></div>";

/* No tokens message */
static const char TMPL_COINS_NO_TOKENS[] =
    "<h3>ZSLP Tokens</h3>"
    "<div class='empty-state' style='padding:16px 0'>"
    "No tokens held at wallet addresses</div>";

/* ── Transaction Detail (/wallet/tx/:txid) ───────────────── */

static const char TMPL_TX_DETAIL[] =
    /* Header with direction and status */
    "<div style='text-align:center;padding:16px 0'>"
    "<span class='pill {{{pill_class}}}' style='font-size:13px;padding:4px 12px'>"
    "{{{direction}}}</span>"
    "<h2 style='margin:12px 0 4px;color:{{{color}}}'>{{{heading}}}</h2>"
    "<div class='balance-sub'>{{{rel_time}}} &middot; {{{abs_time}}}</div>"
    "</div>"

    /* Confirmation meter */
    "<div style='margin:0 0 16px'>"
    "<div style='display:flex;justify-content:space-between;"
    "font-size:13px;color:#888;margin-bottom:4px'>"
    "<span>{{{confs}}} confirmation{{{conf_plural}}}</span>"
    "<span>{{{conf_status}}}</span></div>"
    "<div class='conf-meter'>"
    "<div class='fill' style='width:{{{conf_pct}}}%;background:{{{conf_color}}}'></div>"
    "</div></div>"

    /* Transaction details grid */
    "<div class='detail-grid'>"
    "<div class='lbl'>TxID</div>"
    "<div class='val'><a href='/explorer/tx/{{{txid}}}' class='hash' "
    "style='font-size:13px'>{{{txid}}}</a></div>"
    "<div class='lbl'>Block</div>"
    "<div class='val'>{{{block_height}}}</div>"
    "<div class='lbl'>Direction</div>"
    "<div class='val'>{{{direction}}}</div>"
    "{{{fee_row}}}"
    "</div>"

    /* Wallet outputs */
    "{{{outputs_section}}}"

    /* Links */
    "<div style='text-align:center;margin:24px 0'>"
    "<a href='/explorer/tx/{{{txid}}}' style='color:#60a5fa;font-size:13px'>"
    "View full details in Explorer &rarr;</a></div>"
    "<div style='text-align:center'>"
    "<a href='/wallet/history' style='color:#34d399;font-size:14px'>"
    "&larr; Back to History</a></div>";

/* Transaction not found */
static const char TMPL_TX_NOT_FOUND[] =
    "<div class='result-warning'>"
    "<div class='icon'>&#x1F50D;</div>"
    "<h2>Transaction Not Found</h2>"
    "<p>This transaction is not in your wallet.</p>"
    "<a href='/wallet/history' style='color:#34d399'>Back to History</a>"
    "</div>";

/* Invalid transaction ID */
static const char TMPL_TX_INVALID[] =
    "<div class='result-error'>"
    "<div class='icon'>&#x2717;</div>"
    "<h2>Invalid Transaction ID</h2>"
    "<a href='/wallet/history' style='color:#34d399'>Back to History</a>"
    "</div>";

#endif /* ZCL_VIEWS_WALLET_TEMPLATES_H */
