/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for header sync stall detection and recovery:
 * per-peer tracking, stall detection, inbound fallback. */

#include "test/test_helpers.h"
#include "sync/sync_planner.h"

/* Helper: build a minimal p2p_node for testing stall detection. */
static struct p2p_node make_stall_node(int starting_height, bool inbound,
                                       int64_t connected_time)
{
    struct p2p_node n;
    memset(&n, 0, sizeof(n));
    n.id = 1;
    n.state = PEER_SYNCING_HEADERS;
    n.starting_height = starting_height;
    n.last_getheaders_time = 0;
    n.inbound = inbound;
    n.getheaders_stale_count = 0;
    n.time_connected = connected_time;
    n.last_useful_headers_time = 0;
    n.total_headers_delivered = 0;
    return n;
}

int test_header_sync_stall(void)
{
    int failures = 0;

    /* ── 1. last_useful_headers_time updated on accepted headers ── */
    printf("header_sync_stall: tracking fields updated on accept... ");
    {
        struct p2p_node n = make_stall_node(10000, false, 1000);
        syncsvc_note_headers_received(&n, 50);
        bool ok = (n.last_useful_headers_time > 0);
        ok = ok && (n.total_headers_delivered == 50);
        /* Second batch accumulates */
        syncsvc_note_headers_received(&n, 30);
        ok = ok && (n.total_headers_delivered == 80);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 2. Tracking fields NOT updated on zero accepted ────────── */
    printf("header_sync_stall: tracking fields unchanged on reject... ");
    {
        struct p2p_node n = make_stall_node(10000, false, 1000);
        n.last_useful_headers_time = 500;
        n.total_headers_delivered = 10;
        syncsvc_note_headers_received(&n, 0);
        bool ok = (n.last_useful_headers_time == 500);
        ok = ok && (n.total_headers_delivered == 10);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 3. Stale peer disconnect fires after 120s in IBD ────────── */
    printf("header_sync_stall: disconnect stale peer after 120s... ");
    {
        struct p2p_node n = make_stall_node(10000, false, 1000);
        /* 119s: not yet */
        bool at_119 = syncsvc_should_disconnect_stale_header_peer(&n, 100, 1119);
        /* 120s: fire */
        bool at_120 = syncsvc_should_disconnect_stale_header_peer(&n, 100, 1120);
        bool ok = !at_119 && at_120;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 4. Peer with recent useful headers not disconnected ─────── */
    printf("header_sync_stall: active peer not disconnected... ");
    {
        struct p2p_node n = make_stall_node(10000, false, 1000);
        n.last_useful_headers_time = 1050;
        n.total_headers_delivered = 100;
        bool ok = !syncsvc_should_disconnect_stale_header_peer(&n, 100, 1100);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 5. Non-IBD peer not disconnected ────────────────────────── */
    printf("header_sync_stall: non-IBD peer not disconnected... ");
    {
        struct p2p_node n = make_stall_node(1000, false, 1000);
        /* height 900 = within 144 of starting_height, not IBD */
        bool ok = !syncsvc_should_disconnect_stale_header_peer(&n, 900, 9999);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 6. Header sync stall detection fires ────────────────────── */
    printf("header_sync_stall: stall detected after 120s no advance... ");
    {
        bool at_119 = syncsvc_is_header_sync_stalled(
            SYNC_HEADERS_DOWNLOAD, 5000, 1000, 1119);
        bool at_120 = syncsvc_is_header_sync_stalled(
            SYNC_HEADERS_DOWNLOAD, 5000, 1000, 1120);
        bool ok = !at_119 && at_120;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 7. Stall not detected in wrong sync state ───────────────── */
    printf("header_sync_stall: no stall in SYNC_BLOCKS_DOWNLOAD... ");
    {
        bool stalled = syncsvc_is_header_sync_stalled(
            SYNC_BLOCKS_DOWNLOAD, 5000, 1000, 9999);
        bool ok = !stalled;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 8. Stall not detected with zero last_advance_time ───────── */
    printf("header_sync_stall: no stall with zero advance time... ");
    {
        bool stalled = syncsvc_is_header_sync_stalled(
            SYNC_HEADERS_DOWNLOAD, 5000, 0, 9999);
        bool ok = !stalled;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 9. Inbound fallback: normally skipped ───────────────────── */
    printf("header_sync_stall: inbound peer skipped normally... ");
    {
        struct p2p_node n = make_stall_node(10000, true, 0);
        bool ok = !syncsvc_should_request_headers_with_fallback(
            &n, 100, 9999, false);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 10. Inbound fallback: allowed during stall ──────────────── */
    printf("header_sync_stall: inbound peer allowed during stall... ");
    {
        struct p2p_node n = make_stall_node(10000, true, 0);
        n.state = PEER_ACTIVE;
        bool ok = syncsvc_should_request_headers_with_fallback(
            &n, 100, 9999, true);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* ── 11. Outbound peer still works with fallback function ────── */
    printf("header_sync_stall: outbound peer works with fallback fn... ");
    {
        struct p2p_node n = make_stall_node(10000, false, 0);
        bool ok = syncsvc_should_request_headers_with_fallback(
            &n, 100, 15, false);
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
