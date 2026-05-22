/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "mutator/mutator.h"

#include "application/consensus/validate_block.h"
#include "util/thread_registry.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct mutator {
    struct mutator_input_queue *queue;
    pthread_t tid;
    bool thread_started;
    atomic_ulong dispatched;
};

static struct zcl_result dispatch_validate_block(
        const struct mutator_cmd_validate_block_inputs *in)
{
    struct application_consensus_validate_block_inputs args = {
        .block = in->block, .params = in->params, .utxo = in->utxo,
    };
    return application_consensus_validate_block(&args);
}

static struct zcl_result dispatch_cmd(struct mutator_cmd *cmd)
{
    switch (cmd->kind) {
    case MUTATOR_CMD_VALIDATE_BLOCK:
        return dispatch_validate_block(&cmd->u.validate_block);
    }
    return ZCL_ERR(MUTATOR_ERR_UNKNOWN_KIND,
                   "mutator: unknown cmd kind %d", (int)cmd->kind);
}

static void *mutator_thread_main(void *arg)
{
    struct mutator *m = arg;
    for (;;) {
        struct mutator_cmd cmd;
        struct zcl_result r = mutator_input_queue_pop_blocking(m->queue, &cmd);
        if (!r.ok) {
            /* Only error path here is MUTATOR_ERR_QUEUE_CLOSED. */
            break;
        }
        struct zcl_result res = dispatch_cmd(&cmd);
        atomic_fetch_add_explicit(&m->dispatched, 1, memory_order_relaxed);
        if (cmd.completion)
            mutator_cmd_completion_post(cmd.completion, res);
    }
    thread_registry_unregister_self();
    return NULL;
}

struct zcl_result mutator_start(const struct mutator_config *cfg,
                                struct mutator **out)
{
    if (!out) return ZCL_ERR(MUTATOR_ERR_INTERNAL, "mutator_start: null out");
    size_t cap = (cfg && cfg->queue_capacity) ? cfg->queue_capacity : 64;
    const char *name = (cfg && cfg->thread_name) ? cfg->thread_name : "mutator";

    struct mutator *m = calloc(1, sizeof *m);
    if (!m)
        return ZCL_ERR(MUTATOR_ERR_INTERNAL, "mutator_start: calloc");
    atomic_init(&m->dispatched, 0);
    m->queue = mutator_input_queue_new(cap);
    if (!m->queue) {
        free(m);
        return ZCL_ERR(MUTATOR_ERR_INTERNAL,
                       "mutator_start: queue_new(cap=%zu) failed", cap);
    }

    int rc = thread_registry_spawn_ex(name, mutator_thread_main, m, &m->tid);
    if (rc != 0) {
        mutator_input_queue_free(m->queue);
        free(m);
        return ZCL_ERR(MUTATOR_ERR_INTERNAL,
                       "mutator_start: spawn rc=%d", rc);
    }
    m->thread_started = true;
    *out = m;
    return ZCL_OK;
}

void mutator_stop(struct mutator *m)
{
    if (!m) return;
    if (m->thread_started) {
        mutator_input_queue_close(m->queue);
        pthread_join(m->tid, NULL);
        m->thread_started = false;
    }
    mutator_input_queue_free(m->queue);
    m->queue = NULL;
    free(m);
}

struct mutator_input_queue *mutator_input_queue_of(struct mutator *m)
{
    return m ? m->queue : NULL;
}

unsigned long mutator_dispatched_count(struct mutator *m)
{
    if (!m) return 0;
    return atomic_load_explicit(&m->dispatched, memory_order_relaxed);
}
