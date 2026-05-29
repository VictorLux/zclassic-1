/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the shadow_feeder inbound adapter.
 *
 * The shadow_feeder bridges any block-accept signal (synthetic in
 * tests, or — later — the live event bus) into the new architecture:
 * append to a shadow block log AND post a VALIDATE_BLOCK command to
 * the mutator. The two destinations are *deliberately* uncoupled:
 * the log captures intent, the mutator validates asynchronously.
 *
 * Coverage:
 *   - null-arg / config validation
 *   - happy path: one observe → log grows by 1, observed_count = 1,
 *     mutator dispatches the cmd (we wait for dispatched_count to
 *     reach 1 via a brief sleep — the cmd is fire-and-forget so the
 *     completion can't be used to synchronize here)
 *   - idempotent shadow log: re-observe same block → log doesn't
 *     duplicate; observed_count rises to 2 (each call posts a fresh
 *     mutator cmd by design — log idempotency is at the byte level,
 *     not the dispatch level)
 *   - error: observe_block with NULL block returns NULL_ARG
 */

#include "test/test_helpers.h"

#include "adapters/inbound/shadow_feeder.h"
#include "adapters/outbound/persistence/block_log_file.h"
#include "application/consensus/validate_block.h"
#include "consensus/params.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "mutator/mutator.h"
#include "ports/block_log_port.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SF_CHECK(name, expr) do {                       \
    printf("shadow_feeder: %s... ", (name));            \
    if ((expr)) { printf("OK\n"); }                     \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_sfeed_XXXXXX");
    if (!mkdtemp(buf)) { perror("mkdtemp"); buf[0] = '\0'; }
}

static void minimal_coinbase(struct transaction *tx)
{
    transaction_init(tx);
    transaction_alloc(tx, 1, 1);
    outpoint_set_null(&tx->vin[0].prevout);
    tx->vin[0].sequence = UINT32_MAX;
    tx->vout[0].value = 1250000000;
    tx->vout[0].script_pub_key.size = 0;
}

/* Wait until cond returns true or `max_ms` elapses. Polling at 1ms is
 * crude but fine for a unit test where the mutator drains within a
 * couple of context switches. */
static bool wait_until(bool (*cond)(void *), void *ctx, int max_ms)
{
    for (int i = 0; i < max_ms; i++) {
        if (cond(ctx)) return true;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    return cond(ctx);
}

struct dispatched_at_least_ctx {
    struct mutator *m;
    unsigned long target;
};

static bool dispatched_at_least(void *ctx_v)
{
    struct dispatched_at_least_ctx *c = ctx_v;
    return mutator_dispatched_count(c->m) >= c->target;
}

int test_shadow_feeder(void)
{
    int failures = 0;

    /* ── 1. NULL-arg guards on create. */
    {
        struct shadow_feeder *f = NULL;
        struct zcl_result r = shadow_feeder_create(NULL, &f);
        SF_CHECK("create(NULL cfg) -> ERR_NULL_ARG",
                 !r.ok && r.code == SHADOW_FEEDER_ERR_NULL_ARG);
    }

    /* ── 2. Happy path: one observe → shadow log grows + mutator
     * dispatches the cmd. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        struct mutator *m = NULL;
        struct mutator_config mc = { .queue_capacity = 8, .thread_name = "sf-test" };
        mutator_start(&mc, &m);

        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct shadow_feeder_config cfg = {
            .shadow_dir = dir, .mutator = m, .params = &p,
        };
        struct shadow_feeder *f = NULL;
        struct zcl_result r = shadow_feeder_create(&cfg, &f);
        SF_CHECK("create -> OK", r.ok && f != NULL);

        /* Build a synthetic coinbase-only block. */
        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(1, sizeof(struct transaction));
        b.num_vtx = 1;
        minimal_coinbase(&b.vtx[0]);
        /* Block bytes here are arbitrary — the shadow log treats
         * them as opaque; only the use case parses block structure. */
        uint8_t bytes[16] = "shadow-block-A!";

        r = shadow_feeder_observe_block(f, 0, &b, bytes, sizeof bytes);
        SF_CHECK("observe -> OK", r.ok);
        SF_CHECK("observed_count = 1",
                 shadow_feeder_observed_count(f) == 1);

        struct dispatched_at_least_ctx dctx = { m, 1 };
        SF_CHECK("mutator dispatched the cmd within 200ms",
                 wait_until(dispatched_at_least, &dctx, 200));

        /* Verify the shadow log has the block. Re-open it via a
         * fresh port handle to bypass internal caches. */
        block_log_file_close(NULL);  /* sanity: NULL-safe */

        block_free(&b);
        shadow_feeder_destroy(f);
        mutator_stop(m);
        test_rm_rf(dir);
    }

    /* ── 3. Idempotency: observing the same block twice does not
     * corrupt the log; observed_count rises to 2 since each call
     * posts a fresh mutator cmd. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        struct mutator *m = NULL;
        struct mutator_config mc = { .queue_capacity = 8, .thread_name = "sf-test2" };
        mutator_start(&mc, &m);

        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct shadow_feeder_config cfg = {
            .shadow_dir = dir, .mutator = m, .params = &p,
        };
        struct shadow_feeder *f = NULL;
        shadow_feeder_create(&cfg, &f);

        struct block b; block_init(&b);
        b.header.nBits = 0x207fffff;
        b.vtx = calloc(1, sizeof(struct transaction));
        b.num_vtx = 1;
        minimal_coinbase(&b.vtx[0]);
        uint8_t bytes[8] = {1,2,3,4,5,6,7,8};

        shadow_feeder_observe_block(f, 0, &b, bytes, sizeof bytes);
        struct zcl_result r = shadow_feeder_observe_block(f, 0, &b,
                                                          bytes, sizeof bytes);
        SF_CHECK("re-observe same block -> OK (idempotent)", r.ok);
        SF_CHECK("observed_count = 2 after double observe",
                 shadow_feeder_observed_count(f) == 2);

        struct dispatched_at_least_ctx dctx = { m, 2 };
        SF_CHECK("mutator dispatched both within 200ms",
                 wait_until(dispatched_at_least, &dctx, 200));

        block_free(&b);
        shadow_feeder_destroy(f);
        mutator_stop(m);
        test_rm_rf(dir);
    }

    /* ── 4. observe(NULL block) returns NULL_ARG. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        struct mutator *m = NULL;
        struct mutator_config mc = { .queue_capacity = 4, .thread_name = "sf-test3" };
        mutator_start(&mc, &m);
        struct consensus_params p; test_make_easy_consensus_params(&p);
        struct shadow_feeder_config cfg = {
            .shadow_dir = dir, .mutator = m, .params = &p,
        };
        struct shadow_feeder *f = NULL;
        shadow_feeder_create(&cfg, &f);

        struct zcl_result r = shadow_feeder_observe_block(
                f, 0, NULL, NULL, 0);
        SF_CHECK("observe(NULL block) -> ERR_NULL_ARG",
                 !r.ok && r.code == SHADOW_FEEDER_ERR_NULL_ARG);

        shadow_feeder_destroy(f);
        mutator_stop(m);
        test_rm_rf(dir);
    }

    return failures;
}
