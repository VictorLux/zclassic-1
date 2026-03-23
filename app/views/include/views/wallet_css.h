/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Wallet stylesheet — compiled-in CSS for GTK browser wallet UI.
 * Design: WCAG AA dark theme, 480px mobile-first, 8px grid. */
#ifndef WALLET_CSS_H
#define WALLET_CSS_H

static const char wallet_css_0[] =
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:Inter,-apple-system,'Segoe UI',system-ui,sans-serif;"
    "background:#0c0c0c;color:#e2e2e2;max-width:480px;margin:0 auto;"
    "padding:16px;font-size:15px;line-height:1.5}"
    "a{color:#60a5fa;text-decoration:none;transition:color .15s ease}"
    "a:hover{color:#93c5fd}"
    "a:focus-visible{outline:2px solid #34d399;outline-offset:2px}"
    "code{font-family:'JetBrains Mono','SF Mono','Fira Code','Cascadia Code',monospace;"
    "font-size:12px;color:#f59e0b}"

    /* Navigation */
    ".nav{display:flex;gap:4px;margin:0 0 16px;padding:4px;background:#111;"
    "border-radius:10px}"
    ".nav a{flex:1;text-align:center;padding:8px 4px;border-radius:8px;"
    "font-size:13px;font-weight:600;color:#999;transition:all .15s ease;"
    "text-decoration:none;touch-action:manipulation}"
    ".nav a:hover{color:#e2e2e2;background:#161616}"
    ".nav a.active{color:#34d399;background:#0a1f14}"
    ".nav a:focus-visible{outline:2px solid #34d399;outline-offset:-2px}"

    /* Balance hero */
    ".balance{text-align:center;font-size:42px;font-weight:800;"
    "color:#34d399;letter-spacing:-1px;line-height:1.1}"
    ".balance-sub{text-align:center;color:#a1a1a1;font-size:13px;margin-top:8px}"

    /* Sync badge */
    ".sync-badge{display:inline-block;font-size:10px;font-weight:600;"
    "letter-spacing:.08em;text-transform:uppercase}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}"

    /* Pill variants */
    ".pill{display:inline-block;padding:2px 8px;border-radius:10px;"
    "font-size:11px;font-weight:700;letter-spacing:.03em}"
    ".pill-synced{background:#0a1f14;color:#34d399}"
    ".pill-syncing{background:#1a1510;color:#fbbf24;animation:pulse 1.5s ease infinite}"
    ".pill-confirmed{background:#0a1f14;color:#34d399}"
    ".pill-pending{background:#1a1510;color:#fbbf24}"
    ".pill-private{background:#1a1428;color:#a78bfa}"
    ".pill-t{background:#0a1f14;color:#34d399}"
    ".pill-z{background:#1a1428;color:#a78bfa}"
    ".pill-send{background:#1f1010;color:#f87171}";

static const char wallet_css_1[] =
    /* Actions */
    ".actions{display:flex;gap:12px;margin:16px 0}"
    ".actions a{flex:1;text-align:center;padding:14px 8px;border-radius:10px;"
    "font-size:15px;font-weight:700;text-decoration:none;"
    "transition:all .15s ease;touch-action:manipulation}"
    ".actions a:focus-visible{outline:2px solid #34d399;outline-offset:2px}"

    /* Buttons */
    ".btn-primary{display:block;width:100%;background:#34d399;color:#0c0c0c;"
    "border:none;padding:14px;font-size:15px;font-weight:700;"
    "border-radius:10px;cursor:pointer;font-family:inherit;"
    "transition:background .15s ease;touch-action:manipulation}"
    ".btn-primary:hover{background:#4ade80}"
    ".btn-primary:focus-visible{outline:2px solid #34d399;outline-offset:2px}"
    ".btn-primary:disabled{opacity:.5;cursor:not-allowed}"
    ".btn-secondary{display:block;width:100%;background:transparent;"
    "color:#e2e2e2;border:1px solid #333;padding:14px;font-size:15px;"
    "font-weight:700;border-radius:10px;cursor:pointer;font-family:inherit;"
    "transition:all .15s ease;touch-action:manipulation}"
    ".btn-secondary:hover{border-color:#999}"
    ".btn-secondary:focus-visible{outline:2px solid #34d399;outline-offset:2px}"
    ".send-max{background:#1e1e1e;color:#34d399;border:none;padding:6px 10px;"
    "font-size:11px;font-weight:700;border-radius:6px;cursor:pointer;"
    "font-family:inherit;white-space:nowrap;transition:background .15s ease;"
    "touch-action:manipulation}"
    ".send-max:hover{background:#333}"

    /* Forms */
    ".form-group{margin-bottom:16px}"
    ".form-label{display:block;font-size:11px;font-weight:600;"
    "text-transform:uppercase;letter-spacing:.08em;color:#a1a1a1;margin-bottom:6px}"
    ".form-input{display:block;width:100%;background:#111;color:#e2e2e2;"
    "border:1px solid #1e1e1e;padding:12px 14px;"
    "font-family:inherit;font-size:15px;border-radius:8px;"
    "transition:border-color .15s ease}"
    ".form-input:focus{border-color:#333;outline:none;"
    "box-shadow:0 0 0 3px rgba(52,211,153,.12)}"
    ".form-input::placeholder{color:#555}"
    ".form-error{color:#f87171;font-size:12px;margin-top:4px}";

