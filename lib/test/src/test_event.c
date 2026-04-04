/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the event log ring buffer and state machines. */

#include "test/test_helpers.h"
#include "event/event.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

static _Atomic int g_async_observer_calls = 0;

static void test_async_observer(enum event_type type, uint32_t peer_id,
                                const void *payload, uint32_t payload_len,
                                void *ctx)
{
    (void)type;
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    (void)ctx;
    atomic_fetch_add(&g_async_observer_calls, 1);
}

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

static int test_sync_full_lifecycle(void)
{
    int failures = 0;

    TEST("sync state machine full IBD lifecycle with events") {
        event_log_init();

        /* Full IBD path: idle -> finding -> headers -> blocks -> connect -> tip */
        ASSERT(sync_get_state() == SYNC_IDLE);
        ASSERT(sync_set_state(SYNC_FINDING_PEERS, "bootstrap"));
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "got peers"));
        ASSERT(sync_set_state(SYNC_BLOCKS_DOWNLOAD, "headers done"));
        ASSERT(sync_set_state(SYNC_CONNECTING_BLOCKS, "blocks arrived"));
        ASSERT(sync_set_state(SYNC_AT_TIP, "chain synced"));
        ASSERT(sync_get_state() == SYNC_AT_TIP);

        /* Reorg recovery: tip -> reorg -> tip */
        ASSERT(sync_set_state(SYNC_REORG, "fork detected"));
        ASSERT(sync_get_state() == SYNC_REORG);
        ASSERT(sync_set_state(SYNC_AT_TIP, "reorg resolved"));

        /* Return to idle */
        ASSERT(sync_set_state(SYNC_IDLE, "shutdown"));

        /* Verify events were emitted for transitions */
        char buf[16384];
        size_t len = event_dump_json_filtered(buf, sizeof(buf), 100, "sync.");
        ASSERT(len > 0);
        buf[len] = '\0';
        ASSERT(strstr(buf, "sync.state_change") != NULL);

        PASS();
    } _test_next:;

    return failures;
}

static int test_sync_snapshot_path(void)
{
    int failures = 0;

    TEST("sync state machine snapshot receive path") {
        event_log_init();

        /* Snapshot path: idle -> finding -> snapshot -> connecting -> tip */
        ASSERT(sync_set_state(SYNC_FINDING_PEERS, "bootstrap"));
        ASSERT(sync_set_state(SYNC_SNAPSHOT_RECEIVE, "fast sync"));
        ASSERT(sync_get_state() == SYNC_SNAPSHOT_RECEIVE);
        ASSERT(sync_set_state(SYNC_CONNECTING_BLOCKS, "apply snapshot"));
        ASSERT(sync_set_state(SYNC_AT_TIP, "synced via snapshot"));
        ASSERT(sync_get_state() == SYNC_AT_TIP);

        /* Reset for next test */
        ASSERT(sync_set_state(SYNC_IDLE, "done"));
        PASS();
    } _test_next:;

    return failures;
}

static int test_sync_snapshot_from_headers(void)
{
    int failures = 0;

    TEST("sync state machine allows snapshot receive during header sync") {
        event_log_init();

        ASSERT(sync_set_state(SYNC_FINDING_PEERS, "bootstrap"));
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "headers"));
        ASSERT(sync_set_state(SYNC_SNAPSHOT_RECEIVE, "verified snapshot"));
        ASSERT(sync_get_state() == SYNC_SNAPSHOT_RECEIVE);
        ASSERT(sync_set_state(SYNC_HEADERS_DOWNLOAD, "resume after snapshot"));
        ASSERT(sync_set_state(SYNC_IDLE, "done"));
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_full_lifecycle(void)
{
    int failures = 0;

    TEST("peer state machine full lifecycle with ban") {
        event_log_init();
        enum peer_state s = PEER_DISCONNECTED;

        /* Normal connect -> handshake -> active -> snapshot -> ban */
        ASSERT(peer_set_state_checked(1, &s, PEER_CONNECTING, "outbound"));
        ASSERT(peer_set_state_checked(1, &s, PEER_CONNECTED, "tcp"));
        ASSERT(peer_set_state_checked(1, &s, PEER_VERSION_SENT, "ver"));
        ASSERT(peer_set_state_checked(1, &s, PEER_VERSION_RECEIVED, "verack"));
        ASSERT(s == PEER_VERSION_RECEIVED);
        ASSERT(peer_set_state_checked(1, &s, PEER_HANDSHAKE_COMPLETE, "hs"));
        ASSERT(peer_set_state_checked(1, &s, PEER_ACTIVE, "ready"));

        /* Snapshot serving cycle */
        ASSERT(peer_set_state_checked(1, &s, PEER_SNAPSHOT_SERVING, "zsync"));
        ASSERT(s == PEER_SNAPSHOT_SERVING);
        ASSERT(peer_set_state_checked(1, &s, PEER_ACTIVE, "done"));

        /* Misbehavior -> ban */
        ASSERT(peer_set_state_checked(1, &s, PEER_BANNED, "bad peer"));
        ASSERT(s == PEER_BANNED);
        ASSERT(peer_set_state_checked(1, &s, PEER_DISCONNECTED, "cleanup"));
        ASSERT(s == PEER_DISCONNECTED);

        /* Verify events captured the lifecycle */
        char buf[8192];
        size_t len = event_dump_json_filtered(buf, sizeof(buf), 50, "peer.state");
        ASSERT(len > 0);
        buf[len] = '\0';
        ASSERT(strstr(buf, "peer.state_change") != NULL);
        PASS();
    } _test_next:;

    return failures;
}

