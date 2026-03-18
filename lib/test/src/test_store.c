/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Store controller + ZSLP token + template engine tests. */

#include "test/test_helpers.h"
#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include "util/template.h"
#include <unistd.h>

static char test_datadir[256];

static void setup_datadir(void)
{
    snprintf(test_datadir, sizeof(test_datadir), ".zcl_test_store_%d",
             (int)getpid());
    mkdir(test_datadir, 0755);
}

static void cleanup_datadir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_datadir);
    system(cmd);
}

int test_store(void)
{
    int failures = 0;
    setup_datadir();

    uint8_t resp[16384];

    /* ── Product listing ──────────────────────────────────── */

    printf("store: GET /store returns product listing... ");
    {
        size_t n = store_handle_request("GET", "/store", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "HTTP/1.1 200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "ZCL Store") != NULL);
        ok = ok && (strstr((char *)resp, "ZCL23 Access Token") != NULL);
        ok = ok && (strstr((char *)resp, "VPN Credit") != NULL);
        ok = ok && (strstr((char *)resp, "Storage") != NULL);
        if (ok) printf("OK (%zu bytes, 3 products)\n", n);
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("store: GET /store/ (trailing slash) works... ");
    {
        size_t n = store_handle_request("GET", "/store/", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "200 OK") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Product detail ───────────────────────────────────── */

    printf("store: GET /store/product/1 returns detail page... ");
    {
        size_t n = store_handle_request("GET", "/store/product/1", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "ZCL23 Access Token") != NULL);
        ok = ok && (strstr((char *)resp, "customer_addr") != NULL);
        ok = ok && (strstr((char *)resp, "/store/buy/1") != NULL);
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: GET /store/product/999 returns 404... ");
    {
        size_t n = store_handle_request("GET", "/store/product/999", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Create order ─────────────────────────────────────── */

    printf("store: POST /store/buy/1 creates order... ");
    {
        const char *body = "customer_addr=t1TestAddr123";
        size_t n = store_handle_request("POST", "/store/buy/1",
                                         (const uint8_t *)body, strlen(body),
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Order #") != NULL);
        ok = ok && (strstr((char *)resp, "t1TestAddr123") != NULL);
        ok = ok && (strstr((char *)resp, "0.01000000 ZCL") != NULL);
        if (ok) printf("OK (%zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: POST /store/buy/999 returns 404... ");
    {
        const char *body = "customer_addr=t1Test";
        size_t n = store_handle_request("POST", "/store/buy/999",
                                         (const uint8_t *)body, strlen(body),
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Order status ─────────────────────────────────────── */

    printf("store: GET /store/order/1 returns status page... ");
    {
        size_t n = store_handle_request("GET", "/store/order/1", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Order #1") != NULL);
        ok = ok && (strstr((char *)resp, "Pending") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: GET /store/order/999 returns 404... ");
    {
        size_t n = store_handle_request("GET", "/store/order/999", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── ZSLP token operations ────────────────────────────── */

    printf("store: zslp_create_token returns token_id... ");
    {
        const char *tid = zslp_create_token(test_datadir, "TESTCOIN",
                                             "Test Coin", 8, 1000000);
        bool ok = (tid != NULL) && (strlen(tid) > 0);
        if (ok) printf("OK (token_id=%s)\n", tid);
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: zslp_mint credits tokens... ");
    {
        bool ok = zslp_mint(test_datadir, "TESTCOIN", "t1Buyer123", 500);
        uint64_t bal = zslp_balance(test_datadir, "TESTCOIN", "t1Buyer123");
        ok = ok && (bal == 500);
        if (ok) printf("OK (balance=500)\n");
        else { printf("FAIL (balance=%llu)\n", (unsigned long long)bal); failures++; }
    }

    printf("store: zslp_mint accumulates... ");
    {
        zslp_mint(test_datadir, "TESTCOIN", "t1Buyer123", 250);
        uint64_t bal = zslp_balance(test_datadir, "TESTCOIN", "t1Buyer123");
        bool ok = (bal == 750);
        if (ok) printf("OK (balance=750)\n");
        else { printf("FAIL (balance=%llu)\n", (unsigned long long)bal); failures++; }
    }

    printf("store: zslp_balance returns 0 for unknown addr... ");
    {
        uint64_t bal = zslp_balance(test_datadir, "TESTCOIN", "t1Nobody");
        bool ok = (bal == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: zslp_balance returns 0 for unknown token... ");
    {
        uint64_t bal = zslp_balance(test_datadir, "NOTOKEN", "t1Buyer123");
        bool ok = (bal == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: zslp_send transfers tokens... ");
    {
        bool ok = zslp_send(test_datadir, "TESTCOIN", "t1Seller456", 100);
        uint64_t bal = zslp_balance(test_datadir, "TESTCOIN", "t1Seller456");
        ok = ok && (bal == 100);
        if (ok) printf("OK (t1Seller456 balance=100)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: zslp_generate_payment_address works... ");
    {
        char addr[128] = "";
        bool ok = zslp_generate_payment_address(test_datadir, addr, sizeof(addr));
        ok = ok && (strlen(addr) > 0);
        ok = ok && (strstr(addr, "zs1_pay_") != NULL);
        if (ok) printf("OK (%s)\n", addr);
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Token-gated access ───────────────────────────────── */

    printf("store: token-gated access denied without tokens... ");
    {
        size_t n = store_handle_request("GET",
            "/store/access?addr=t1Nobody&token=TESTCOIN",
            NULL, 0, resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "403") != NULL);
        ok = ok && (strstr((char *)resp, "Access Denied") != NULL);
        if (ok) printf("OK (403)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: token-gated access granted with tokens... ");
    {
        size_t n = store_handle_request("GET",
            "/store/access?addr=t1Buyer123&token=TESTCOIN",
            NULL, 0, resp, sizeof(resp), test_datadir);
        bool ok = (n > 0) && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Premium Service") != NULL);
        if (ok) printf("OK (200)\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── NULL safety ──────────────────────────────────────── */

    printf("store: NULL path returns 0... ");
    {
        size_t n = store_handle_request("GET", NULL, NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: zslp NULL params safe... ");
    {
        bool ok = true;
        ok = ok && (zslp_create_token(NULL, "X", "X", 0, 0) == NULL);
        ok = ok && (zslp_create_token(test_datadir, NULL, "X", 0, 0) == NULL);
        ok = ok && (zslp_balance(NULL, "X", "X") == 0);
        ok = ok && (zslp_balance(test_datadir, NULL, "X") == 0);
        ok = ok && (!zslp_mint(NULL, "X", "X", 1));
        ok = ok && (!zslp_mint(test_datadir, NULL, "X", 1));
        ok = ok && (!zslp_send(NULL, "X", "X", 1));
        char addr[128];
        ok = ok && (!zslp_generate_payment_address(NULL, addr, sizeof(addr)));
        ok = ok && (zslp_check_payment(NULL, "X", 0) == 0);
        if (ok) printf("OK (all NULL cases safe)\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Payment processor ────────────────────────────────── */

    printf("store: store_process_payments runs safely... ");
    {
        store_process_payments(test_datadir);
        store_process_payments(NULL);
        printf("OK (no crash)\n");
    }

    /* ── Multiple products with different tokens ──────────── */

    printf("store: multiple token types isolated... ");
    {
        zslp_create_token(test_datadir, "TOKEN_A", "Token A", 0, 1000);
        zslp_create_token(test_datadir, "TOKEN_B", "Token B", 0, 2000);
        zslp_mint(test_datadir, "TOKEN_A", "t1Multi", 100);
        zslp_mint(test_datadir, "TOKEN_B", "t1Multi", 200);
        uint64_t a = zslp_balance(test_datadir, "TOKEN_A", "t1Multi");
        uint64_t b = zslp_balance(test_datadir, "TOKEN_B", "t1Multi");
        bool ok = (a == 100) && (b == 200);
        if (ok) printf("OK (A=100, B=200)\n");
        else { printf("FAIL (A=%llu, B=%llu)\n",
                       (unsigned long long)a, (unsigned long long)b); failures++; }
    }

    /* ── ZSLP input validation ────────────────────────────── */

    printf("store: reject ticker > 10 chars... ");
    {
        const char *tid = zslp_create_token(test_datadir, "ABCDEFGHIJK",
                                             "Valid Name", 0, 1000);
        bool ok = (tid == NULL);
        if (ok) printf("OK (rejected)\n");
        else { printf("FAIL (should reject)\n"); failures++; }
    }

    printf("store: reject empty ticker... ");
    {
        const char *tid = zslp_create_token(test_datadir, "",
                                             "Valid Name", 0, 1000);
        bool ok = (tid == NULL);
        if (ok) printf("OK (rejected)\n");
        else { printf("FAIL (should reject)\n"); failures++; }
    }

    printf("store: reject decimals > 8... ");
    {
        const char *tid = zslp_create_token(test_datadir, "GOOD",
                                             "Good Token", 9, 1000);
        bool ok = (tid == NULL);
        if (ok) printf("OK (rejected)\n");
        else { printf("FAIL (should reject)\n"); failures++; }
    }

    printf("store: reject amount=0 mint... ");
    {
        bool ok = !zslp_mint(test_datadir, "TESTCOIN", "t1Buyer123", 0);
        if (ok) printf("OK (rejected)\n");
        else { printf("FAIL (should reject)\n"); failures++; }
    }

    printf("store: reject empty recipient address on mint... ");
    {
        bool ok = !zslp_mint(test_datadir, "TESTCOIN", "", 100);
        if (ok) printf("OK (rejected)\n");
        else { printf("FAIL (should reject)\n"); failures++; }
    }

    /* ── End-to-end purchase flow ────────────────────────────── */

    printf("store: e2e: GET /store lists 3 products... ");
    {
        size_t n = store_handle_request("GET", "/store", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "ZCL23 Access Token") != NULL);
        ok = ok && (strstr((char *)resp, "VPN Credit") != NULL);
        ok = ok && (strstr((char *)resp, "Storage") != NULL);
        if (ok) printf("OK (3 products listed)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: e2e: GET /store/product/1 shows price + form... ");
    {
        size_t n = store_handle_request("GET", "/store/product/1", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "0.01000000 ZCL") != NULL);
        ok = ok && (strstr((char *)resp, "customer_addr") != NULL);
        ok = ok && (strstr((char *)resp, "<form") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: e2e: POST /store/buy/1 creates order with z-addr... ");
    {
        const char *body = "customer_addr=t1Test";
        size_t n = store_handle_request("POST", "/store/buy/1",
                                         (const uint8_t *)body, strlen(body),
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Order #") != NULL);
        ok = ok && (strstr((char *)resp, "t1Test") != NULL);
        ok = ok && (strstr((char *)resp, "zs1_order_") != NULL);
        if (ok) printf("OK (z-address generated)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: e2e: GET /store/order shows Pending status... ");
    {
        /* Order created above; query the latest order */
        size_t n = store_handle_request("GET", "/store/order/2", NULL, 0,
                                         resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Pending") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("store: e2e: mint ZCL23ACCESS + verify gated access 200... ");
    {
        zslp_create_token(test_datadir, "ZCL23ACCESS", "Access Token", 0, 1000);
        zslp_mint(test_datadir, "ZCL23ACCESS", "t1Test", 10);
        size_t n = store_handle_request("GET",
            "/store/access?addr=t1Test&token=ZCL23ACCESS",
            NULL, 0, resp, sizeof(resp), test_datadir);
        bool ok = (n > 0);
        ok = ok && (strstr((char *)resp, "200 OK") != NULL);
        ok = ok && (strstr((char *)resp, "Premium Service") != NULL);
        if (ok) printf("OK (access granted)\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── ZSLP edge cases ─────────────────────────────────────── */

    printf("store: zslp multiple mints accumulate... ");
    {
        zslp_create_token(test_datadir, "ACCUM", "Accumulate", 0, 10000);
        zslp_mint(test_datadir, "ACCUM", "t1Accum", 100);
        zslp_mint(test_datadir, "ACCUM", "t1Accum", 200);
        zslp_mint(test_datadir, "ACCUM", "t1Accum", 300);
        uint64_t bal = zslp_balance(test_datadir, "ACCUM", "t1Accum");
        bool ok = (bal == 600);
        if (ok) printf("OK (balance=600)\n");
        else { printf("FAIL (balance=%llu)\n", (unsigned long long)bal); failures++; }
    }

    printf("store: zslp separate addresses keep independent balances... ");
    {
        zslp_create_token(test_datadir, "SPLIT", "Split Token", 0, 10000);
        zslp_mint(test_datadir, "SPLIT", "t1Alice", 100);
        zslp_mint(test_datadir, "SPLIT", "t1Bob", 250);
        uint64_t a = zslp_balance(test_datadir, "SPLIT", "t1Alice");
        uint64_t b = zslp_balance(test_datadir, "SPLIT", "t1Bob");
        bool ok = (a == 100) && (b == 250);
        if (ok) printf("OK (Alice=100, Bob=250)\n");
        else { printf("FAIL (Alice=%llu, Bob=%llu)\n",
                       (unsigned long long)a, (unsigned long long)b); failures++; }
    }

    printf("store: zslp balance 0 for existing token, missing addr... ");
    {
        uint64_t bal = zslp_balance(test_datadir, "SPLIT", "t1Ghost");
        bool ok = (bal == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (balance=%llu)\n", (unsigned long long)bal); failures++; }
    }

    printf("store: zslp two tokens with different tickers independent... ");
    {
        zslp_create_token(test_datadir, "COIN_X", "Coin X", 2, 5000);
        zslp_create_token(test_datadir, "COIN_Y", "Coin Y", 4, 9000);
        zslp_mint(test_datadir, "COIN_X", "t1Holder", 42);
        zslp_mint(test_datadir, "COIN_Y", "t1Holder", 99);
        uint64_t x = zslp_balance(test_datadir, "COIN_X", "t1Holder");
        uint64_t y = zslp_balance(test_datadir, "COIN_Y", "t1Holder");
        bool ok = (x == 42) && (y == 99);
        /* Cross-check: minting one doesn't affect the other */
        zslp_mint(test_datadir, "COIN_X", "t1Holder", 8);
        x = zslp_balance(test_datadir, "COIN_X", "t1Holder");
        y = zslp_balance(test_datadir, "COIN_Y", "t1Holder");
        ok = ok && (x == 50) && (y == 99);
        if (ok) printf("OK (X=50, Y=99)\n");
        else { printf("FAIL (X=%llu, Y=%llu)\n",
                       (unsigned long long)x, (unsigned long long)y); failures++; }
    }

    /* ── Template engine tests ────────────────────────────────── */

    printf("template: basic variable substitution... ");
    {
        char out[256];
        struct template_var vars[] = {
            { "name", "Alice" },
            { "greeting", "Hello" },
        };
        size_t n = template_render("{{greeting}}, {{name}}!",
            vars, 2, out, sizeof(out));
        bool ok = (n > 0) && (strcmp(out, "Hello, Alice!") == 0);
        if (ok) printf("OK (%s)\n", out);
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    printf("template: HTML escaping of < > & \" '... ");
    {
        char out[256];
        struct template_var vars[] = {
            { "val", "<b>A&B \"C\" 'D'</b>" },
        };
        size_t n = template_render("{{val}}", vars, 1, out, sizeof(out));
        bool ok = (n > 0);
        ok = ok && (strstr(out, "&lt;b&gt;") != NULL);
        ok = ok && (strstr(out, "&amp;") != NULL);
        ok = ok && (strstr(out, "&quot;") != NULL);
        ok = ok && (strstr(out, "&#39;") != NULL);
        ok = ok && (strstr(out, "<b>") == NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    printf("template: triple-brace raw output (no escaping)... ");
    {
        char out[256];
        struct template_var vars[] = {
            { "html", "<b>bold</b>" },
        };
        size_t n = template_render("{{{html}}}", vars, 1, out, sizeof(out));
        bool ok = (n > 0) && (strcmp(out, "<b>bold</b>") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    printf("template: missing variable leaves placeholder... ");
    {
        char out[256];
        struct template_var vars[] = {
            { "exists", "yes" },
        };
        size_t n = template_render("{{exists}} {{missing}}",
            vars, 1, out, sizeof(out));
        bool ok = (n > 0);
        ok = ok && (strstr(out, "yes") != NULL);
        ok = ok && (strstr(out, "{{missing}}") != NULL);
        if (ok) printf("OK (%s)\n", out);
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    printf("template: empty template returns empty... ");
    {
        char out[64];
        size_t n = template_render("", NULL, 0, out, sizeof(out));
        bool ok = (n == 0) && (out[0] == '\0');
        if (ok) printf("OK\n");
        else { printf("FAIL (n=%zu)\n", n); failures++; }
    }

    printf("template: NULL safety... ");
    {
        char out[64];
        bool ok = true;
        ok = ok && (template_render(NULL, NULL, 0, out, sizeof(out)) == 0);
        ok = ok && (template_render("hello", NULL, 0, NULL, 0) == 0);
        ok = ok && (template_render("hello", NULL, 0, out, 0) == 0);
        /* NULL vars with non-zero count: no crash, placeholders unchanged */
        size_t n = template_render("{{x}}", NULL, 0, out, sizeof(out));
        ok = ok && (n > 0) && (strcmp(out, "{{x}}") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("template: mixed escaped and raw in same template... ");
    {
        char out[512];
        struct template_var vars[] = {
            { "safe", "<script>alert(1)</script>" },
            { "raw", "<div class='x'>" },
        };
        size_t n = template_render("{{{raw}}}{{safe}}{{{raw}}}",
            vars, 2, out, sizeof(out));
        bool ok = (n > 0);
        ok = ok && (strstr(out, "<div class='x'>") != NULL);
        ok = ok && (strstr(out, "&lt;script&gt;") != NULL);
        ok = ok && (strstr(out, "<script>") == NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    printf("template: value with NULL treated as empty string... ");
    {
        char out[64];
        struct template_var vars[] = {
            { "key", NULL },
        };
        size_t n = template_render("[{{key}}]", vars, 1, out, sizeof(out));
        bool ok = (n > 0) && (strcmp(out, "[]") == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (%s)\n", out); failures++; }
    }

    if (failures > 0)
        printf("Store: debug datadir preserved at %s\n", test_datadir);
    else
        cleanup_datadir();

    printf("Store + ZSLP + Template: %d failures\n", failures);
    return failures;
}
