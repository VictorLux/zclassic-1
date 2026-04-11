/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for chain_activation_controller — state machine + planning tests. */

#include "test/test_helpers.h"
#include "services/chain_activation_controller.h"
#include <string.h>

/* ── Planning tests (pure function, no global state) ───────────── */

static int test_should_connect_ready(void) {
    int failures = 0;
    TEST("activation: READY + no shutdown + no anchor → DO_CONNECT") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_BOOT,
            .current_state = ACTIVATION_READY,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_DO_CONNECT);
        ASSERT(dec.should_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_should_connect_at_tip(void) {
    int failures = 0;
    TEST("activation: AT_TIP + new block → DO_CONNECT") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_NEW_BLOCK,
            .current_state = ACTIVATION_AT_TIP,
            .chain_tip_height = 3072280,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_DO_CONNECT);
        ASSERT(dec.should_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_skip_shutdown(void) {
    int failures = 0;
    TEST("activation: shutdown requested → SKIP_SHUTDOWN") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_BOOT,
            .current_state = ACTIVATION_READY,
            .shutdown_requested = true,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_SKIP_SHUTDOWN);
        ASSERT(dec.should_activate == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_skip_anchor_active(void) {
    int failures = 0;
    TEST("activation: ANCHOR_ACTIVE → SKIP_ANCHOR_BLOCKS") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_NEW_BLOCK,
            .current_state = ACTIVATION_ANCHOR_ACTIVE,
            .anchor_active = true,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_SKIP_ANCHOR_BLOCKS);
        ASSERT(dec.should_activate == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_skip_awaiting_utxos(void) {
    int failures = 0;
    TEST("activation: awaiting UTXOs → SKIP_AWAITING_UTXOS") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_BOOT,
            .current_state = ACTIVATION_READY,
            .awaiting_utxos = true,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_SKIP_AWAITING_UTXOS);
        ASSERT(dec.should_activate == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_skip_wrong_state(void) {
    int failures = 0;
    TEST("activation: BOOT_PENDING → SKIP_WRONG_STATE") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_BOOT,
            .current_state = ACTIVATION_BOOT_PENDING,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_SKIP_WRONG_STATE);
        ASSERT(dec.should_activate == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_skip_already_running(void) {
    int failures = 0;
    TEST("activation: CONNECTING → SKIP_ALREADY_RUNNING") {
        struct activation_request req = {
            .source = ACTIVATION_SRC_NEW_BLOCK,
            .current_state = ACTIVATION_CONNECTING,
        };
        struct activation_decision dec;
        activation_should_connect(&dec, &req);
        ASSERT(dec.result == ACTIVATION_SKIP_ALREADY_RUNNING);
        ASSERT(dec.should_activate == false);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Transition tests ──────────────────────────────────────────── */

static int test_transition_idle_to_boot(void) {
    int failures = 0;
    TEST("activation transition: IDLE → BOOT_PENDING valid") {
        ASSERT(activation_transition_valid(ACTIVATION_IDLE, ACTIVATION_BOOT_PENDING));
        PASS();
    } _test_next:;
    return failures;
}

static int test_transition_idle_to_ready_invalid(void) {
    int failures = 0;
    TEST("activation transition: IDLE → READY invalid") {
        ASSERT(!activation_transition_valid(ACTIVATION_IDLE, ACTIVATION_READY));
        PASS();
    } _test_next:;
    return failures;
}

static int test_transition_ready_to_connecting(void) {
    int failures = 0;
    TEST("activation transition: READY → CONNECTING valid") {
        ASSERT(activation_transition_valid(ACTIVATION_READY, ACTIVATION_CONNECTING));
        PASS();
    } _test_next:;
    return failures;
}

static int test_transition_connecting_to_at_tip(void) {
    int failures = 0;
    TEST("activation transition: CONNECTING → AT_TIP valid") {
        ASSERT(activation_transition_valid(ACTIVATION_CONNECTING, ACTIVATION_AT_TIP));
        PASS();
    } _test_next:;
    return failures;
}

static int test_transition_at_tip_to_connecting(void) {
    int failures = 0;
    TEST("activation transition: AT_TIP → CONNECTING valid") {
        ASSERT(activation_transition_valid(ACTIVATION_AT_TIP, ACTIVATION_CONNECTING));
        PASS();
    } _test_next:;
    return failures;
}

static int test_transition_anchor_to_connecting_invalid(void) {
    int failures = 0;
    TEST("activation transition: ANCHOR_ACTIVE → CONNECTING invalid") {
        ASSERT(!activation_transition_valid(ACTIVATION_ANCHOR_ACTIVE, ACTIVATION_CONNECTING));
        PASS();
    } _test_next:;
    return failures;
}

/* ── UTXO wipe tests ──────────────────────────────────────────── */

static int test_wipe_safe_idle(void) {
    int failures = 0;
    TEST("activation wipe: IDLE, no anchor → safe") {
        struct utxo_wipe_decision wd;
        activation_should_allow_utxo_wipe(&wd, ACTIVATION_IDLE, false);
        ASSERT(wd.safe_to_wipe == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wipe_blocked_anchor_active(void) {
    int failures = 0;
    TEST("activation wipe: ANCHOR_ACTIVE → NOT safe") {
        struct utxo_wipe_decision wd;
        activation_should_allow_utxo_wipe(&wd, ACTIVATION_ANCHOR_ACTIVE, true);
        ASSERT(wd.safe_to_wipe == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wipe_blocked_anchor_clearing(void) {
    int failures = 0;
    TEST("activation wipe: ANCHOR_CLEARING → NOT safe") {
        struct utxo_wipe_decision wd;
        activation_should_allow_utxo_wipe(&wd, ACTIVATION_ANCHOR_CLEARING, false);
        ASSERT(wd.safe_to_wipe == false);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wipe_safe_ready(void) {
    int failures = 0;
    TEST("activation wipe: READY, no anchor → safe") {
        struct utxo_wipe_decision wd;
        activation_should_allow_utxo_wipe(&wd, ACTIVATION_READY, false);
        ASSERT(wd.safe_to_wipe == true);
        PASS();
    } _test_next:;
    return failures;
}

/* ── State name tests ──────────────────────────────────────────── */

static int test_state_names(void) {
    int failures = 0;
    TEST("activation: all state names non-NULL") {
        for (int i = 0; i < ACTIVATION_NUM_STATES; i++) {
            ASSERT(activation_state_name((enum activation_state)i) != NULL);
            ASSERT(strlen(activation_state_name((enum activation_state)i)) > 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_state_name_unknown(void) {
    int failures = 0;
    TEST("activation: out-of-range state returns 'unknown'") {
        ASSERT(strcmp(activation_state_name((enum activation_state)99), "unknown") == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Registration ──────────────────────────────────────────────── */

int test_chain_activation_controller(void) {
    int failures = 0;
    /* Planning tests */
    failures += test_should_connect_ready();
    failures += test_should_connect_at_tip();
    failures += test_skip_shutdown();
    failures += test_skip_anchor_active();
    failures += test_skip_awaiting_utxos();
    failures += test_skip_wrong_state();
    failures += test_skip_already_running();
    /* Transition tests */
    failures += test_transition_idle_to_boot();
    failures += test_transition_idle_to_ready_invalid();
    failures += test_transition_ready_to_connecting();
    failures += test_transition_connecting_to_at_tip();
    failures += test_transition_at_tip_to_connecting();
    failures += test_transition_anchor_to_connecting_invalid();
    /* UTXO wipe tests */
    failures += test_wipe_safe_idle();
    failures += test_wipe_blocked_anchor_active();
    failures += test_wipe_blocked_anchor_clearing();
    failures += test_wipe_safe_ready();
    /* State name tests */
    failures += test_state_names();
    failures += test_state_name_unknown();
    return failures;
}