static const char wallet_css_2[] =
    /* Section header */
    ".section-header{display:flex;justify-content:space-between;"
    "align-items:baseline;margin:24px 0 8px}"
    ".section-header span{font-size:11px;font-weight:600;"
    "text-transform:uppercase;letter-spacing:.08em;color:#888}"
    ".section-header a{font-size:12px;color:#888}"
    ".section-header a:hover{color:#a1a1a1}"

    /* Transaction rows */
    ".tx-row{display:flex;justify-content:space-between;align-items:center;"
    "padding:12px 0;border-bottom:1px solid #1e1e1e}"
    ".tx-row:last-child{border-bottom:none}"
    ".tx-amount{font-size:16px;font-weight:700;"
    "font-family:'JetBrains Mono','SF Mono','Fira Code',monospace}"
    ".tx-amount.recv{color:#34d399}"
    ".tx-amount.send{color:#f87171}"
    ".tx-meta{font-size:12px;color:#888;text-align:right}"
    ".tx-meta a{color:#60a5fa;font-family:'JetBrains Mono','SF Mono',monospace;"
    "font-size:11px}"
    ".tx-time{display:block;margin-bottom:2px}"
    ".tx-conf{font-size:11px;color:#555}"

    /* UTXO rows */
    ".utxo-row{display:flex;justify-content:space-between;align-items:center;"
    "padding:10px 0;border-bottom:1px solid #1e1e1e}"
    ".utxo-row:last-child{border-bottom:none}"

    /* Transaction cards (history page) */
    ".tx-card{background:#111;padding:14px 16px;border-radius:8px;"
    "margin:6px 0;border-left:3px solid #333}"
    ".tx-card:hover{background:#161616}"

    /* Address display */
    ".addr-display{background:#111;padding:14px;border-radius:8px;"
    "font-family:'JetBrains Mono','SF Mono','Fira Code',monospace;"
    "font-size:14px;color:#60a5fa;word-break:break-all;text-align:center;"
    "margin:12px 0;user-select:all;cursor:pointer;"
    "border:1px solid #1e1e1e;transition:border-color .15s ease}"
    ".addr-display:hover{border-color:#333}"
    ".addr-display-sm{background:#111;padding:10px;border-radius:8px;"
    "font-family:'JetBrains Mono','SF Mono','Fira Code',monospace;"
    "font-size:11px;color:#a78bfa;word-break:break-all;text-align:center;"
    "margin:8px 0;user-select:all;cursor:pointer;"
    "border:1px solid #1e1e1e;transition:border-color .15s ease}"
    ".addr-display-sm:hover{border-color:#333}";

