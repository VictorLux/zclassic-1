/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User stories: Node Command Center
 * The node page should make users feel like spacecraft commanders. */

#include "test/spec.h"
#include "controllers/wallet_view_controller.h"
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

int spec_wallet_node(void)
{
    wallet_view_init(NULL);

    FEATURE("Node — Command Center") {
        STORY("node page shows block height and peers") {
            GIVEN("node page loads")
                GET("/wallet/node");
            THEN("command center heading visible")
                EXPECT(has("Command Center"));
            THEN("block height stat shown")
                EXPECT(has("Block Height"));
            THEN("peers stat shown")
                EXPECT(has("Connected Peers"));
            PASS();
        }

        STORY("node page shows network protocol info") {
            GIVEN("node page loads")
                GET("/wallet/node");
            THEN("version string shown")
                EXPECT(has("ZClassic-C23"));
            THEN("protocol flags shown")
                EXPECT(has("NODE_NETWORK"));
            PASS();
        }

        STORY("node page has Tor section") {
            GIVEN("node page loads")
                GET("/wallet/node");
            THEN("Tor section exists")
                EXPECT(has("Tor Hidden Service"));
            PASS();
        }

        STORY("node page has peer table") {
            GIVEN("node page loads")
                GET("/wallet/node");
            THEN("peer table headers exist")
                EXPECT(has("Address"));
            THEN("connecting message for empty table")
                EXPECT(has("Connecting to network"));
            PASS();
        }

        STORY("node page links to coin audit and explorer") {
            GIVEN("node page loads")
                GET("/wallet/node");
            THEN("coin audit link exists")
                EXPECT(has("Coin Audit"));
            THEN("explorer link exists")
                EXPECT(has("Block Explorer"));
            PASS();
        }
    }

    FEATURE("Node — Coins Audit") {
        STORY("coins page has breadcrumb or loading") {
            GIVEN("coins page loads")
                GET("/wallet/coins");
            THEN("breadcrumb or loading state")
                EXPECT(has("Coin Audit") || has("Wallet Loading"));
            PASS();
        }
    }

    SPEC_SUMMARY();
    return SPEC_FAILURES();
}
