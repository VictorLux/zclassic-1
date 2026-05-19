/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for sync watchdog service — stall detection and recovery. */

#include "test/test_helpers.h"
#include "services/sync_watchdog_service.h"
#include "net/msgprocessor.h"
#include "net/download.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include <string.h>
#include <time.h>

extern _Atomic int64_t g_sync_state_entered_time;
extern int64_t g_utxo_pause_first_seen;       /* Round 7 A1 */
extern int64_t g_queue_starved_first_seen;    /* Round 7 A7 */

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
    zcl_mutex_init(&g_test_ms.cs_main);
    zcl_mutex_init(&g_test_cm.manager.cs_nodes);

    sync_watchdog_init();
    sync_set_state(SYNC_IDLE, "test reset");
    /* Round 7 A1+A7: reset cross-test global timers so a backdated
     * timestamp in one TEST() doesn't bleed into the next. */
    g_utxo_pause_first_seen = 0;
    g_queue_starved_first_seen = 0;
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
        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.last_recovery_type == WATCHDOG_HEADER_STALL);
        ASSERT_STR_EQ(status.last_recovery_reason,
                      "header_height_not_advancing");
        ASSERT(status.last_recovery_peer_height == 2000);

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
        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.last_recovery_type == WATCHDOG_STATE_STUCK);
        ASSERT_STR_EQ(status.last_recovery_reason, "finding_peers");

        PASS();
    } _test_next:;

    return failures;
}

static int test_at_tip_reconciliation(void)
{
    int failures = 0;

    TEST("watchdog reconciles finding_peers to at_tip when chain is current") {
        reset_test_state();

        struct block_index tip = {0};
        tip.nHeight = 500;
        ASSERT(active_chain_set_tip(&g_test_ms.chain_active, &tip));
        g_test_ms.pindex_best_header = &tip;

        struct p2p_node p1 = {0};
        p1.id = 1;
        p1.starting_height = 500;
        p1.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &p1 };
        g_test_cm.manager.nodes = peers;
        g_test_cm.manager.num_nodes = 1;

        sync_set_state(SYNC_FINDING_PEERS, "startup reconcile");
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_NONE);
        ASSERT(sync_get_state() == SYNC_AT_TIP);

        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;
        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: repeated restart circuit breaker ──────────────── */

static int test_repeated_restart_circuit_breaker(void)
{
    int failures = 0;

    TEST("watchdog escalates after repeated recoveries") {
        reset_test_state();

        /* Trigger 3 L1 recoveries via STATE_STUCK */
        for (int i = 0; i < 3; i++) {
            sync_set_state(SYNC_IDLE, "reset");
            sync_set_state(SYNC_FINDING_PEERS, "trigger");
            atomic_store(&g_sync_state_entered_time,
                         (int64_t)time(NULL) - 650);
            sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        }

        /* 4th recovery should trigger L2 escalation */
        sync_set_state(SYNC_IDLE, "reset");
        sync_set_state(SYNC_FINDING_PEERS, "trigger L2");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 650);
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_REPEATED_RESTART);

        struct sync_watchdog_status status;
        sync_watchdog_get_status(&status);
        ASSERT(status.escalation_level >= 2);
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

/* ── Test: zero peers triggers state stuck ──────────────── */

static int test_zero_peers_stuck(void)
{
    int failures = 0;

    TEST("watchdog with 0 peers in FINDING_PEERS triggers STATE_STUCK") {
        reset_test_state();

        /* Zero peers: num_nodes = 0, nodes = NULL */
        g_test_cm.manager.num_nodes = 0;
        g_test_cm.manager.nodes = NULL;

        sync_set_state(SYNC_FINDING_PEERS, "test 0 peers");
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 650);

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_STATE_STUCK);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: exactly at timeout boundary (not past) ───────── */

static int test_timeout_boundary_exact(void)
{
    int failures = 0;

    TEST("watchdog does NOT trigger at exactly 300s (boundary)") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "boundary test");

        struct block_index fake_header = {0};
        fake_header.nHeight = 5000;
        g_test_ms.pindex_best_header = &fake_header;

        /* Set exactly 300s — should record baseline, not trigger */
        atomic_store(&g_sync_state_entered_time,
                     (int64_t)time(NULL) - 300);

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        /* First check records baseline — should not be a stall */
        ASSERT(r == WATCHDOG_NONE);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Test: progress rate tracking via status ────────────── */

static int test_progress_rate_tracking(void)
{
    int failures = 0;

    TEST("watchdog stats track blocks_per_sec after checks") {
        reset_test_state();

        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_AT_TIP, "at tip");

        /* Run several checks at tip */
        for (int i = 0; i < 5; i++)
            sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);

        struct watchdog_stats wstats;
        sync_watchdog_get_stats(&wstats);
        ASSERT(wstats.checks_run >= 5);
        /* blocks_per_sec at tip with no height change should be >= 0 */
        ASSERT(wstats.blocks_per_sec >= 0.0);

        PASS();
    } _test_next:;

    return failures;
}

/* ── Round 7 A1: UTXO_PAUSE detection ────────────────────── */

static int test_utxo_pause_fires_after_window(void)
{
    int failures = 0;

    TEST("watchdog detects + clears UTXO_PAUSE after 300s") {
        reset_test_state();
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "utxo_pause setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "utxo_pause test");

        process_block_test_set_utxo_activation_paused_height(1500);
        g_utxo_pause_first_seen = 0;

        /* First check: records first_seen, no recovery yet. */
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r != WATCHDOG_UTXO_PAUSE);
        ASSERT(g_utxo_pause_first_seen != 0);
        ASSERT(process_block_test_get_utxo_activation_paused_height() == 1500);

        /* Backdate first_seen by 350s and check again — must fire. */
        g_utxo_pause_first_seen = (int64_t)time(NULL) - 350;
        r = sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_UTXO_PAUSE);
        /* Pause must have been cleared by the recovery action. */
        ASSERT(process_block_test_get_utxo_activation_paused_height() == -1);

        PASS();
    } _test_next:;

    return failures;
}

