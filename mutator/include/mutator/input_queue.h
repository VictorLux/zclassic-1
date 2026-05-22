/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * mutator/input_queue.h — bounded MPSC queue for typed commands.
 *
 * Single consumer (the mutator thread). Multiple producers (inbound
 * adapters). Backed by a fixed-capacity ring buffer. Producers that
 * arrive when the queue is full receive ZCL_E_BACKPRESSURE rather
 * than blocking — backpressure must propagate upward to the inbound
 * adapter so the upstream peer / RPC client sees the load signal.
 *
 * The queue can be `close()`d to wake any blocked consumer at
 * shutdown; subsequent `push`/`pop_blocking` return
 * MUTATOR_ERR_QUEUE_CLOSED.
 */

#ifndef ZCL_MUTATOR_INPUT_QUEUE_H
#define ZCL_MUTATOR_INPUT_QUEUE_H

#include <stddef.h>

#include "mutator/cmd.h"
#include "util/result.h"

struct mutator_input_queue;

enum mutator_input_queue_err {
    MUTATOR_ERR_BACKPRESSURE  = 3001,
    MUTATOR_ERR_QUEUE_CLOSED  = 3002,
    MUTATOR_ERR_UNKNOWN_KIND  = 3003,
    MUTATOR_ERR_INTERNAL      = 3004,
};

/* Construct a queue with the given fixed capacity. Returns NULL on
 * allocation failure or capacity == 0. */
struct mutator_input_queue *mutator_input_queue_new(size_t capacity);
void mutator_input_queue_free(struct mutator_input_queue *q);

/* Non-blocking enqueue. Returns ZCL_OK, MUTATOR_ERR_BACKPRESSURE if
 * the queue is full, or MUTATOR_ERR_QUEUE_CLOSED if the queue has
 * been closed. The cmd is copied by value. */
struct zcl_result mutator_input_queue_push(struct mutator_input_queue *q,
                                           const struct mutator_cmd *cmd);

/* Blocking dequeue. Returns ZCL_OK or MUTATOR_ERR_QUEUE_CLOSED. */
struct zcl_result mutator_input_queue_pop_blocking(
        struct mutator_input_queue *q,
        struct mutator_cmd *out);

/* Mark the queue closed and wake any waiting consumer. Idempotent. */
void mutator_input_queue_close(struct mutator_input_queue *q);

/* Current number of pending entries. Lock-free observer; result is a
 * point-in-time snapshot. */
size_t mutator_input_queue_depth(struct mutator_input_queue *q);

size_t mutator_input_queue_capacity(struct mutator_input_queue *q);

#endif /* ZCL_MUTATOR_INPUT_QUEUE_H */
