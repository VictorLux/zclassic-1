/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User stories: Send Flow
 * Sending ZCL must be clear, safe, and privacy-aware. */

#include "test/spec.h"
#include "controllers/wallet_view_controller.h"
#include "util/template.h"
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

int spec_wallet_send(void)
{
    wallet_view_init(NULL);

    FEATURE("Send — Form") {
        STORY("send page shows spendable balance") {
            GIVEN("send page loads")
                GET("/wallet/send");
            THEN("spendable balance is displayed")
                EXPECT(has("Spendable balance"));
            THEN("amount input exists")
                EXPECT(has("id='amt'"));
            THEN("address input exists")
                EXPECT(has("id='addr'"));
            PASS();
        }

        STORY("send page has fee display") {
            GIVEN("send page loads")
                GET("/wallet/send");
            THEN("fee is shown")
                EXPECT(has("Network fee"));
            THEN("fee amount visible")
                EXPECT(has("0.0001"));
            PASS();
        }

        STORY("send form has address validation JS") {
            GIVEN("send page loads")
                GET("/wallet/send");
            THEN("validation function exists")
                EXPECT(has("validateSend"));
            THEN("privacy hint element exists")
                EXPECT(has("privacy-hint"));
            PASS();
        }

        STORY("send tab is highlighted") {
            GIVEN("send page loads")
                GET("/wallet/send");
            THEN("Send tab has active class")
                EXPECT(has("class='active'>Send"));
            PASS();
        }
    }

    FEATURE("Send — Review") {
        STORY("review page shows breadcrumb") {
            GIVEN("a send is reviewed")
                POST("/wallet/send/review",
                    "address=t1ExampleAddr123456789012345&amount=0.5");
            THEN("breadcrumb shows Send > Review path")
                /* Partial {{> breadcrumb}} renders with parent vars */
                EXPECT(has("Send") && has("Review"));
            PASS();
        }

        STORY("invalid address shows validation error") {
            GIVEN("review with bad address")
                POST("/wallet/send/review", "address=bad&amount=0.5");
            THEN("error message shown")
                EXPECT(has("Invalid") || has("too short"));
            PASS();
        }

        STORY("zero amount shows validation error") {
            GIVEN("review with zero amount")
                POST("/wallet/send/review",
                    "address=t1ExampleAddr123456789012345&amount=0");
            THEN("error message shown")
                EXPECT(has("Invalid"));
            PASS();
        }
    }

    FEATURE("Send — Privacy Awareness") {
        STORY("send form shows privacy hint for z-address input") {
            GIVEN("send page loads")
                GET("/wallet/send");
            THEN("privacy hint area exists for real-time feedback")
                EXPECT(has("id='privacy-hint'"));
            THEN("JS checks for zs1 prefix")
                EXPECT(has("zs1"));
            PASS();
        }
    }

    SPEC_SUMMARY();
    return SPEC_FAILURES();
}
