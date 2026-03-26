/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User stories: Receive Flow
 * Receiving should default to private, with clear copy mechanics. */

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

int spec_wallet_receive(void)
{
    wallet_view_init(NULL);

    FEATURE("Receive — Privacy Default") {
        STORY("private tab is selected by default") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("private tab has active class")
                EXPECT(has("active-z"));
            THEN("tab says recommended")
                EXPECT(has("Private (recommended)"));
            PASS();
        }

        STORY("private pane explains invisibility") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("context text explains privacy")
                EXPECT(has("invisible on the blockchain"));
            PASS();
        }

        STORY("public pane warns about visibility") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("public pane has warning text")
                EXPECT(has("visible to anyone"));
            PASS();
        }
    }

    FEATURE("Receive — Copy Mechanics") {
        STORY("public pane has explicit copy button") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("copy button exists")
                EXPECT(has("Copy Address"));
            THEN("copy JS function exists")
                EXPECT(has("copyAddr"));
            PASS();
        }

        STORY("click-to-copy works on address elements") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("clipboard JS is present")
                EXPECT(has("navigator.clipboard"));
            PASS();
        }
    }

    FEATURE("Receive — QR Code") {
        STORY("QR code is rendered as SVG") {
            GIVEN("receive page loads")
                GET("/wallet/receive");
            THEN("SVG element exists")
                EXPECT(has("<svg"));
            PASS();
        }
    }

    SPEC_SUMMARY();
    return SPEC_FAILURES();
}