static int test_peer_stale_recovery(void)
{
    int failures = 0;

    TEST("peer state machine stale -> active recovery") {
        event_log_init();
        enum peer_state s = PEER_DISCONNECTED;

        /* Get to active */
        peer_set_state_checked(1, &s, PEER_CONNECTING, "out");
        peer_set_state_checked(1, &s, PEER_CONNECTED, "tcp");
        peer_set_state_checked(1, &s, PEER_VERSION_SENT, "ver");
        peer_set_state_checked(1, &s, PEER_HANDSHAKE_COMPLETE, "hs");
        peer_set_state_checked(1, &s, PEER_ACTIVE, "go");

        /* Go stale and recover */
        ASSERT(peer_set_state_checked(1, &s, PEER_STALE, "no response"));
        ASSERT(s == PEER_STALE);
        ASSERT(peer_set_state_checked(1, &s, PEER_ACTIVE, "responded"));
        ASSERT(s == PEER_ACTIVE);

        /* Clean disconnect from stale */
        peer_set_state_checked(1, &s, PEER_STALE, "stale again");
        ASSERT(peer_set_state_checked(1, &s, PEER_DISCONNECTING, "give up"));
        ASSERT(peer_set_state_checked(1, &s, PEER_DISCONNECTED, "closed"));
        PASS();
    } _test_next:;

    return failures;
}

static void *event_emit_worker(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 5000; i++)
        event_emitf(EV_MSG_RECEIVED, (uint32_t)id, "t%d_e%d", id, i);
    return NULL;
}

static int test_concurrent_emit(void)
{
    int failures = 0;

    TEST("concurrent event emission (4 threads x 5000 events)") {
        event_log_init();

        #define EV_NUM_THREADS 4
        pthread_t threads[EV_NUM_THREADS];
        int ids[EV_NUM_THREADS];

        for (int i = 0; i < EV_NUM_THREADS; i++) {
            ids[i] = i + 1;
            pthread_create(&threads[i], NULL, event_emit_worker, &ids[i]);
        }
        for (int i = 0; i < EV_NUM_THREADS; i++)
            pthread_join(threads[i], NULL);

        /* 20,000 events emitted total — ring buffer holds 65536 so all fit */
        char buf[65536];
        size_t len = event_dump_json(buf, sizeof(buf), 100);
        ASSERT(len > 0);
        buf[len] = '\0';

        /* Verify events from all threads are present in the last 100 */
        ASSERT(strstr(buf, "msg.received") != NULL);

        /* Dump large to verify no corruption */
        char *big = malloc(16 * 1024 * 1024);
        ASSERT(big != NULL);
        len = event_dump_json(big, 16 * 1024 * 1024, 20000);
        ASSERT(len > 0);
        big[len] = '\0';

        /* Must be valid JSON array */
        ASSERT(big[0] == '[');
        ASSERT(big[len - 1] == ']');

        /* Verify events from all 4 threads present */
        ASSERT(strstr(big, "t1_e") != NULL);
        ASSERT(strstr(big, "t2_e") != NULL);
        ASSERT(strstr(big, "t3_e") != NULL);
        ASSERT(strstr(big, "t4_e") != NULL);

        free(big);
        PASS();
    } _test_next:;

    return failures;
}

static int test_async_dispatch_lifecycle(void)
{
    int failures = 0;

    TEST("event async dispatcher starts, drains, and stops idempotently") {
        event_log_init();
        event_clear_all_observers();
        atomic_store(&g_async_observer_calls, 0);

        ASSERT(event_observe_async(EV_NODE_READY, test_async_observer, NULL));
        ASSERT(event_async_start());
        ASSERT(event_async_start());

        event_emitf(EV_NODE_READY, 7, "ready");

        for (int i = 0; i < 50; i++) {
            struct timespec pause = {0, 1000000};
            if (atomic_load(&g_async_observer_calls) > 0)
                break;
            nanosleep(&pause, NULL);
        }

        ASSERT(atomic_load(&g_async_observer_calls) > 0);
        event_async_stop();
        event_async_stop();
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
    failures += test_sync_full_lifecycle();
    failures += test_sync_snapshot_path();
    failures += test_sync_snapshot_from_headers();
    failures += test_peer_full_lifecycle();
    failures += test_peer_stale_recovery();
    failures += test_concurrent_emit();
    failures += test_async_dispatch_lifecycle();

    return failures;
}
