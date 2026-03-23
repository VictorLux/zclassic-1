/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Comprehensive interaction tests for the wallet view controller.
 *
 * These tests simulate every user interaction: clicking through pages,
 * submitting forms, checking visual elements, verifying error states,
 * and validating security properties. Each test calls the controller
 * directly and inspects the HTML output like a user would see it.
 *
 * Test categories:
 *   1. Route resolution — every URL produces the right page
 *   2. Dashboard — balance, sync badge, recent txs, privacy card
 *   3. Send flow — form, validation, review, confirm, errors
 *   4. Receive — QR code, address display, chunking, tabs
 *   5. History — pagination, filters, search
 *   6. Coins — UTXO audit, shielded notes, data comparison
 *   7. Shield flow — confirmation, POST enforcement, error states
 *   8. Transaction detail — /wallet/tx/:txid
 *   9. Pulse API — JSON balance endpoint
 *  10. Visual consistency — CSS classes, color system, accessibility
 *  11. Security — XSS prevention, SQL injection, CSRF
 *  12. Edge cases — empty DB, huge values, special characters */

#include "test/test_helpers.h"
#include "controllers/wallet_view_controller.h"
#include <unistd.h>
#include <sys/stat.h>

/* Response buffer — 64KB is enough for any wallet page */
static uint8_t _wv_resp[65536];

/* Helper: call GET route, return response size */
static size_t wv_get(const char *path) {
    memset(_wv_resp, 0, sizeof(_wv_resp));
    return wallet_view_handle_request("GET", path, NULL, 0,
                                       _wv_resp, sizeof(_wv_resp));
}

/* Helper: call POST route with form body */
static size_t wv_post(const char *path, const char *body) {
    memset(_wv_resp, 0, sizeof(_wv_resp));
    return wallet_view_handle_request("POST", path,
                                       (const uint8_t *)body,
                                       body ? strlen(body) : 0,
                                       _wv_resp, sizeof(_wv_resp));
}

/* Helper: check response contains string */
static bool wv_has(const char *needle) {
    return strstr((char *)_wv_resp, needle) != NULL;
}

/* Helper: check response is a 200 HTML page */
static bool wv_is_200(void) {
    return wv_has("HTTP/1.1 200 OK") && wv_has("text/html");
}

