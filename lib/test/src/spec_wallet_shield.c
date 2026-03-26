/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User stories: Shield Flow
 * Shielding should feel like upgrading security, not a complex operation. */

#include "test/spec.h"
#include "controllers/wallet_view_controller.h"
#include <string.h>

static uint8_t _resp[131072];

static size_t GET(const char *path) {
    memset(_resp, 0, sizeof(_resp));
    return wallet_view_handle_request("GET", path, NULL, 0,
                                       _resp, sizeof(_resp));
}

static size_t POST(const char *path, const char *body) {
    memset(_resp, 0, sizeof(_resp));
    return wallet_view_handle_request("POST", path,
        (const uint8_t *)body, body ? strlen(body) : 0,
        _resp, sizeof(_resp));
}

static bool has(const char *needle) {
    return strstr((char *)_resp, needle) != NULL;
}

int spec_wallet_shield(void)
{
    wallet_view_init(NULL);

    FEATURE("Shield — Entry Points") {
        STORY("shield page is reachable from dashboard") {
            GIVEN("dashboard loads")
                GET("/wallet");
            THEN("shield link or loading state")
                EXPECT(has("/wallet/shield") || has("Wallet Loading"));
            PASS();
        }

        STORY("shield form shows breadcrumb") {
            GIVEN("shield page loads")
                GET("/wallet/shield");
            THEN("breadcrumb shows Home > Secure Funds")
                EXPECT(has("Secure Funds"));
            PASS();
        }
    }

    FEATURE("Shield — Amount Form") {
        STORY("shield form shows available balance") {
            GIVEN("shield page loads")
                GET("/wallet/shield");
            THEN("available balance is displayed")
                EXPECT(has("Available"));
            THEN("max button exists")
                EXPECT(has("Max"));
            PASS();
        }

        STORY("shield form has review button") {
            GIVEN("shield page loads")
                GET("/wallet/shield");
            THEN("review button exists")
                EXPECT(has("Review"));
            PASS();
        }
    }

    FEATURE("Shield — Confirmation") {
        STORY("shield confirm page explains the 3 steps") {
            GIVEN("shield confirm loads with amount")
                GET("/wallet/shield?amount=0.5");
            THEN("step 1 explains the process")
                EXPECT(has("Step 1"));
            THEN("amount is displayed")
                EXPECT(has("0.5"));
            PASS();
        }

        STORY("shield confirm has cancel and confirm buttons") {
            GIVEN("shield confirm loads")
                GET("/wallet/shield?amount=0.5");
            THEN("cancel button exists")
                EXPECT(has("Cancel"));
            THEN("confirm button exists")
                EXPECT(has("Confirm"));
            PASS();
        }
    }

    FEATURE("Shield — Error Handling") {
        STORY("shield with zero amount shows form") {
            GIVEN("shield loads with no amount")
                GET("/wallet/shield");
            THEN("amount form is shown")
                EXPECT(has("Amount to Secure"));
            PASS();
        }

        STORY("shield confirm with invalid amount shows error") {
            GIVEN("shield confirm with zero")
                POST("/wallet/shield/confirm", "amount=0");
            THEN("error is shown")
                EXPECT(has("Invalid") || has("error"));
            PASS();
        }
    }

    SPEC_SUMMARY();
    return SPEC_FAILURES();
}
