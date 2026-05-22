/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * mutator/mutator.h — single-thread mutator at the center of the new
 * architecture.
 *
 * The mutator owns a bounded MPSC input queue and one thread. The
 * thread drains the queue, dispatches each command through a small
 * use-case table (currently just validate_block), and posts the
 * result via the command's completion handle.
 *
 * Lifecycle:
 *   mutator_start(cfg, &m)   — spawn the thread, return a handle.
 *   mutator_input_queue_of(m) — used by inbound adapters to push cmds.
 *   mutator_stop(m)          — close the queue, join the thread, free.
 */

#ifndef ZCL_MUTATOR_MUTATOR_H
#define ZCL_MUTATOR_MUTATOR_H

#include <stddef.h>

#include "mutator/cmd.h"
#include "mutator/input_queue.h"
#include "util/result.h"

struct mutator;

struct mutator_config {
    size_t queue_capacity;   /* 0 → default 64 */
    const char *thread_name; /* NULL → "mutator" */
};

struct zcl_result mutator_start(const struct mutator_config *cfg,
                                struct mutator **out);

/* Signals the mutator to stop, joins the thread, frees the handle.
 * Safe to call once. The associated input queue is freed too. */
void mutator_stop(struct mutator *m);

struct mutator_input_queue *mutator_input_queue_of(struct mutator *m);

/* Lifetime stats — number of commands dispatched (regardless of
 * result). Monotonically increasing. */
unsigned long mutator_dispatched_count(struct mutator *m);

#endif /* ZCL_MUTATOR_MUTATOR_H */
