/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for sync watchdog service — stall detection and recovery. */

#include "test/test_helpers.h"
#include "services/sync_watchdog_service.h"
#include "net/download.h"
#include "validation/main_state.h"
#include <string.h>
#include <time.h>

extern _Atomic int64_t g_sync_state_entered_time;

/* ── Helpers ──────────────────────────────────────────────── */

static struct download_manager g_test_dm;
static struct main_state g_test_ms;
static struct connman g_test_cm;

static void reset_test_state(void)
{
    memset(&g_test_dm, 0, sizeof(g_test_dm));
    memset(&g_test_ms, 0, sizeof(g_test_ms));
    memset(&g_test_cm, 0, sizeof(g_test_cm));
    zcl_mutex_init(&g_test_dm.cs);
    zcl_mutex_init(&g_test_cm.manager.cs_nodes);

    sync_watchdog_init();
    sync_set_state(SYNC_IDLE, "test reset");
}

/* ── Test: header stall detection ────────────────────────── */

static int test_header_stall_detection(void)
{
    int failures = 0;

    TEST("watchdog detects header stall after 300s") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");

        /* Set up a best header that won't advance */
        struct block_index fake_header = {0};
        fake_header.nHeight = 2000;
        g_test_ms.pindex_best_header = &fake_header;

        /* Backdate state entry >300s */
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 350);

        /* First check: last_header_height=-1, current=2000 → records baseline */
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);

        /* Re-enter headers_download and backdate (recovery may have changed state) */
        if (sync_get_state() != SYNC_HEADERS_DOWNLOAD) {
            sync_set_state(SYNC_IDLE, "re-enter");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "re-enter");
        }
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 350);

        /* Second check: last_header_height=2000, current=2000 → stall */
        r = sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_HEADER_STALL);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: block stall detection ─────────────────────────── */

static int test_block_stall_detection(void)
{
    int failures = 0;

    TEST("watchdog detects block stall after 300s") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 350);

        /* active_chain_height returns 0 for zeroed main_state.
         * First check: last_chain_height=-1, current=0 → baseline recorded */
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);

        /* Re-enter blocks_download if recovery changed state */
        if (sync_get_state() != SYNC_BLOCKS_DOWNLOAD) {
            sync_set_state(SYNC_IDLE, "re-enter");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "re-enter");
            sync_set_state(SYNC_BLOCKS_DOWNLOAD, "re-enter");
        }
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 350);

        /* Second check: last_chain_height=0, current=0 → stall */
        r = sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_BLOCK_STALL);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: state stuck detection ─────────────────────────── */

static int test_state_stuck_detection(void)
{
    int failures = 0;

    TEST("watchdog detects state stuck after 600s") {
        reset_test_state();

        sync_set_state(SYNC_FINDING_PEERS, "test");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 650);

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_STATE_STUCK);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: repeated restart circuit breaker ──────────────── */

static int test_repeated_restart_circuit_breaker(void)
{
    int failures = 0;

    TEST("watchdog stops after >3 recoveries in 30min") {
        reset_test_state();

        /* Trigger 4 recoveries via STATE_STUCK */
        for (int i = 0; i < 4; i++) {
            sync_set_state(SYNC_IDLE, "reset");
            sync_set_state(SYNC_FINDING_PEERS, "trigger");
            atomic_store(&g_sync_state_entered_time,
                         (int64_t)time(NULL) - 650);
            sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        }

        /* Now try to trigger another — should be blocked */
        sync_set_state(SYNC_IDLE, "reset");
        sync_set_state(SYNC_FINDING_PEERS, "trigger again");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 650);
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);

        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.recoveries_triggered >= 3);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: SYNC_AT_TIP is exempt ─────────────────────────── */

static int test_at_tip_exempt(void)
{
    int failures = 0;

    TEST("watchdog does not trigger for SYNC_AT_TIP") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
        sync_set_state(SYNC_AT_TIP, "test");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 3600);

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test runner ─────────────────────────────────────────── */

int test_sync_watchdog(void)
{
    int failures = 0;
    printf("\n=== Sync Watchdog Tests ===\n");
    failures += test_header_stall_detection();
    failures += test_block_stall_detection();
    failures += test_state_stuck_detection();
    failures += test_repeated_restart_circuit_breaker();
    failures += test_at_tip_exempt();
    return failures;
}