static const char wallet_css_3[] =
    /* Review table */
    ".review-table{width:100%}"
    ".review-table td{padding:10px 0;font-size:14px}"
    ".review-table td:first-child{color:#888}"
    ".review-table td:last-child{text-align:right}"
    ".review-table tr+tr{border-top:1px solid #1e1e1e}"

    /* Result states */
    ".result-success{text-align:center;padding:32px 0}"
    ".result-success .icon{font-size:48px;margin-bottom:12px}"
    ".result-success h2{color:#34d399;font-size:20px;font-weight:700;margin-bottom:8px}"
    ".result-success p{color:#999;font-size:14px;margin:4px 0}"
    ".result-error{text-align:center;padding:32px 0}"
    ".result-error .icon{font-size:48px;margin-bottom:12px}"
    ".result-error h2{color:#f87171;font-size:20px;font-weight:700;margin-bottom:8px}"
    ".result-error p{color:#999;font-size:14px;margin:4px 0}"
    ".result-warning{text-align:center;padding:32px 0}"
    ".result-warning .icon{font-size:48px;margin-bottom:12px}"
    ".result-warning h2{color:#fbbf24;font-size:20px;font-weight:700;margin-bottom:8px}"
    ".result-warning p{color:#999;font-size:14px;margin:4px 0}"

    /* Status bar */
    ".status-bar{position:fixed;bottom:0;left:0;right:0;background:#111;"
    "border-top:1px solid #1e1e1e;padding:6px 16px;display:flex;"
    "justify-content:center;gap:16px;font-size:11px;color:#555;"
    "font-family:'JetBrains Mono','SF Mono',monospace;z-index:100}"
    ".status-bar span{white-space:nowrap}"
    "body{padding-bottom:40px}"

    /* Empty state */
    ".empty-state{text-align:center;padding:40px 0;color:#555;font-size:14px}"

    /* Discrepancy warning */
    ".discrepancy{background:#1a1510;border:1px solid #4a3520;border-radius:8px;"
    "padding:12px;font-size:12px;margin:12px 0}"
    ".discrepancy .title{color:#f59e0b;font-weight:700;margin-bottom:4px}"
    ".discrepancy .detail{color:#92712a}"

    /* Page controls */
    ".page-controls{display:flex;justify-content:center;gap:16px;"
    "margin:20px 0;font-size:14px}"
    ".page-controls a{color:#60a5fa}"

    /* Tables (coins page) */
    "table{width:100%;border-collapse:collapse;font-size:13px}"
    "th{text-align:left;color:#666;padding:8px 6px;"
    "border-bottom:1px solid #1e1e1e;font-size:11px;"
    "text-transform:uppercase;letter-spacing:.05em;font-weight:600}"
    "td{padding:8px 6px;border-bottom:1px solid #1e1e1e;color:#e2e2e2}"
    "tr:hover{background:#111}";

static const char wallet_css_4[] =
    ".hash{color:#60a5fa;"
    "font-family:'JetBrains Mono','SF Mono','Fira Code',monospace;font-size:12px}"
    ".zcl{color:#34d399;font-weight:700;font-family:'JetBrains Mono','SF Mono',monospace;"
    "font-size:14px;text-align:right}"
    ".mono{font-family:'JetBrains Mono','SF Mono','Fira Code',monospace;font-size:13px}"
    ".total-row{font-weight:700;background:#0a1f14}"
    ".overflow-x{overflow-x:auto;-webkit-overflow-scrolling:touch}"

    /* Coins page stats */
    ".stats{display:flex;gap:10px;margin:12px 0;flex-wrap:wrap}"
    ".stat{flex:1;min-width:120px;background:#111;padding:14px;"
    "border-radius:8px;text-align:center}"
    ".stat .n{font-size:24px;color:#34d399;font-weight:800;line-height:1.2;"
    "font-family:'JetBrains Mono','SF Mono',monospace}"
    ".stat .l{font-size:10px;color:#666;text-transform:uppercase;"
    "letter-spacing:.08em;margin-top:4px;font-weight:600}"

    /* Headings */
    "h2{color:#e2e2e2;font-size:18px;margin:24px 0 8px;font-weight:700}"
    "h3{color:#999;font-size:13px;font-weight:600;margin:20px 0 8px;"
    "text-transform:uppercase;letter-spacing:.05em}"
    ".sub{color:#666;font-size:13px;margin-bottom:12px}"

    /* Card (legacy compat for shield/coins pages) */
    ".card{background:#111;padding:14px 16px;border-radius:8px;"
    "margin:8px 0;border-left:3px solid #34d399}"
    ".card .label{color:#666;font-size:11px;font-weight:600;"
    "text-transform:uppercase;letter-spacing:.05em}"
    ".card .value{font-size:22px;color:#34d399;font-weight:700}"

    /* QR wrap */
    ".qr-wrap{text-align:center;margin:16px 0}"

    /* Remaining text in send form */
    ".remaining{color:#666;font-size:12px;margin-top:6px}"

    /* Sync note */
    ".sync-note{color:#60a5fa;font-size:12px;margin-top:6px;text-align:center}"

    /* Print */
    "@media print{.nav,.status-bar,.actions{display:none}"
    "body{background:#fff;color:#000;max-width:none}}";

