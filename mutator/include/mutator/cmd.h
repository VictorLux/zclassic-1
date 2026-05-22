/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * mutator/cmd.h — typed command envelope posted by inbound adapters
 * onto the mutator's bounded input queue.
 *
 * The mutator is the single-thread mutator at the center of the new
 * architecture: every state change in the node funnels through this
 * queue. Inbound adapters (P2P, RPC, MCP, CLI) construct a
 * `struct mutator_cmd`, post it via `mutator_input_queue_push`, and
 * (optionally) wait on the attached `mutator_cmd_completion` for the
 * result.
 *
 * Memory model: the command carries pointers, not copies. The
 * producer must keep the referenced storage alive until the
 * completion fires (or, for fire-and-forget commands without a
 * completion, until the mutator has drained the queue past this
 * entry).
 */

#ifndef ZCL_MUTATOR_CMD_H
#define ZCL_MUTATOR_CMD_H

#include <pthread.h>
#include <stdbool.h>

#include "util/result.h"

struct block;
struct consensus_params;
struct utxo_snapshot_port;

enum mutator_cmd_kind {
    MUTATOR_CMD_VALIDATE_BLOCK = 1,
};

struct mutator_cmd_validate_block_inputs {
    const struct block *block;
    const struct consensus_params *params;
    const struct utxo_snapshot_port *utxo;   /* may be NULL */
};

/* Completion handle the producer waits on. Embed in producer-owned
 * stack/heap memory; pass a pointer in the cmd. NULL means
 * fire-and-forget. */
struct mutator_cmd_completion {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    struct zcl_result result;
};

struct mutator_cmd {
    enum mutator_cmd_kind kind;
    struct mutator_cmd_completion *completion;
    union {
        struct mutator_cmd_validate_block_inputs validate_block;
    } u;
};

void mutator_cmd_completion_init(struct mutator_cmd_completion *c);
void mutator_cmd_completion_destroy(struct mutator_cmd_completion *c);

/* Block the caller until the mutator fills `c->result` and signals.
 * Returns the result by value. */
struct zcl_result mutator_cmd_completion_wait(struct mutator_cmd_completion *c);

/* Called by the mutator after dispatching a cmd. Idempotent on the
 * completion handle. */
void mutator_cmd_completion_post(struct mutator_cmd_completion *c,
                                 struct zcl_result r);

#endif /* ZCL_MUTATOR_CMD_H */
