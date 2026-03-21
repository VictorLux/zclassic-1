/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the event log ring buffer and state machines. */

#include "test/test_helpers.h"
#include "event/event.h"
#include <string.h>
#include <stdio.h>

static int test_emit_dump_roundtrip(void)
{
    int failures = 0;

    TEST("event_emit + event_dump_json round-trip") {
        event_log_init();

        event_emitf(EV_NODE_STARTING, 0, "test v1.0");
        event_emitf(EV_TCP_CONNECTED, 42, "127.0.0.1:8033");
        event_emit(EV_PEER_VERSION, 42, "hello", 5);

        char buf[4096];
        size_t len = event_dump_json(buf, sizeof(buf), 10);
        ASSERT(len > 0);
        ASSERT(len < sizeof(buf));
        buf[len] = '\0';

        ASSERT(buf[0] == '[');
        ASSERT(buf[len - 1] == ']');
        ASSERT(strstr(buf, "sys.starting") != NULL);
        ASSERT(strstr(buf, "tcp.connected") != NULL);
        ASSERT(strstr(buf, "peer.version") != NULL);
        ASSERT(strstr(buf, "\"peer\":42") != NULL);
        ASSERT(strstr(buf, "test v1.0") != NULL);
        ASSERT(strstr(buf, "127.0.0.1:8033") != NULL);
        PASS();
    } _test_next:;

    return failures;
}