static const char wallet_css_5[] =
    /* Loading overlay */
    ".loading-overlay{position:fixed;inset:0;background:rgba(12,12,12,.85);"
    "display:flex;align-items:center;justify-content:center;"
    "flex-direction:column;z-index:200}"
    ".loading-overlay .spinner{width:40px;height:40px;"
    "border:3px solid #222;border-top-color:#34d399;"
    "border-radius:50%;animation:spin .8s linear infinite}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".loading-overlay p{color:#a1a1a1;font-size:14px;margin-top:16px}"

    /* Filter tabs */
    ".filter-tabs{display:flex;gap:0;margin:12px 0;background:#111;"
    "border-radius:8px;overflow:hidden;border:1px solid #1e1e1e}"
    ".filter-tabs a{flex:1;text-align:center;padding:8px;font-size:12px;"
    "font-weight:600;color:#888;text-decoration:none;"
    "transition:all .15s ease}"
    ".filter-tabs a:hover{color:#e2e2e2;background:#161616}"
    ".filter-tabs a.active{color:#34d399;background:#0a1f14}"

    /* Privacy card */
    ".privacy-card{background:linear-gradient(135deg,#0a1428,#1a1428);"
    "border:1px solid #2a1a3a;border-radius:10px;padding:16px;"
    "margin:16px 0;text-align:center}"
    ".privacy-card .title{color:#a78bfa;font-size:13px;font-weight:600;"
    "margin-bottom:4px}"
    ".privacy-card .desc{color:#888;font-size:12px;margin-bottom:12px}"
    ".privacy-card .btn{display:inline-block;background:#a78bfa;color:#0c0c0c;"
    "padding:10px 24px;border-radius:8px;font-weight:700;font-size:13px;"
    "text-decoration:none;transition:background .15s ease}"
    ".privacy-card .btn:hover{background:#c4b5fd}"

    /* Address chunking */
    ".addr-chunked{font-family:'JetBrains Mono','SF Mono','Fira Code',monospace;"
    "font-size:15px;letter-spacing:.5px;line-height:1.8;"
    "word-break:break-all;text-align:center}"
    ".addr-chunked .sep{color:#333;margin:0 1px}"
    ".addr-chunked .hi{color:#34d399;font-weight:700}"

    /* Search input */
    ".search-input{display:block;width:100%;background:#111;color:#e2e2e2;"
    "border:1px solid #1e1e1e;padding:10px 14px;font-size:13px;"
    "border-radius:8px;font-family:inherit;margin:8px 0}"
    ".search-input:focus{border-color:#333;outline:none;"
    "box-shadow:0 0 0 3px rgba(52,211,153,.12)}"
    ".search-input::placeholder{color:#555}"

    /* Tab toggle (receive page) */
    ".tab-toggle{display:flex;gap:0;margin:12px 0;background:#111;"
    "border-radius:10px;overflow:hidden;border:1px solid #1e1e1e}"
    ".tab-toggle button,.tab-toggle a{flex:1;text-align:center;padding:10px;"
    "font-size:13px;font-weight:700;border:none;cursor:pointer;"
    "color:#888;background:transparent;font-family:inherit;"
    "transition:all .15s ease;text-decoration:none}"
    ".tab-toggle .active{color:#34d399;background:#0a1f14}"
    ".tab-toggle .active-z{color:#a78bfa;background:#1a1428}"

    /* Tx detail grid */
    ".detail-grid{display:grid;grid-template-columns:100px 1fr;"
    "gap:8px 12px;font-size:14px;margin:12px 0}"
    ".detail-grid .lbl{color:#888;font-weight:600;font-size:12px;"
    "text-transform:uppercase;letter-spacing:.05em}"
    ".detail-grid .val{color:#e2e2e2;word-break:break-all}"

    /* Confirmation progress */
    ".conf-meter{height:4px;background:#1e1e1e;border-radius:2px;margin:8px 0}"
    ".conf-meter .fill{height:100%;border-radius:2px;transition:width .3s ease}"

    /* Small screens */
    "@media(max-width:360px){body{padding:12px;font-size:14px}"
    ".balance{font-size:32px}.nav a{font-size:11px;padding:6px 2px}"
    ".actions a{padding:12px 4px;font-size:14px}"
    ".stat .n{font-size:18px}"
    "table{font-size:12px}td,th{padding:5px 4px}"
    ".detail-grid{grid-template-columns:1fr;gap:2px 0}"
    ".detail-grid .lbl{margin-top:8px}}";

static char _wallet_css_buf[24576];
static const char *wallet_css_get(void) {
    size_t off = 0;
    size_t l0 = __builtin_strlen(wallet_css_0);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_0, l0); off += l0;
    size_t l1 = __builtin_strlen(wallet_css_1);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_1, l1); off += l1;
    size_t l2 = __builtin_strlen(wallet_css_2);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_2, l2); off += l2;
    size_t l3 = __builtin_strlen(wallet_css_3);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_3, l3); off += l3;
    size_t l4 = __builtin_strlen(wallet_css_4);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_4, l4); off += l4;
    size_t l5 = __builtin_strlen(wallet_css_5);
    __builtin_memcpy(_wallet_css_buf + off, wallet_css_5, l5); off += l5;
    _wallet_css_buf[off] = 0;
    return _wallet_css_buf;
}
#define wallet_css (wallet_css_get())

#endif
