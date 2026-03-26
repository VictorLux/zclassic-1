/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User stories: Wallet Dashboard
 * The dashboard is the first thing users see. It must show balance,
 * privacy status, and clear paths to every action. */

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

static bool has(const char *needle) {
    return strstr((char *)_resp, needle) != NULL;
}

int spec_wallet_dashboard(void)
{
    wallet_view_init(NULL);

    FEATURE("Dashboard — Balance Display") {
        STORY("user sees balance or loading state") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("page is a valid HTML response")
                EXPECT(has("HTTP/1.1 200 OK"));
            THEN("shows ZCL or loading message")
                EXPECT(has("ZCL") || has("Wallet Loading"));
            PASS();
        }
    }

    FEATURE("Dashboard — Privacy Meter") {
        STORY("privacy meter or loading state shown") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("either privacy meter or loading")
                EXPECT(has("private") || has("Wallet Loading"));
            PASS();
        }

        STORY("page links to shield or shows loading") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("shield link or loading state")
                EXPECT(has("/wallet/shield") || has("Wallet Loading"));
            PASS();
        }
    }

    FEATURE("Dashboard — Navigation") {
        STORY("all 5 nav tabs are present") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("Home tab exists")
                EXPECT(has(">Home<"));
            THEN("Send tab exists")
                EXPECT(has(">Send<"));
            THEN("Receive tab exists")
                EXPECT(has(">Receive<"));
            THEN("History tab exists")
                EXPECT(has(">History<"));
            THEN("Node tab exists")
                EXPECT(has(">Node<"));
            PASS();
        }

        STORY("Home tab is highlighted on dashboard") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("Home tab has active class")
                EXPECT(has("class='active'>Home"));
            PASS();
        }

        STORY("send and receive buttons are prominent") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("Send button links to send page")
                EXPECT(has("href='/wallet/send'"));
            THEN("Receive button links to receive page")
                EXPECT(has("href='/wallet/receive'"));
            PASS();
        }
    }

    FEATURE("Dashboard — Node Status") {
        STORY("compact node strip shows at bottom") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("node strip links to command center")
                EXPECT(has("/wallet/node"));
            PASS();
        }
    }

    FEATURE("Dashboard — Live Updates") {
        STORY("page has polling JS for live balance") {
            GIVEN("the dashboard loads")
                GET("/wallet");
            THEN("pulse endpoint is polled")
                EXPECT(has("api/wallet/pulse"));
            THEN("interval is set")
                EXPECT(has("setInterval"));
            PASS();
        }

        STORY("pulse endpoint returns JSON") {
            GIVEN("pulse endpoint is called")
                GET("/api/wallet/pulse");
            THEN("response contains height")
                EXPECT(has("height"));
            THEN("response contains balance")
                EXPECT(has("balance"));
            PASS();
        }
    }

    SPEC_SUMMARY();
    return SPEC_FAILURES();
}
