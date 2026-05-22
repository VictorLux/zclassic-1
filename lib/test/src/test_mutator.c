/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the mutator skeleton + bounded input queue.
 *
 * Three concerns are exercised separately:
 *   1. Input queue semantics — push/pop, FIFO order, backpressure on
 *      full, queue-closed sentinel. Tested without spawning a thread.
 *   2. End-to-end dispatch — submit a VALIDATE_BLOCK cmd with inputs
 *      that the existing validate_block use case will reject with a
 *      known typed code; verify the mutator returns the exact code
 *      via the completion handle.
 *   3. Lifecycle — start/stop is clean, dispatched_count tracks
 *      consumed commands, the thread joins on stop.
 */

#include "test/test_helpers.h"

#include "application/consensus/validate_block.h"
#include "mutator/cmd.h"
#include "mutator/input_queue.h"
#include "mutator/mutator.h"

#include <stdio.h>
#include <string.h>

#define MUT_CHECK(name, expr) do {                  \
    printf("mutator: %s... ", (name));              \
    if ((expr)) { printf("OK\n"); }                 \
    else { printf("FAIL\n"); failures++; }          \
} while (0)

static struct mutator_cmd null_validate_cmd(struct mutator_cmd_completion *c)
{
    struct mutator_cmd cmd = {0};
    cmd.kind = MUTATOR_CMD_VALIDATE_BLOCK;
    cmd.completion = c;
    /* All NULL fields → application_consensus_validate_block returns
     * APPLICATION_CONSENSUS_ERR_NULL_ARG (2001). */
    cmd.u.validate_block.block = NULL;
    cmd.u.validate_block.params = NULL;
    cmd.u.validate_block.utxo = NULL;
    return cmd;
}

int test_mutator(void)
{
    int failures = 0;

    /* ── 1. Queue: zero capacity is rejected. */
    {
        struct mutator_input_queue *q = mutator_input_queue_new(0);
        MUT_CHECK("queue_new(0) -> NULL", q == NULL);
    }

    /* ── 2. Queue: FIFO order. */
    {
        struct mutator_input_queue *q = mutator_input_queue_new(4);
        MUT_CHECK("queue_new(4) -> handle", q != NULL);
        MUT_CHECK("initial depth = 0", mutator_input_queue_depth(q) == 0);

        struct mutator_cmd a = null_validate_cmd(NULL);
        struct mutator_cmd b = null_validate_cmd(NULL);
        struct mutator_cmd c = null_validate_cmd(NULL);
        /* tag each via a stable union value: vout of utxo pointer
         * stand-in — instead, just rely on FIFO of struct value. We
         * mark the kind to a unique sentinel via the .block pointer
         * cast to a known offset. */
        a.u.validate_block.block = (const struct block *)(uintptr_t)0xa;
        b.u.validate_block.block = (const struct block *)(uintptr_t)0xb;
        c.u.validate_block.block = (const struct block *)(uintptr_t)0xc;
        mutator_input_queue_push(q, &a);
        mutator_input_queue_push(q, &b);
        mutator_input_queue_push(q, &c);
        MUT_CHECK("depth after 3 pushes = 3",
                  mutator_input_queue_depth(q) == 3);

        struct mutator_cmd out;
        mutator_input_queue_pop_blocking(q, &out);
        MUT_CHECK("pop yields a in FIFO",
                  out.u.validate_block.block == (const struct block *)(uintptr_t)0xa);
        mutator_input_queue_pop_blocking(q, &out);
        MUT_CHECK("pop yields b in FIFO",
                  out.u.validate_block.block == (const struct block *)(uintptr_t)0xb);
        mutator_input_queue_pop_blocking(q, &out);
        MUT_CHECK("pop yields c in FIFO",
                  out.u.validate_block.block == (const struct block *)(uintptr_t)0xc);
        MUT_CHECK("depth back to 0", mutator_input_queue_depth(q) == 0);
        mutator_input_queue_free(q);
    }

    /* ── 3. Queue: backpressure when full. */
    {
        struct mutator_input_queue *q = mutator_input_queue_new(2);
        struct mutator_cmd cmd = null_validate_cmd(NULL);
        MUT_CHECK("push 1/2 -> OK", mutator_input_queue_push(q, &cmd).ok);
        MUT_CHECK("push 2/2 -> OK", mutator_input_queue_push(q, &cmd).ok);
        struct zcl_result r = mutator_input_queue_push(q, &cmd);
        MUT_CHECK("push 3/2 -> BACKPRESSURE",
                  !r.ok && r.code == MUTATOR_ERR_BACKPRESSURE);
        mutator_input_queue_free(q);
    }

    /* ── 4. Queue: closed → push and pop return CLOSED. */
    {
        struct mutator_input_queue *q = mutator_input_queue_new(4);
        mutator_input_queue_close(q);
        struct mutator_cmd cmd = null_validate_cmd(NULL);
        struct zcl_result r = mutator_input_queue_push(q, &cmd);
        MUT_CHECK("push after close -> CLOSED",
                  !r.ok && r.code == MUTATOR_ERR_QUEUE_CLOSED);
        struct mutator_cmd out;
        r = mutator_input_queue_pop_blocking(q, &out);
        MUT_CHECK("pop after close on empty -> CLOSED",
                  !r.ok && r.code == MUTATOR_ERR_QUEUE_CLOSED);
        mutator_input_queue_free(q);
    }

    /* ── 5. Dispatch: VALIDATE_BLOCK with null inputs returns the
     * application_consensus error code via the completion. */
    {
        struct mutator *m = NULL;
        struct mutator_config cfg = { .queue_capacity = 8, .thread_name = "mutator-test" };
        struct zcl_result r = mutator_start(&cfg, &m);
        MUT_CHECK("mutator_start -> OK", r.ok && m != NULL);

        struct mutator_cmd_completion comp;
        mutator_cmd_completion_init(&comp);
        struct mutator_cmd cmd = null_validate_cmd(&comp);

        r = mutator_input_queue_push(mutator_input_queue_of(m), &cmd);
        MUT_CHECK("queue push -> OK", r.ok);

        struct zcl_result dispatched = mutator_cmd_completion_wait(&comp);
        MUT_CHECK("dispatch surfaces APPLICATION_CONSENSUS_ERR_NULL_ARG",
                  !dispatched.ok &&
                  dispatched.code == APPLICATION_CONSENSUS_ERR_NULL_ARG);
        MUT_CHECK("dispatched_count = 1",
                  mutator_dispatched_count(m) == 1);

        mutator_cmd_completion_destroy(&comp);
        mutator_stop(m);
    }

    /* ── 6. Lifecycle: start, push N, stop joins cleanly. */
    {
        struct mutator *m = NULL;
        struct mutator_config cfg = { .queue_capacity = 16, .thread_name = NULL };
        mutator_start(&cfg, &m);
        const unsigned long N = 5;
        struct mutator_cmd_completion comps[5];
        for (unsigned long i = 0; i < N; i++) {
            mutator_cmd_completion_init(&comps[i]);
            struct mutator_cmd c = null_validate_cmd(&comps[i]);
            mutator_input_queue_push(mutator_input_queue_of(m), &c);
        }
        for (unsigned long i = 0; i < N; i++)
            (void)mutator_cmd_completion_wait(&comps[i]);

        MUT_CHECK("dispatched_count = N after N submits",
                  mutator_dispatched_count(m) == N);

        for (unsigned long i = 0; i < N; i++)
            mutator_cmd_completion_destroy(&comps[i]);
        mutator_stop(m);
        MUT_CHECK("mutator_stop returns (thread joined)", true);
    }

    return failures;
}