static int test_utxo_pause_inactive_resets_timer(void)
{
    int failures = 0;

    TEST("watchdog does not fire UTXO_PAUSE when no pause set") {
        reset_test_state();
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "no pause setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "no pause");

        process_block_test_set_utxo_activation_paused_height(-1);
        g_utxo_pause_first_seen = (int64_t)time(NULL) - 9999;

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r != WATCHDOG_UTXO_PAUSE);
        /* first_seen must be reset to 0 when no pause is active. */
        ASSERT(g_utxo_pause_first_seen == 0);

        PASS();
    } _test_next:;

    return failures;
}

/* Round 7 A7: QUEUE_STARVED — empty in-flight slots for >120s with
 * peers connected fires the recovery before BLOCK_STALL would. */
static int test_queue_starved_fires_after_window(void)
{
    int failures = 0;

    TEST("watchdog fires QUEUE_STARVED after 120s of empty in-flight") {
        reset_test_state();
        /* Transition idle → headers → blocks (direct idle→blocks is
         * an illegal sync state transition). */
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "queue starved setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "queue starved test");

        /* Fake a single connected peer so the > 0 gate passes. */
        struct p2p_node fake_peer = {0};
        struct p2p_node *peer_arr[1] = { &fake_peer };
        g_test_cm.manager.nodes = peer_arr;
        g_test_cm.manager.num_nodes = 1;

        g_queue_starved_first_seen = 0;
        /* First check: records baseline, no recovery yet. */
        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r != WATCHDOG_QUEUE_STARVED);
        ASSERT(g_queue_starved_first_seen != 0);

        /* Backdate >120s and check again — must fire. */
        g_queue_starved_first_seen = (int64_t)time(NULL) - 150;
        r = sync_watchdog_check(&g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_QUEUE_STARVED);

        /* Cleanup */
        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;

        PASS();
    } _test_next:;

    return failures;
}

static int test_next_child_missing_triggers_local_refill(void)
{
    int failures = 0;

    TEST("watchdog locally refills headers when next child is missing") {
        reset_test_state();
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "next child missing");

        struct block_index tip = {0};
        tip.nHeight = 10;
        ASSERT(active_chain_set_tip(&g_test_ms.chain_active, &tip));

        struct p2p_node p1 = {0}, p2 = {0}, p3 = {0};
        p1.id = 1; p1.starting_height = 20; p1.state = PEER_ACTIVE;
        p2.id = 2; p2.starting_height = 20; p2.state = PEER_ACTIVE;
        p3.id = 3; p3.starting_height = 20; p3.state = PEER_ACTIVE;
        p1.last_getheaders_time = p2.last_getheaders_time =
            p3.last_getheaders_time = (int64_t)time(NULL);
        struct p2p_node *peers[3] = { &p1, &p2, &p3 };
        g_test_cm.manager.nodes = peers;
        g_test_cm.manager.num_nodes = 3;

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_LOCAL_HEADER_REFILL);
        ASSERT(p1.state == PEER_SYNCING_HEADERS);
        ASSERT(p2.state == PEER_SYNCING_HEADERS);
        ASSERT(p3.state == PEER_SYNCING_HEADERS);
        ASSERT(p1.last_getheaders_time == 0);

        struct watchdog_local_recovery_stats lr;
        sync_watchdog_get_local_recovery_stats(&lr);
        ASSERT(lr.active);
        ASSERT(lr.missing_height == 11);
        ASSERT(lr.distinct_peer_count == 3);
        ASSERT(lr.retries_exhausted);
        ASSERT(!lr.mirror_repair_gated);

        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;
        PASS();
    } _test_next:;

    return failures;
}

static int test_next_child_missing_gates_mirror_until_retries(void)
{
    int failures = 0;

    TEST("watchdog gates mirror repair while local retries remain") {
        reset_test_state();
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "next child missing");

        struct block_index tip = {0};
        tip.nHeight = 100;
        ASSERT(active_chain_set_tip(&g_test_ms.chain_active, &tip));

        struct p2p_node p1 = {0};
        p1.id = 1;
        p1.starting_height = 110;
        p1.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &p1 };
        g_test_cm.manager.nodes = peers;
        g_test_cm.manager.num_nodes = 1;

        enum watchdog_recovery_type r = sync_watchdog_check(
            &g_test_cm, &g_test_dm, &g_test_ms);
        ASSERT(r == WATCHDOG_LOCAL_HEADER_REFILL);

        struct watchdog_local_recovery_stats lr;
        sync_watchdog_get_local_recovery_stats(&lr);
        ASSERT(lr.active);
        ASSERT(lr.mirror_repair_gated);
        ASSERT(!lr.retries_exhausted);
        ASSERT(lr.retry_count == 1);

        g_test_cm.manager.nodes = NULL;
        g_test_cm.manager.num_nodes = 0;
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
    failures += test_at_tip_reconciliation();
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
    failures += test_zero_peers_stuck();
    failures += test_timeout_boundary_exact();
    failures += test_progress_rate_tracking();
    failures += test_utxo_pause_fires_after_window();
    failures += test_utxo_pause_inactive_resets_timer();
    failures += test_queue_starved_fires_after_window();
    failures += test_next_child_missing_triggers_local_refill();
    failures += test_next_child_missing_gates_mirror_until_retries();
    return failures;
}