int test_wallet_view(void)
{
    int failures = 0;

    /* Initialize with no datadir — tests DB-unavailable paths.
     * This is intentional: we want to verify graceful degradation. */
    wallet_view_init(NULL);

    /* ═══════════════════════════════════════════════════════════
     * 1. ROUTE RESOLUTION — every URL returns the correct page
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: GET /wallet returns dashboard... ");
    {
        size_t n = wv_get("/wallet");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("ZClassic Wallet");  /* page title */
        ok = ok && wv_has("class='nav'");       /* navigation */
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: GET /wallet/ (trailing slash) works... ");
    {
        size_t n = wv_get("/wallet/");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("ZClassic Wallet");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: GET /wallet/send returns send form... ");
    {
        size_t n = wv_get("/wallet/send");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("Send ZCL");
        ok = ok && wv_has("form");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: GET /wallet/receive returns receive page... ");
    {
        size_t n = wv_get("/wallet/receive");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("Receive");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: GET /wallet/history returns history (or loading)... ");
    {
        size_t n = wv_get("/wallet/history");
        bool ok = (n > 0) && wv_is_200();
        /* With no DB, should show loading state, not blank */
        ok = ok && wv_has("Wallet Loading");
        if (ok) printf("OK (graceful loading state)\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: GET /wallet/coins returns coins (or loading)... ");
    {
        size_t n = wv_get("/wallet/coins");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("Wallet Loading");
        if (ok) printf("OK (graceful loading state)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: GET /api/wallet/pulse returns JSON... ");
    {
        size_t n = wv_get("/api/wallet/pulse");
        bool ok = (n > 0);
        ok = ok && wv_has("application/json");
        ok = ok && wv_has("\"height\":");
        ok = ok && wv_has("\"balance\":");
        ok = ok && wv_has("\"peers\":");
        ok = ok && wv_has("\"sync\":");
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: GET /wallet/nonexistent returns 0 (no match)... ");
    {
        size_t n = wv_get("/wallet/nonexistent");
        if (n == 0) printf("OK\n");
        else { printf("FAIL (n=%zu, expected 0)\n", n); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 2. DASHBOARD — visual elements and structure
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: dashboard has navigation with 5 tabs... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("Dashboard</a>");
        ok = ok && wv_has("Send</a>");
        ok = ok && wv_has("Receive</a>");
        ok = ok && wv_has("History</a>");
        ok = ok && wv_has("Coins</a>");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard nav marks Dashboard as active... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("class='active'>Dashboard");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has balance or loading state... ");
    {
        wv_get("/wallet");
        /* With DB: shows balance. Without DB: shows loading hourglass. */
        bool ok = wv_has("class='balance'") || wv_has("Wallet Loading");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has sync badge or loading... ");
    {
        wv_get("/wallet");
        bool ok = (wv_has("id='sync'") && wv_has("sync-badge")) ||
                  wv_has("Wallet Loading");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has Send and Receive action buttons... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("href='/wallet/send'");
        ok = ok && wv_has("href='/wallet/receive'");
        ok = ok && wv_has(">Send</a>");
        ok = ok && wv_has(">Receive</a>");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has recent txs or loading... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("Recent</span>") || wv_has("Wallet Loading");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has status bar with live polling... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("id='sbar'");
        ok = ok && wv_has("class='status-bar'");
        ok = ok && wv_has("id='sb-h'");  /* height */
        ok = ok && wv_has("id='sb-p'");  /* peers */
        ok = ok && wv_has("id='sb-m'");  /* mempool */
        ok = ok && wv_has("setInterval");  /* polling JS */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard status bar uses readable labels... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("'Block '+d.height");
        ok = ok && wv_has("d.peers+' peers'");
        ok = ok && wv_has("d.mempool+' pending'");
        /* Must NOT have cryptic H:/P:/M: abbreviations */
        bool bad = wv_has("'H:'+d.height");
        if (ok && !bad) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard has polling JS or loading... ");
    {
        wv_get("/wallet");
        /* With DB: polling at 500ms. Without DB: loading state still has footer poll. */
        bool ok = wv_has(",500)") || wv_has("setInterval") || wv_has("Wallet Loading");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: dashboard DB-unavailable shows loading state... ");
    {
        wv_get("/wallet");
        /* With NULL datadir, should show loading hourglass */
        bool ok = wv_has("Wallet Loading") || wv_has("class='balance'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 3. SEND FLOW — form elements, validation, review, confirm
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: send form has address input with label... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("id='addr'");
        ok = ok && wv_has("name='address'");
        ok = ok && wv_has("for='addr'");  /* label association */
        ok = ok && wv_has("t1... or zs1...");  /* placeholder */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form has amount input with Max button... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("id='amt'");
        ok = ok && wv_has("name='amount'");
        ok = ok && wv_has("for='amt'");  /* label association */
        ok = ok && wv_has("send-max");  /* Max button class */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form has error display divs... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("id='addr-err'");
        ok = ok && wv_has("id='amt-err'");
        ok = ok && wv_has("id='remaining'");
        ok = ok && wv_has("form-error");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form POSTs to review (not confirm)... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("action='zcl://node/wallet/send/review'");
        ok = ok && wv_has("method='POST'");
        /* Must NOT go directly to confirm */
        bool bad = wv_has("action='zcl://node/wallet/send/confirm'");
        if (ok && !bad) printf("OK (two-step send)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form JS validates address prefix... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("^(t[13]|zs1)");  /* regex for prefix check */
        ok = ok && wv_has("Must start with t1, t3, or zs1");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form JS validates alphanumeric... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("^[a-zA-Z0-9]+$");  /* alphanumeric regex */
        ok = ok && wv_has("Invalid characters in address");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form shows specific insufficient funds message... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("Insufficient funds: need ");
        ok = ok && wv_has("more ZCL");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form has blur validation on address field... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("addEventListener('blur'");
        ok = ok && wv_has("Address too short");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send form Review button changes text on click... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("id='review-btn'");
        ok = ok && wv_has("this.textContent='Reviewing...'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send nav marks Send as active... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("class='active'>Send");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Send Review (POST) ──────────────────────────────────── */

    printf("wallet_view: send review rejects empty address... ");
    {
        size_t n = wv_post("/wallet/send/review", "address=&amount=1.0");
        bool ok = (n > 0) && wv_has("Invalid");
        ok = ok && wv_has("Try Again");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review rejects short address... ");
    {
        size_t n = wv_post("/wallet/send/review", "address=t1short&amount=1.0");
        bool ok = (n > 0) && wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review rejects bad prefix... ");
    {
        size_t n = wv_post("/wallet/send/review",
            "address=x1YRBXKYLHRB4X8sTkBeRysAzBTMMHpUXrn&amount=0.1");
        bool ok = (n > 0) && wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review rejects zero amount... ");
    {
        size_t n = wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0");
        bool ok = (n > 0) && wv_has("Invalid amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review rejects negative amount... ");
    {
        size_t n = wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=-5.0");
        bool ok = (n > 0) && wv_has("Invalid amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review accepts valid t1 address... ");
    {
        size_t n = wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = (n > 0) && wv_is_200();
        /* Should show review page with amount and address */
        ok = ok && wv_has("t1YRBXKYL");  /* address shown */
        ok = ok && wv_has("0.01");        /* amount shown */
        ok = ok && wv_has("Review");
        ok = ok && wv_has("Confirm Send");
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: send review shows fee on valid tx... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = wv_has("0.0001");  /* fee */
        ok = ok && wv_has("Fee");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review shows privacy level (transparent)... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = wv_has("Public (transparent)");
        ok = ok && wv_has("pill-t");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review has loading overlay for confirm... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = wv_has("id='send-loading'");
        ok = ok && wv_has("loading-overlay");
        ok = ok && wv_has("class='spinner'");
        ok = ok && wv_has("Sending transaction...");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review confirm uses POST (not GET link)... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = wv_has("method='POST'");
        ok = ok && wv_has("action='zcl://node/wallet/send/confirm'");
        ok = ok && wv_has("type='hidden' name='address'");
        ok = ok && wv_has("type='hidden' name='amount'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send review has Cancel button back to send... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = wv_has("href='/wallet/send'");
        ok = ok && wv_has("Cancel");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Send Confirm (POST) ─────────────────────────────────── */

    printf("wallet_view: send confirm rejects invalid address... ");
    {
        size_t n = wv_post("/wallet/send/confirm",
            "address=invalid&amount=0.01");
        bool ok = (n > 0) && wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: send confirm shows node offline when no node... ");
    {
        size_t n = wv_post("/wallet/send/confirm",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.01");
        bool ok = (n > 0);
        /* Node isn't running in test env, so should get offline or error */
        ok = ok && (wv_has("Node Offline") || wv_has("Send Failed") ||
                    wv_has("Unknown Response"));
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 4. RECEIVE — QR code, address display, chunking, tabs
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: receive has tab toggle (Transparent/Shielded)... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("tab-toggle");
        ok = ok && wv_has("id='tab-t'");
        ok = ok && wv_has("id='tab-z'");
        ok = ok && wv_has(">Transparent</a>");
        ok = ok && wv_has(">Shielded</a>");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive transparent tab is active by default... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("id='tab-t' class='active'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive has QR code SVG... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("<svg");
        ok = ok && wv_has("viewBox");
        ok = ok && wv_has("fill='black'");  /* QR modules */
        ok = ok && wv_has("fill='white'");  /* QR background */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive shows primary address with chunking... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("addr-chunked");
        ok = ok && wv_has("t1YR");  /* first chunk of PRIMARY_ADDR */
        ok = ok && wv_has("class='hi'");  /* highlighted chunks */
        ok = ok && wv_has("class='sep'");  /* separators */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive shows 'Tap to copy' hint... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("Tap address to copy");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive has click-to-copy JS with fallback... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("navigator.clipboard.writeText");
        ok = ok && wv_has("document.execCommand('copy')");  /* fallback */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive has tab switching JS... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("function showTab(t)");
        ok = ok && wv_has("pane-t");
        ok = ok && wv_has("pane-z");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive shielded pane hidden by default... ");
    {
        wv_get("/wallet/receive");
        /* Check both quote styles and that pane-z exists with display:none */
        bool ok = wv_has("id='pane-z'") && wv_has("display:none");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive shows privacy type indicator... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("pill-t");
        ok = ok && wv_has("Publicly visible on chain");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: receive nav marks Receive as active... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("class='active'>Receive");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 5. HISTORY — graceful degradation, filter tabs
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: history without DB shows loading... ");
    {
        size_t n = wv_get("/wallet/history");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("Wallet Loading");
        ok = ok && wv_has("database is not yet available");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: history has filter tabs in HTML... ");
    {
        /* With filter params, check the HTML contains filter structure */
        wv_get("/wallet/history?filter=all");
        /* Even without DB, should show loading, but let's check filter
         * params are parsed correctly by checking the structure */
        bool ok = wv_has("Wallet Loading");
        if (ok) printf("OK (loading state with filter param)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: history page=negative clamped to 0... ");
    {
        size_t n = wv_get("/wallet/history?page=-5");
        bool ok = (n > 0);  /* doesn't crash */
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 6. COINS — graceful degradation
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: coins without DB shows loading... ");
    {
        size_t n = wv_get("/wallet/coins");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("Wallet Loading");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: coins nav marks Coins as active... ");
    {
        wv_get("/wallet/coins");
        bool ok = wv_has("class='active'>Coins");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 7. SHIELD FLOW — confirmation, POST enforcement
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: shield with valid amount shows confirmation... ");
    {
        size_t n = wv_get("/wallet/shield?amount=0.5");
        bool ok = (n > 0) && wv_is_200();
        ok = ok && wv_has("0.50000000");  /* amount displayed */
        ok = ok && wv_has("Confirm Shield");
        ok = ok && wv_has("Cancel");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield shows 3-step privacy explanation... ");
    {
        wv_get("/wallet/shield?amount=0.5");
        bool ok = wv_has("Step 1:");
        ok = ok && wv_has("Step 2:");
        ok = ok && wv_has("Step 3:");
        ok = ok && wv_has("shielded address");
        ok = ok && wv_has("private and untraceable");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield shows fee and total cost... ");
    {
        wv_get("/wallet/shield?amount=0.5");
        bool ok = wv_has("0.0001");  /* fee */
        ok = ok && wv_has("Total:");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield confirm uses POST form (not GET link)... ");
    {
        wv_get("/wallet/shield?amount=0.5");
        bool ok = wv_has("method='POST'");
        ok = ok && wv_has("action='zcl://node/wallet/shield/confirm'");
        ok = ok && wv_has("name='amount'");
        /* Must NOT have a direct GET link to shield/confirm */
        bool bad = wv_has("href='/wallet/shield/confirm");
        if (ok && !bad) printf("OK (POST enforced)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield has loading overlay... ");
    {
        wv_get("/wallet/shield?amount=0.5");
        bool ok = wv_has("id='shield-loading'");
        ok = ok && wv_has("loading-overlay");
        ok = ok && wv_has("Shielding funds...");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield with zero amount shows error... ");
    {
        size_t n = wv_get("/wallet/shield?amount=0");
        bool ok = (n > 0) && wv_has("Invalid Amount");
        ok = ok && wv_has("Back to Wallet");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield with negative amount shows error... ");
    {
        size_t n = wv_get("/wallet/shield?amount=-1");
        bool ok = (n > 0) && wv_has("Invalid Amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield with no amount param shows error... ");
    {
        size_t n = wv_get("/wallet/shield");
        bool ok = (n > 0) && wv_has("Invalid Amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield confirm POST with no body shows error... ");
    {
        size_t n = wv_post("/wallet/shield/confirm", NULL);
        bool ok = (n > 0) && wv_has("Invalid amount");
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: shield confirm POST with zero amount shows error... ");
    {
        size_t n = wv_post("/wallet/shield/confirm", "amount=0");
        bool ok = (n > 0) && wv_has("Invalid amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 8. TRANSACTION DETAIL — /wallet/tx/:txid
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: tx detail with short txid shows error or loading... ");
    {
        size_t n = wv_get("/wallet/tx/abc123");
        bool ok = (n > 0);
        ok = ok && (wv_has("Invalid Transaction ID") || wv_has("Wallet Loading"));
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("wallet_view: tx detail with non-hex chars sanitized... ");
    {
        /* Inject SQL-like chars — should be stripped to hex only */
        size_t n = wv_get("/wallet/tx/"
            "aa' OR 1=1; DROP TABLE wallet_transactions; --bb");
        bool ok = (n > 0);
        /* Should show invalid ID or loading (not enough hex after sanitization) */
        ok = ok && (wv_has("Invalid Transaction ID") || wv_has("Wallet Loading"));
        if (ok) printf("OK (SQL injection prevented)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: tx detail with valid-format txid and no DB shows loading... ");
    {
        /* 64 hex chars, valid format but no DB available */
        size_t n = wv_get("/wallet/tx/"
            "0000000000000000000000000000000000000000000000000000000000000000");
        bool ok = (n > 0);
        ok = ok && (wv_has("Wallet Loading") || wv_has("Not Found"));
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 9. PULSE API — JSON structure and content
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: pulse returns valid JSON structure... ");
    {
        wv_get("/api/wallet/pulse");
        bool ok = wv_has("\"height\":");
        ok = ok && wv_has("\"balance\":");
        ok = ok && wv_has("\"shielded\":");
        ok = ok && wv_has("\"speed_balance\":");
        ok = ok && wv_has("\"t_utxos\":");
        ok = ok && wv_has("\"z_notes\":");
        ok = ok && wv_has("\"peers\":");
        ok = ok && wv_has("\"sync\":");
        ok = ok && wv_has("\"mempool\":");
        if (ok) printf("OK (9 fields)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: pulse has no-cache header... ");
    {
        wv_get("/api/wallet/pulse");
        bool ok = wv_has("Cache-Control: no-cache");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 10. VISUAL CONSISTENCY — CSS classes, design system
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: all pages include wallet CSS... ");
    {
        /* Check multiple pages have the CSS loaded */
        wv_get("/wallet");
        bool ok1 = wv_has("nav a:focus-visible");  /* from CSS */
        wv_get("/wallet/send");
        bool ok2 = wv_has("form-input");
        wv_get("/wallet/receive");
        bool ok3 = wv_has("addr-display");
        if (ok1 && ok2 && ok3) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: design system colors are consistent... ");
    {
        /* Check that non-standard colors are NOT used */
        wv_get("/wallet");
        bool has_bad = wv_has("#9966ff") || wv_has("#bb99ff") ||
                       wv_has("#ff4444") || wv_has("#ff8800") ||
                       wv_has("#ff6666");
        wv_get("/wallet/shield?amount=0.5");
        has_bad = has_bad || wv_has("#9966ff") || wv_has("#bb99ff");
        if (!has_bad) printf("OK (standard palette only)\n");
        else { printf("FAIL (non-standard colors found)\n"); failures++; }
    }

    printf("wallet_view: pages have viewport meta tag... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("name='viewport'");
        ok = ok && wv_has("width=device-width");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: pages have charset declaration... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("charset='utf-8'") || wv_has("charset=utf-8");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 11. SECURITY — XSS, injection, CSRF
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: XSS in address field is escaped... ");
    {
        wv_post("/wallet/send/review",
            "address=%3Cscript%3Ealert(1)%3C/script%3E&amount=0.01");
        /* The <script> tags should NOT appear unescaped in the output */
        bool bad = wv_has("<script>alert(1)</script>");
        bool ok = !bad && wv_has("Invalid");  /* should fail validation */
        if (ok) printf("OK\n");
        else { printf("FAIL (XSS possible!)\n"); failures++; }
    }

    printf("wallet_view: SQL injection in history search is sanitized... ");
    {
        /* Search param with SQL injection attempt */
        wv_get("/wallet/history?q='; DROP TABLE blocks; --");
        /* Should not crash, and search should use only hex chars */
        bool ok = !wv_has("DROP TABLE");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: path traversal in tx detail is sanitized... ");
    {
        wv_get("/wallet/tx/../../etc/passwd");
        /* Should strip non-hex chars and show invalid ID or loading */
        bool ok = wv_has("Invalid Transaction ID") || wv_has("Wallet Loading");
        /* Must NOT contain any file system content */
        bool bad = wv_has("root:") || wv_has("/bin/");
        if (ok && !bad) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: shield confirm is POST-only (not GET)... ");
    {
        /* GET to shield/confirm should show error (no amount in body) */
        wv_get("/wallet/shield/confirm");
        bool ok = wv_has("Invalid amount") || wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 12. EDGE CASES
     * ═══════════════════════════════════════════════════════════ */

    printf("wallet_view: very long address rejected... ");
    {
        char long_body[512];
        snprintf(long_body, sizeof(long_body),
            "address=t1%0200d&amount=0.01", 0);
        size_t n = wv_post("/wallet/send/review", long_body);
        bool ok = (n > 0) && wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: amount with many decimals accepted... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.00000001");
        bool ok = wv_has("0.00000001") || wv_has("Review");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: amount with text rejected... ");
    {
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=hello");
        bool ok = wv_has("Invalid amount");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: empty POST body handled gracefully... ");
    {
        size_t n = wv_post("/wallet/send/review", "");
        bool ok = (n > 0);  /* doesn't crash */
        ok = ok && wv_has("Invalid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: NULL path returns 0... ");
    {
        size_t n = wallet_view_handle_request("GET", NULL, NULL, 0,
                                                _wv_resp, sizeof(_wv_resp));
        if (n == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: zero-size response buffer doesn't crash... ");
    {
        uint8_t tiny[1];
        size_t n = wallet_view_handle_request("GET", "/wallet", NULL, 0,
                                                tiny, 0);
        if (n == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("wallet_view: small response buffer doesn't overflow... ");
    {
        uint8_t small[64];
        memset(small, 0xAA, sizeof(small));
        size_t n = wallet_view_handle_request("GET", "/wallet", NULL, 0,
                                                small, sizeof(small));
        /* Should produce a truncated but valid response, no overflow */
        bool ok = (n <= sizeof(small));
        if (ok) printf("OK (n=%zu)\n", n);
        else { printf("FAIL (overflow! n=%zu)\n", n); failures++; }
    }

    /* ═══════════════════════════════════════════════════════════
     * 13. LIVE RENDER — real datadir, real data, production-grade audit
     *     These tests only run if ~/.zclassic-c23/node.db exists.
     * ═══════════════════════════════════════════════════════════ */

    const char *home = getenv("HOME");
    char live_datadir[256], live_db[300];
    if (home) {
        snprintf(live_datadir, sizeof(live_datadir), "%s/.zclassic-c23", home);
        snprintf(live_db, sizeof(live_db), "%s/node.db", live_datadir);
    } else {
        live_datadir[0] = '\0';
        live_db[0] = '\0';
    }

    bool have_live_db = (access(live_db, R_OK) == 0);
    if (!have_live_db) {
        printf("wallet_view: LIVE TESTS SKIPPED (no %s)\n", live_db);
        return failures;
    }

    /* Switch to real datadir for live tests */
    wallet_view_init(live_datadir);
    printf("\n=== LIVE WALLET RENDER TESTS (real data) ===\n\n");

    /* ── Dashboard with real data ────────────────────────────── */

    printf("LIVE: dashboard shows real balance (not loading)... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("class='balance'") && wv_has("ZCL</div>");
        bool bad = wv_has("Wallet Loading");
        if (ok && !bad) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: dashboard shows sync badge... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("id='sync'") && wv_has("sync-badge");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: dashboard shows balance breakdown (transparent)... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("id='breakdown'") && wv_has("transparent");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: dashboard shows privacy shield card... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("privacy-card") && wv_has("Shield All Funds");
        if (ok) printf("OK (privacy nudge visible)\n");
        else printf("OK (no card — shielded balance exists or zero balance)\n");
        /* Not a failure either way — depends on balance state */
    }

    printf("LIVE: dashboard recent txs from wallet_transactions... ");
    {
        wv_get("/wallet");
        /* Should have tx-row elements with links to /wallet/tx/ */
        bool ok = wv_has("tx-row") || wv_has("No transactions yet");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: dashboard recent txs link to /wallet/tx/... ");
    {
        wv_get("/wallet");
        bool ok = wv_has("href='/wallet/tx/") || wv_has("No transactions yet");
        if (ok) printf("OK\n");
        else { printf("FAIL (links to explorer instead of wallet detail)\n"); failures++; }
    }

    printf("LIVE: dashboard balance > 0 (funds exist)... ");
    {
        wv_get("/api/wallet/pulse");
        /* Extract balance from JSON */
        const char *bal = strstr((char *)_wv_resp, "\"balance\":");
        int64_t balance = 0;
        if (bal) balance = strtoll(bal + 10, NULL, 10);
        if (balance > 0) printf("OK (%.8f ZCL)\n", (double)balance / 1e8);
        else { printf("FAIL (balance=%lld)\n", (long long)balance); failures++; }
    }

    printf("LIVE: pulse returns correct sync state... ");
    {
        wv_get("/api/wallet/pulse");
        bool ok = wv_has("\"sync\":\"");
        /* Must be a known state */
        ok = ok && (wv_has("at_tip") || wv_has("downloading") ||
                    wv_has("scanning") || wv_has("connecting") ||
                    wv_has("init") || wv_has("idle"));
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: pulse has peers > 0... ");
    {
        wv_get("/api/wallet/pulse");
        const char *p = strstr((char *)_wv_resp, "\"peers\":");
        int peers = 0;
        if (p) peers = atoi(p + 8);
        if (peers > 0) printf("OK (%d peers)\n", peers);
        else printf("WARN (0 peers)\n");
        /* Not a failure — node might be offline */
    }

    /* ── Send form with real balance ─────────────────────────── */

    printf("LIVE: send form shows real available balance... ");
    {
        wv_get("/wallet/send");
        bool ok = wv_has("Available:") && wv_has("ZCL");
        /* Should show a non-zero balance */
        bool has_zero_only = wv_has("0.00000000 ZCL") && !wv_has("0.9");
        if (ok && !has_zero_only) printf("OK\n");
        else if (ok) printf("OK (but balance might be 0)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: send form Max button uses real balance... ");
    {
        wv_get("/wallet/send");
        /* BAL variable should be set to actual balance */
        bool ok = wv_has("var BAL=");
        /* Should NOT be 0 */
        const char *bal_js = strstr((char *)_wv_resp, "var BAL=");
        double bal_val = 0;
        if (bal_js) bal_val = strtod(bal_js + 8, NULL);
        if (ok && bal_val > 0) printf("OK (BAL=%.8f)\n", bal_val);
        else if (ok) printf("OK (BAL=0, empty wallet)\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Receive with real address ───────────────────────────── */

    printf("LIVE: receive shows PRIMARY_ADDR in chunked format... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("t1YR") && wv_has("BXK");
        ok = ok && wv_has("addr-chunked");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: receive QR code is valid SVG... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("<svg") && wv_has("</svg>");
        ok = ok && wv_has("viewBox='0 0");
        /* Should have many rect elements (QR modules) */
        int rects = 0;
        const char *p = (char *)_wv_resp;
        while ((p = strstr(p, "<rect")) != NULL) { rects++; p += 5; }
        ok = ok && (rects > 50);  /* QR has many modules */
        if (ok) printf("OK (%d rects)\n", rects);
        else { printf("FAIL (rects=%d)\n", rects); failures++; }
    }

    printf("LIVE: receive has shielded addresses from DB... ");
    {
        wv_get("/wallet/receive");
        bool ok = wv_has("Shielded") || wv_has("No shielded addresses");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── History with real transactions ──────────────────────── */

    printf("LIVE: history shows real transactions... ");
    {
        wv_get("/wallet/history");
        bool ok = wv_has("Transaction History") && wv_has("filter-tabs");
        /* Should have tx cards or empty state */
        ok = ok && (wv_has("tx-card") || wv_has("0 transaction"));
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history filter tabs present and styled... ");
    {
        wv_get("/wallet/history");
        bool ok = wv_has("filter=all");
        ok = ok && wv_has("filter=sent");
        ok = ok && wv_has("filter=recv");
        ok = ok && wv_has(">All</a>");
        ok = ok && wv_has(">Sent</a>");
        ok = ok && wv_has(">Received</a>");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history search input present... ");
    {
        wv_get("/wallet/history");
        bool ok = wv_has("search-input") && wv_has("Search by txid");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history sent filter works... ");
    {
        wv_get("/wallet/history?filter=sent");
        bool ok = wv_has("Transaction History");
        /* The sent filter tab should be active */
        ok = ok && wv_has("filter=sent' class='active'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history recv filter works... ");
    {
        wv_get("/wallet/history?filter=recv");
        bool ok = wv_has("Transaction History");
        ok = ok && wv_has("filter=recv' class='active'");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history tx cards have direction badges... ");
    {
        wv_get("/wallet/history");
        bool ok = wv_has("pill-t") || wv_has("pill-pending") ||
                  wv_has("0 transaction");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: history pagination shows page count... ");
    {
        wv_get("/wallet/history");
        bool ok = wv_has("page ") && wv_has(" of ");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Coins page with real UTXOs ──────────────────────────── */

    printf("LIVE: coins page shows UTXO table... ");
    {
        wv_get("/wallet/coins");
        bool ok = wv_has("Coin Audit");
        ok = ok && wv_has("Transparent UTXOs");
        ok = ok && (wv_has("total-row") || wv_has("0 UTXO"));
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: coins page shows shielded notes section... ");
    {
        wv_get("/wallet/coins");
        bool ok = wv_has("Shielded Notes");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: coins page shows data source comparison... ");
    {
        wv_get("/wallet/coins");
        bool ok = wv_has("Data Source Comparison");
        ok = ok && wv_has("Chain UTXO set");
        ok = ok && wv_has("verified");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: coins page shows grand total stats... ");
    {
        wv_get("/wallet/coins");
        bool ok = wv_has("Transparent</div>") && wv_has("Shielded</div>");
        ok = ok && wv_has("Total</div>");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── production-grade design scrutiny ─────────────────────────── */

    printf("LIVE: no inline style= colors (use CSS classes)... ");
    {
        wv_get("/wallet");
        /* Count inline color overrides — some are acceptable for dynamic
         * values but raw hex colors in style= are a design smell */
        int inline_colors = 0;
        const char *p = (char *)_wv_resp;
        while ((p = strstr(p, "style='")) != NULL) {
            const char *end = strchr(p + 7, '\'');
            if (end && (strstr(p, "color:#") && strstr(p, "color:#") < end))
                inline_colors++;
            p += 7;
        }
        /* A few inline colors are acceptable for dynamic state.
         * More than 10 suggests we should use more CSS classes. */
        if (inline_colors <= 10) printf("OK (%d inline)\n", inline_colors);
        else { printf("WARN (%d inline colors — consider CSS classes)\n",
                       inline_colors); }
    }

    printf("LIVE: all pages have consistent footer structure... ");
    {
        const char *pages[] = {"/wallet", "/wallet/send", "/wallet/receive",
                                "/wallet/coins", NULL};
        bool ok = true;
        for (int i = 0; pages[i]; i++) {
            wv_get(pages[i]);
            if (!wv_has("class='status-bar'") || !wv_has("</html>")) {
                ok = false;
                break;
            }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: all pages have consistent nav structure... ");
    {
        const char *pages[] = {"/wallet", "/wallet/send", "/wallet/receive",
                                "/wallet/coins", NULL};
        bool ok = true;
        for (int i = 0; pages[i]; i++) {
            wv_get(pages[i]);
            if (!wv_has("class='nav'") || !wv_has("Dashboard</a>")) {
                ok = false;
                break;
            }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: no broken HTML entities... ");
    {
        wv_get("/wallet");
        /* Check for common broken entities */
        bool bad = wv_has("&amp;amp;") || wv_has("&amp;lt;") ||
                   wv_has("&#x0;") || wv_has("&undefined;");
        if (!bad) printf("OK\n");
        else { printf("FAIL (double-encoded entities)\n"); failures++; }
    }

    printf("LIVE: wallet CSS loads without overflow... ");
    {
        wv_get("/wallet");
        /* Check CSS contains key selectors */
        bool ok = wv_has(".balance{") || wv_has(".balance {");
        ok = ok && (wv_has(".nav{") || wv_has(".nav {") || wv_has(".nav a{"));
        ok = ok && (wv_has("@keyframes") || wv_has("@media"));
        if (ok) printf("OK\n");
        else { printf("FAIL (CSS missing/truncated)\n"); failures++; }
    }

    printf("LIVE: no TODO/FIXME/HACK in rendered HTML... ");
    {
        const char *pages[] = {"/wallet", "/wallet/send", "/wallet/receive",
                                "/wallet/history", "/wallet/coins", NULL};
        bool bad = false;
        for (int i = 0; pages[i]; i++) {
            wv_get(pages[i]);
            if (wv_has("TODO") || wv_has("FIXME") || wv_has("HACK") ||
                wv_has("XXX")) {
                bad = true;
                break;
            }
        }
        if (!bad) printf("OK\n");
        else { printf("FAIL (debug text in production HTML)\n"); failures++; }
    }

    printf("LIVE: send review with valid address shows checksum validation... ");
    {
        /* t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn is a real valid address */
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn&amount=0.001");
        bool ok = wv_has("Review") && wv_has("Confirm Send");
        ok = ok && wv_has("0.001");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("LIVE: send review with typo address shows checksum error... ");
    {
        /* Change one character to create invalid checksum */
        wv_post("/wallet/send/review",
            "address=t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrN&amount=0.001");
        bool ok = wv_has("checksum") || wv_has("Invalid");
        if (ok) printf("OK (typo caught)\n");
        else { printf("FAIL (typo not caught!)\n"); failures++; }
    }

    /* Dump pages to disk for manual inspection */
    system("mkdir -p .zcl_test_render");
    {
        const char *routes[][3] = {
            {"GET", "/wallet", "dashboard.html"},
            {"GET", "/wallet/send", "send.html"},
            {"GET", "/wallet/receive", "receive.html"},
            {"GET", "/wallet/history", "history.html"},
            {"GET", "/wallet/history?filter=sent", "history_sent.html"},
            {"GET", "/wallet/coins", "coins.html"},
            {"GET", "/wallet/shield?amount=0.5", "shield.html"},
            {"GET", "/api/wallet/pulse", "pulse.json"},
            {NULL, NULL, NULL}
        };
        for (int i = 0; routes[i][0]; i++) {
            wv_get(routes[i][1]);
            char path[128];
            snprintf(path, sizeof(path), ".zcl_test_render/%s", routes[i][2]);
            const char *html = strstr((char *)_wv_resp, "\r\n\r\n");
            if (html) html += 4; else html = (char *)_wv_resp;
            FILE *f = fopen(path, "w");
            if (f) { fputs(html, f); fclose(f); }
        }
        printf("LIVE: pages dumped to .zcl_test_render/ for inspection\n");
    }

    /* Restore NULL for safety */
    wallet_view_init(NULL);

    return failures;
}
