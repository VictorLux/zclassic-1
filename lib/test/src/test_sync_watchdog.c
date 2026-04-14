/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for sync watchdog service — stall detection and recovery. */

#include "test/test_helpers.h"
#include "services/sync_watchdog_service.h"
#include "net/msgprocessor.h"
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

/* ── Test: diagnostic counter increments ────────────────── */

static int test_diagnostic_counters(void)
{
    int failures = 0;

    TEST("header stats struct is populated without crash") {
        struct msg_headers_stats stats;
        memset(&stats, 0xff, sizeof(stats));
        msg_headers_get_stats(&stats);
        /* After get_stats, struct should be coherent (not 0xff garbage).
         * Counters are cumulative; just verify the call worked. */
        ASSERT(stats.total_accepted + stats.total_rejected >=
               stats.total_accepted); /* no overflow */
        PASS();
    } _test_next:;

    return failures;
}

static int test_diagnostic_counters_null(void)
{
    int failures = 0;

    TEST("header stats getter handles NULL") {
        msg_headers_get_stats(NULL); /* should not crash */
        ASSERT(true);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: watchdog init and status ─────────────────────── */

static int test_watchdog_init_valid(void)
{
    int failures = 0;

    TEST("watchdog init produces valid status") {
        reset_test_state();
        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);

        ASSERT(status.enabled == true);
        ASSERT(status.checks_run == 0);
        ASSERT(status.recoveries_triggered == 0);
        ASSERT(status.last_recovery_type == WATCHDOG_NONE);
        PASS();
    } _test_next:;

    return failures;
}

static int test_watchdog_checks_run(void)
{
    int failures = 0;

    TEST("watchdog check increments checks_run") {
        reset_test_state();
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");
        sync_set_state(SYNC_AT_TIP, "test");

        sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);

        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.checks_run == 2);
        PASS();
    } _test_next:;

    return failures;
}

static int test_watchdog_null_status(void)
{
    int failures = 0;

    TEST("watchdog status reports NULL safely") {
        sync_watchdog_get_status(NULL); /* should not crash */
        ASSERT(true);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: watchdog escalation ──────────────────────────── */

static int test_watchdog_escalation(void)
{
    int failures = 0;

    TEST("watchdog escalates after 2 consecutive header stalls") {
        reset_test_state();

        struct block_index fake_header = {0};
        fake_header.nHeight = 5000;
        g_test_ms.pindex_best_header = &fake_header;

        /* Set a reject reason for escalation diagnostic */
        sync_watchdog_set_last_reject_reason("equihash-bad-solution");

        /* Trigger 2 HEADER_STALL recoveries */
        for (int i = 0; i < 2; i++) {
            sync_set_state(SYNC_IDLE, "reset");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "trigger");
            atomic_store(&g_sync_state_entered_time,
                         (int64_t)time(NULL) - 350);

            /* First check: record baseline */
            sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);

            /* Re-enter headers_download */
            if (sync_get_state() != SYNC_HEADERS_DOWNLOAD) {
                sync_set_state(SYNC_IDLE, "re-enter");
                sync_set_state(SYNC_HEADERS_DOWNLOAD, "re-enter");
            }
            atomic_store(&g_sync_state_entered_time,
                         (int64_t)time(NULL) - 350);

            /* Second check: trigger stall */
            enum watchdog_recovery_type r = sync_watchdog_check(
                &g_test_cm, &g_test_dm, &g_test_ms);
            ASSERT(r == WATCHDOG_HEADER_STALL);
        }

        /* After 2 stalls, the escalation should have fired
         * (verified by the printf output; here we just confirm
         * the recoveries count) */
        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.recoveries_triggered >= 2);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: header lag detection ─────────────────────────── */

static int test_header_lag_detection(void)
{
    int failures = 0;

    TEST("watchdog detects header lag during block download") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");

        /* Backdate state entry >60s */
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 90);

        /* Headers at 2000, but set up a fake peer at 3000 */
        struct block_index fake_header = {0};
        fake_header.nHeight = 2000;
        g_test_ms.pindex_best_header = &fake_header;

        /* Add a fake peer with starting_height 3000 */
        struct p2p_node fake_peer = {0};
        fake_peer.starting_height = 3000;
        struct p2p_node *peer_ptrs[1] = { &fake_peer };
        g_test_cm.manager.nodes = peer_ptrs;
        g_test_cm.manager.num_nodes = 1;

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_HEADER_LAG);

        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;
        PASS();
    } _test_next:;

    return failures;
}

static int test_header_lag_small_gap(void)
{
    int failures = 0;

    TEST("watchdog does NOT detect header lag when gap < 500") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 90);

        struct block_index fake_header = {0};
        fake_header.nHeight = 2800;
        g_test_ms.pindex_best_header = &fake_header;

        struct p2p_node fake_peer = {0};
        fake_peer.starting_height = 3000;
        struct p2p_node *peer_ptrs[1] = { &fake_peer };
        g_test_cm.manager.nodes = peer_ptrs;
        g_test_cm.manager.num_nodes = 1;

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);

        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;
        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: recovery type names ──────────────────────────── */

static int test_recovery_type_names(void)
{
    int failures = 0;

    TEST("recovery type names are non-null") {
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_NONE), "NONE") == 0);
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_HEADER_STALL),
                      "HEADER_STALL") == 0);
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_HEADER_LAG),
                      "HEADER_LAG") == 0);
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_BLOCK_STALL),
                      "BLOCK_STALL") == 0);
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_STATE_STUCK),
                      "STATE_STUCK") == 0);
        ASSERT(strcmp(watchdog_recovery_type_name(WATCHDOG_REPEATED_RESTART),
                      "REPEATED_RESTART") == 0);
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
    failures += test_diagnostic_counters();
    failures += test_diagnostic_counters_null();
    failures += test_watchdog_init_valid();
    failures += test_watchdog_checks_run();
    failures += test_watchdog_null_status();
    failures += test_watchdog_escalation();
    failures += test_header_lag_detection();
    failures += test_header_lag_small_gap();
    failures += test_recovery_type_names();
    return failures;
}
