/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the block download manager. */

#include "test/test_helpers.h"
#include "net/download.h"
#include "core/uint256.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* Helper: make a uint256 from a single byte value */
static struct uint256 make_hash(uint8_t v)
{
    struct uint256 h;
    memset(h.data, 0, 32);
    h.data[0] = v;
    h.data[31] = v; /* non-zero in both positions for probe chain testing */
    return h;
}

static int test_dl_init_free(void)
{
    int failures = 0;
    TEST("dl_init and dl_free") {
        struct download_manager dm;
        dl_init(&dm);
        ASSERT(dm.num_slots > 0);
        ASSERT(dm.num_active == 0);
        ASSERT(dm.queue_len == 0);
        dl_free(&dm);
        ASSERT(dm.slots == NULL);
        ASSERT(dm.queue == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_mark_requested(void)
{
    int failures = 0;
    TEST("dl_mark_requested basic") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);

        ASSERT(dl_mark_requested(&dm, &h1, 100, 1));
        ASSERT(dl_is_in_flight(&dm, &h1));
        ASSERT(!dl_is_in_flight(&dm, &h2));

        ASSERT(dl_mark_requested(&dm, &h2, 101, 2));
        ASSERT(dl_is_in_flight(&dm, &h2));

        /* Duplicate request rejected */
        ASSERT(!dl_mark_requested(&dm, &h1, 100, 3));

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(req == 2);
        ASSERT(inflight == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_mark_received(void)
{
    int failures = 0;
    TEST("dl_mark_received removes from in-flight") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);
        struct uint256 h3 = make_hash(3);

        dl_mark_requested(&dm, &h1, 100, 1);
        dl_mark_requested(&dm, &h2, 101, 1);
        dl_mark_requested(&dm, &h3, 102, 2);

        /* Receive h2 */
        uint32_t peer = dl_mark_received(&dm, &h2);
        ASSERT(peer == 1);
        ASSERT(!dl_is_in_flight(&dm, &h2));
        ASSERT(dl_is_in_flight(&dm, &h1));
        ASSERT(dl_is_in_flight(&dm, &h3));

        /* Receive unknown hash */
        struct uint256 h4 = make_hash(4);
        ASSERT(dl_mark_received(&dm, &h4) == 0);

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv == 1);
        ASSERT(inflight == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_queue_dedup(void)
{
    int failures = 0;
    TEST("dl_queue_blocks deduplicates") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 hashes[5];
        int32_t heights[5];
        for (int i = 0; i < 5; i++) {
            hashes[i] = make_hash((uint8_t)(10 + i));
            heights[i] = 200 + i;
        }

        /* Queue 5 blocks */
        size_t added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 5);

        /* Queue same 5 again — should add 0 */
        added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 0);

        /* Mark one as in-flight, then try to queue it */
        dl_mark_requested(&dm, &hashes[0], 200, 1);
        added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 0); /* all either queued or in-flight */

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 4); /* 5 queued minus 1 moved to in-flight */
        ASSERT(inflight == 1);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_assign_to_peer(void)
{
    int failures = 0;
    TEST("dl_assign_to_peer pulls from queue") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 hashes[10];
        int32_t heights[10];
        for (int i = 0; i < 10; i++) {
            hashes[i] = make_hash((uint8_t)(20 + i));
            heights[i] = 300 + i;
        }

        dl_queue_blocks(&dm, hashes, heights, 10);

        struct uint256 out[5];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 5);
        ASSERT(assigned == 5);

        /* First 5 should be in-flight for peer 1 */
        ASSERT(dl_peer_in_flight(&dm, 1) == 5);

        /* Queue should have 5 remaining */
        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 5);
        ASSERT(inflight == 5);

        /* Assign 5 more to peer 2 */
        assigned = dl_assign_to_peer(&dm, 2, out, 5);
        ASSERT(assigned == 5);
        ASSERT(dl_peer_in_flight(&dm, 2) == 5);

        /* Queue empty now */
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 0);
        ASSERT(inflight == 10);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_peer_disconnected(void)
{
    int failures = 0;
    TEST("dl_peer_disconnected re-queues blocks") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);
        struct uint256 h3 = make_hash(3);

        dl_mark_requested(&dm, &h1, 100, 1);
        dl_mark_requested(&dm, &h2, 101, 1);
        dl_mark_requested(&dm, &h3, 102, 2);

        /* Disconnect peer 1 — h1 and h2 should be re-queued */
        size_t requeued = dl_peer_disconnected(&dm, 1);
        ASSERT(requeued == 2);

        ASSERT(!dl_is_in_flight(&dm, &h1));
        ASSERT(!dl_is_in_flight(&dm, &h2));
        ASSERT(dl_is_in_flight(&dm, &h3)); /* peer 2 still active */

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 2);
        ASSERT(inflight == 1);

        /* Assign re-queued blocks to peer 3 */
        struct uint256 out[5];
        size_t assigned = dl_assign_to_peer(&dm, 3, out, 5);
        ASSERT(assigned == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_check_timeouts(void)
{
    int failures = 0;
    TEST("dl_check_timeouts re-queues stale blocks") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        dl_mark_requested(&dm, &h1, 100, 1);

        /* No timeout at current time */
        int64_t now = (int64_t)time(NULL);
        ASSERT(dl_check_timeouts(&dm, now) == 0);
        ASSERT(dl_is_in_flight(&dm, &h1));

        /* Timeout after DL_REQUEST_TIMEOUT_SECS */
        ASSERT(dl_check_timeouts(&dm, now + DL_REQUEST_TIMEOUT_SECS + 1) == 1);
        ASSERT(!dl_is_in_flight(&dm, &h1));

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(tout == 1);
        ASSERT(queued == 1); /* re-queued */
        ASSERT(inflight == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_many_insertions(void)
{
    int failures = 0;
    TEST("dl hash table handles many insertions and deletions") {
        struct download_manager dm;
        dl_init(&dm);

        /* Insert 500 blocks */
        for (int i = 0; i < 500; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            dl_mark_requested(&dm, &h, i, (uint32_t)(i % 10));
        }

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(inflight == 500);

        /* Receive half */
        for (int i = 0; i < 250; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            dl_mark_received(&dm, &h);
        }

        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv == 250);
        ASSERT(inflight == 250);

        /* Remaining 250 should still be findable */
        for (int i = 250; i < 500; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            ASSERT(dl_is_in_flight(&dm, &h));
        }

        /* Received ones should NOT be findable */
        for (int i = 0; i < 250; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            ASSERT(!dl_is_in_flight(&dm, &h));
        }

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_per_peer_limit(void)
{
    int failures = 0;
    TEST("dl_assign_to_peer respects per-peer limit") {
        struct download_manager dm;
        dl_init(&dm);

        /* Queue 200 blocks */
        struct uint256 hashes[200];
        int32_t heights[200];
        for (int i = 0; i < 200; i++) {
            hashes[i] = make_hash((uint8_t)(i & 0xFF));
            hashes[i].data[1] = (uint8_t)(i >> 8);
            heights[i] = i;
        }
        dl_queue_blocks(&dm, hashes, heights, 200);

        /* Assign all to peer 1 — should cap at DL_MAX_IN_FLIGHT_PER_PEER */
        struct uint256 out[200];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 200);
        ASSERT(assigned == DL_MAX_IN_FLIGHT_PER_PEER);
        ASSERT(dl_peer_in_flight(&dm, 1) == DL_MAX_IN_FLIGHT_PER_PEER);

        /* Try to assign more to same peer — should get 0 */
        assigned = dl_assign_to_peer(&dm, 1, out, 200);
        ASSERT(assigned == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* Concurrency test: multiple threads hitting the download manager */
struct dl_thread_ctx {
    struct download_manager *dm;
    int thread_id;
    int ops_done;
};

static void *dl_concurrent_worker(void *arg)
{
    struct dl_thread_ctx *ctx = (struct dl_thread_ctx *)arg;
    struct download_manager *dm = ctx->dm;
    int tid = ctx->thread_id;
    int ops = 0;

    /* Each thread works on a unique range of hashes */
    for (int round = 0; round < 10; round++) {
        struct uint256 hashes[20];
        int32_t heights[20];
        for (int i = 0; i < 20; i++) {
            memset(hashes[i].data, 0, 32);
            hashes[i].data[0] = (uint8_t)(tid);
            hashes[i].data[1] = (uint8_t)(round);
            hashes[i].data[2] = (uint8_t)(i);
            heights[i] = tid * 10000 + round * 100 + i;
        }

        /* Queue blocks */
        dl_queue_blocks(dm, hashes, heights, 20);
        ops++;

        /* Assign to self */
        struct uint256 out[20];
        size_t assigned = dl_assign_to_peer(dm, (uint32_t)tid, out, 20);
        ops++;

        /* "Receive" them */
        for (size_t i = 0; i < assigned; i++) {
            dl_mark_received(dm, &out[i]);
            ops++;
        }
    }

    ctx->ops_done = ops;
    return NULL;
}

static int test_dl_concurrent(void)
{
    int failures = 0;
    TEST("download manager concurrent access (4 threads)") {
        struct download_manager dm;
        dl_init(&dm);

        #define DL_NUM_THREADS 4
        pthread_t threads[DL_NUM_THREADS];
        struct dl_thread_ctx ctxs[DL_NUM_THREADS];

        for (int i = 0; i < DL_NUM_THREADS; i++) {
            ctxs[i].dm = &dm;
            ctxs[i].thread_id = i + 1;
            ctxs[i].ops_done = 0;
            pthread_create(&threads[i], NULL, dl_concurrent_worker, &ctxs[i]);
        }

        for (int i = 0; i < DL_NUM_THREADS; i++)
            pthread_join(threads[i], NULL);

        /* Verify all threads completed */
        int total_ops = 0;
        for (int i = 0; i < DL_NUM_THREADS; i++) {
            ASSERT(ctxs[i].ops_done > 0);
            total_ops += ctxs[i].ops_done;
        }

        /* Verify stats are consistent: received <= requested, no negative */
        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv <= req);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

int test_download(void)
{
    int failures = 0;
    failures += test_dl_init_free();
    failures += test_dl_mark_requested();
    failures += test_dl_mark_received();
    failures += test_dl_queue_dedup();
    failures += test_dl_assign_to_peer();
    failures += test_dl_peer_disconnected();
    failures += test_dl_check_timeouts();
    failures += test_dl_many_insertions();
    failures += test_dl_per_peer_limit();
    failures += test_dl_concurrent();
    return failures;
}
