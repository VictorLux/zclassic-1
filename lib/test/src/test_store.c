/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Store controller + ZSLP token integration tests. */

#include "test/test_helpers.h"
#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include <unistd.h>

static char test_datadir[256];

static void setup_datadir(void)
{
    snprintf(test_datadir, sizeof(test_datadir), "/tmp/zcl_test_store_%d",
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

    if (failures > 0)
        printf("Store: debug datadir preserved at %s\n", test_datadir);
    else
        cleanup_datadir();

    printf("Store + ZSLP: %d failures\n", failures);
    return failures;
}