static int test_dump_count(void)
{
    int failures = 0;

    TEST("event_dump_json respects count") {
        event_log_init();

        for (int i = 0; i < 50; i++)
            event_emitf(EV_MSG_RECEIVED, (uint32_t)i, "msg%d", i);

        char buf[4096];
        size_t len = event_dump_json(buf, sizeof(buf), 5);
        ASSERT(len > 0);
        buf[len] = '\0';

        ASSERT(strstr(buf, "msg49") != NULL);
        ASSERT(strstr(buf, "msg45") != NULL);
        ASSERT(strstr(buf, "msg44") == NULL);
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_state_legal(void)
{
    int failures = 0;

    TEST("peer_set_state_checked legal transitions") {
        event_log_init();
        enum peer_state state = PEER_DISCONNECTED;

        ASSERT(peer_set_state_checked(1, &state, PEER_CONNECTING, "outbound"));
        ASSERT(state == PEER_CONNECTING);
        ASSERT(peer_set_state_checked(1, &state, PEER_CONNECTED, "tcp ok"));
        ASSERT(state == PEER_CONNECTED);
        ASSERT(peer_set_state_checked(1, &state, PEER_VERSION_SENT, "sent ver"));
        ASSERT(state == PEER_VERSION_SENT);
        ASSERT(peer_set_state_checked(1, &state, PEER_HANDSHAKE_COMPLETE, "verack"));
        ASSERT(state == PEER_HANDSHAKE_COMPLETE);
        ASSERT(peer_set_state_checked(1, &state, PEER_ACTIVE, "relay mode"));
        ASSERT(state == PEER_ACTIVE);
        ASSERT(peer_set_state_checked(1, &state, PEER_SYNCING_HEADERS, "IBD"));
        ASSERT(state == PEER_SYNCING_HEADERS);
        ASSERT(peer_set_state_checked(1, &state, PEER_SYNCING_BLOCKS, "blocks"));
        ASSERT(state == PEER_SYNCING_BLOCKS);
        ASSERT(peer_set_state_checked(1, &state, PEER_ACTIVE, "sync done"));
        ASSERT(state == PEER_ACTIVE);
        ASSERT(peer_set_state_checked(1, &state, PEER_SNAPSHOT_SERVING, "zsync"));
        ASSERT(state == PEER_SNAPSHOT_SERVING);
        ASSERT(peer_set_state_checked(1, &state, PEER_ACTIVE, "zsync done"));
        ASSERT(state == PEER_ACTIVE);
        ASSERT(peer_set_state_checked(1, &state, PEER_DISCONNECTING, "bye"));
        ASSERT(state == PEER_DISCONNECTING);
        ASSERT(peer_set_state_checked(1, &state, PEER_DISCONNECTED, "closed"));
        ASSERT(state == PEER_DISCONNECTED);
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_state_illegal(void)
{
    int failures = 0;

    TEST("peer_set_state_checked rejects illegal transitions") {
        event_log_init();
        enum peer_state state = PEER_DISCONNECTED;

        /* DISCONNECTED -> ACTIVE (skip handshake) */
        ASSERT(!peer_set_state_checked(1, &state, PEER_ACTIVE, "skip"));
        ASSERT(state == PEER_DISCONNECTED);

        /* DISCONNECTED -> SYNCING_HEADERS */
        ASSERT(!peer_set_state_checked(1, &state, PEER_SYNCING_HEADERS, "nope"));
        ASSERT(state == PEER_DISCONNECTED);

        /* DISCONNECTED -> BANNED */
        ASSERT(!peer_set_state_checked(1, &state, PEER_BANNED, "nope"));
        ASSERT(state == PEER_DISCONNECTED);

        /* Get to ACTIVE legally */
        peer_set_state_checked(1, &state, PEER_CONNECTING, "out");
        peer_set_state_checked(1, &state, PEER_CONNECTED, "tcp");
        peer_set_state_checked(1, &state, PEER_VERSION_SENT, "ver");
        peer_set_state_checked(1, &state, PEER_HANDSHAKE_COMPLETE, "ack");
        peer_set_state_checked(1, &state, PEER_ACTIVE, "go");

        /* ACTIVE -> CONNECTING (can't go back) */
        ASSERT(!peer_set_state_checked(1, &state, PEER_CONNECTING, "back"));
        ASSERT(state == PEER_ACTIVE);

        /* ACTIVE -> DISCONNECTED (must go through DISCONNECTING) */
        ASSERT(!peer_set_state_checked(1, &state, PEER_DISCONNECTED, "skip"));
        ASSERT(state == PEER_ACTIVE);
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_transition_valid(void)
{
    int failures = 0;

    TEST("peer_transition_valid") {
        ASSERT(peer_transition_valid(PEER_DISCONNECTED, PEER_CONNECTING));
        ASSERT(peer_transition_valid(PEER_DISCONNECTED, PEER_CONNECTED));
        ASSERT(peer_transition_valid(PEER_ACTIVE, PEER_BANNED));
        ASSERT(peer_transition_valid(PEER_BANNED, PEER_DISCONNECTED));
        ASSERT(peer_transition_valid(PEER_DISCONNECTING, PEER_DISCONNECTED));

        ASSERT(!peer_transition_valid(PEER_DISCONNECTED, PEER_ACTIVE));
        ASSERT(!peer_transition_valid(PEER_ACTIVE, PEER_CONNECTING));
        ASSERT(!peer_transition_valid(PEER_BANNED, PEER_ACTIVE));
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_state_name(void)
{
    int failures = 0;

    TEST("peer_state_name") {
        ASSERT_STR_EQ(peer_state_name(PEER_DISCONNECTED), "disconnected");
        ASSERT_STR_EQ(peer_state_name(PEER_ACTIVE), "active");
        ASSERT_STR_EQ(peer_state_name(PEER_SNAPSHOT_SERVING), "snapshot_serving");
        ASSERT_STR_EQ(peer_state_name(PEER_BANNED), "banned");
        PASS();
    } _test_next:;

    return failures;
}

static int test_sync_state_transitions(void)
{
    int failures = 0;

    TEST("sync_set_state legal transitions") {
        event_log_init();

        ASSERT(sync_set_state(SYNC_FINDING_PEERS, "test"));
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "test"));
        ASSERT(sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test"));
        ASSERT(sync_set_state(SYNC_CONNECTING_BLOCKS, "test"));
        ASSERT(sync_set_state(SYNC_AT_TIP, "test"));
        ASSERT(sync_set_state(SYNC_REORG, "test"));
        ASSERT(sync_set_state(SYNC_AT_TIP, "test"));
        ASSERT(sync_set_state(SYNC_IDLE, "test"));
        PASS();
    } _test_next:;

    return failures;
}

static int test_sync_state_illegal(void)
{
    int failures = 0;

    TEST("sync_set_state rejects illegal transitions") {
        ASSERT(sync_get_state() == SYNC_IDLE);

        ASSERT(!sync_set_state(SYNC_AT_TIP, "illegal"));
        ASSERT(sync_get_state() == SYNC_IDLE);

        ASSERT(!sync_set_state(SYNC_REORG, "illegal"));
        ASSERT(sync_get_state() == SYNC_IDLE);
        PASS();
    } _test_next:;

    return failures;
}

static int test_sync_state_name(void)
{
    int failures = 0;

    TEST("sync_state_name") {
        ASSERT_STR_EQ(sync_state_name(SYNC_IDLE), "idle");
        ASSERT_STR_EQ(sync_state_name(SYNC_AT_TIP), "at_tip");
        ASSERT_STR_EQ(sync_state_name(SYNC_SNAPSHOT_RECEIVE), "snapshot_receive");
        ASSERT_STR_EQ(sync_state_name(SYNC_FAILED), "failed");
        PASS();
    } _test_next:;

    return failures;
}

static int test_ring_buffer_wrapping(void)
{
    int failures = 0;

    TEST("ring buffer wrapping (>65536 events)") {
        event_log_init();

        for (int i = 0; i < 70000; i++)
            event_emitf(EV_MSG_SENT, 0, "e%d", i);

        char buf[65536];
        size_t len = event_dump_json(buf, sizeof(buf), 100);
        ASSERT(len > 0);
        buf[len] = '\0';

        ASSERT(strstr(buf, "e69999") != NULL);
        ASSERT(strstr(buf, "e69900") != NULL);
        ASSERT(strstr(buf, "\"e0\"") == NULL);
        ASSERT(strstr(buf, "\"e1\"") == NULL);

        char *big = malloc(64 * 1024 * 1024);
        ASSERT(big != NULL);
        len = event_dump_json(big, 64 * 1024 * 1024, 70000);
        ASSERT(len > 0);
        big[len] = '\0';

        ASSERT(strstr(big, "e69999") != NULL);
        ASSERT(strstr(big, "e69998") != NULL);
        ASSERT(strstr(big, "\"e0\"") == NULL);

        free(big);
        PASS();
    } _test_next:;

    return failures;
}

static int test_event_type_name(void)
{
    int failures = 0;

    TEST("event_type_name") {
        ASSERT_STR_EQ(event_type_name(EV_TCP_CONNECTED), "tcp.connected");
        ASSERT_STR_EQ(event_type_name(EV_BLOCK_CONNECTED), "val.block_connected");
        ASSERT_STR_EQ(event_type_name(EV_NODE_READY), "sys.ready");
        ASSERT_STR_EQ(event_type_name(EV_CRASH), "sys.crash");
        ASSERT_STR_EQ(event_type_name(EV_NUM_TYPES), "unknown");
        PASS();
    } _test_next:;

    return failures;
}

static int test_dump_small_buffer(void)
{
    int failures = 0;

    TEST("event_dump_json truncates gracefully") {
        event_log_init();
        event_emitf(EV_NODE_STARTING, 0, "test");

        char tiny[16];
        size_t len = event_dump_json(tiny, sizeof(tiny), 10);
        ASSERT(len > 0);
        ASSERT(len <= sizeof(tiny));
        ASSERT(tiny[0] == '[');
        PASS();
    } _test_next:;

    return failures;
}

static int test_dump_empty_log(void)
{
    int failures = 0;

    TEST("event_dump_json empty log") {
        event_log_init();

        char buf[256];
        size_t len = event_dump_json(buf, sizeof(buf), 100);
        ASSERT(len == 2);
        ASSERT(buf[0] == '[');
        ASSERT(buf[1] == ']');
        PASS();
    } _test_next:;

    return failures;
}

static int test_dump_filtered(void)
{
    int failures = 0;

    TEST("event_dump_json_filtered by type prefix") {
        event_log_init();

        event_emitf(EV_TCP_CONNECTED, 1, "peer1");
        event_emitf(EV_PEER_VERSION, 1, "v170011");
        event_emitf(EV_MSG_RECEIVED, 1, "block size=1000");
        event_emitf(EV_BLOCK_CONNECTED, 1, "h=100");
        event_emitf(EV_TX_ACCEPTED, 2, "txid");
        event_emitf(EV_PEER_MISBEHAVE, 1, "+10=10 bad");

        char buf[4096];

        /* Filter: peer. should match PEER_VERSION and PEER_MISBEHAVE */
        size_t len = event_dump_json_filtered(buf, sizeof(buf), 100, "peer.");
        ASSERT(len > 0);
        buf[len] = '\0';
        ASSERT(strstr(buf, "peer.version") != NULL);
        ASSERT(strstr(buf, "peer.misbehave") != NULL);
        ASSERT(strstr(buf, "tcp.connected") == NULL);
        ASSERT(strstr(buf, "val.block_connected") == NULL);

        /* Filter: val. should match BLOCK_CONNECTED only */
        len = event_dump_json_filtered(buf, sizeof(buf), 100, "val.");
        buf[len] = '\0';
        ASSERT(strstr(buf, "val.block_connected") != NULL);
        ASSERT(strstr(buf, "peer.") == NULL);

        /* Empty prefix = all events */
        len = event_dump_json_filtered(buf, sizeof(buf), 100, "");
        buf[len] = '\0';
        ASSERT(strstr(buf, "tcp.connected") != NULL);
        ASSERT(strstr(buf, "val.block_connected") != NULL);

        PASS();
    } _test_next:;

    return failures;
}

int test_event(void)
{
    int failures = 0;

    failures += test_emit_dump_roundtrip();
    failures += test_dump_count();
    failures += test_peer_state_legal();
    failures += test_peer_state_illegal();
    failures += test_peer_transition_valid();
    failures += test_peer_state_name();
    failures += test_sync_state_transitions();
    failures += test_sync_state_illegal();
    failures += test_sync_state_name();
    failures += test_ring_buffer_wrapping();
    failures += test_event_type_name();
    failures += test_dump_small_buffer();
    failures += test_dump_empty_log();
    failures += test_dump_filtered();

    return failures;
}
