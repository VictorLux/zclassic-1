/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "mutator/input_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct mutator_input_queue {
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    struct mutator_cmd *slots;
    size_t capacity;
    size_t head;             /* dequeue index */
    size_t tail;             /* enqueue index */
    atomic_size_t depth;     /* for lock-free depth() observer */
    bool closed;
};

struct mutator_input_queue *mutator_input_queue_new(size_t capacity)
{
    if (capacity == 0) return NULL;
    struct mutator_input_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->slots = calloc(capacity, sizeof *q->slots);
    if (!q->slots) { free(q); return NULL; }
    q->capacity = capacity;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    atomic_init(&q->depth, 0);
    return q;
}

void mutator_input_queue_free(struct mutator_input_queue *q)
{
    if (!q) return;
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
    free(q->slots);
    free(q);
}

struct zcl_result mutator_input_queue_push(struct mutator_input_queue *q,
                                           const struct mutator_cmd *cmd)
{
    if (!q || !cmd)
        return ZCL_ERR(MUTATOR_ERR_INTERNAL, "push: null arg(s)");

    pthread_mutex_lock(&q->mu);
    if (q->closed) {
        pthread_mutex_unlock(&q->mu);
        return ZCL_ERR(MUTATOR_ERR_QUEUE_CLOSED, "push: queue closed");
    }
    size_t d = atomic_load_explicit(&q->depth, memory_order_relaxed);
    if (d >= q->capacity) {
        pthread_mutex_unlock(&q->mu);
        return ZCL_ERR(MUTATOR_ERR_BACKPRESSURE,
                       "push: queue full (depth=%zu/cap=%zu)",
                       d, q->capacity);
    }
    q->slots[q->tail] = *cmd;
    q->tail = (q->tail + 1) % q->capacity;
    atomic_fetch_add_explicit(&q->depth, 1, memory_order_release);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return ZCL_OK;
}

struct zcl_result mutator_input_queue_pop_blocking(
        struct mutator_input_queue *q,
        struct mutator_cmd *out)
{
    if (!q || !out)
        return ZCL_ERR(MUTATOR_ERR_INTERNAL, "pop: null arg(s)");

    pthread_mutex_lock(&q->mu);
    while (atomic_load_explicit(&q->depth, memory_order_acquire) == 0
           && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (atomic_load_explicit(&q->depth, memory_order_acquire) == 0) {
        /* Closed and empty. */
        pthread_mutex_unlock(&q->mu);
        return ZCL_ERR(MUTATOR_ERR_QUEUE_CLOSED, "pop: queue closed");
    }
    *out = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    atomic_fetch_sub_explicit(&q->depth, 1, memory_order_release);
    pthread_mutex_unlock(&q->mu);
    return ZCL_OK;
}

void mutator_input_queue_close(struct mutator_input_queue *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

size_t mutator_input_queue_depth(struct mutator_input_queue *q)
{
    if (!q) return 0;
    return atomic_load_explicit(&q->depth, memory_order_acquire);
}

size_t mutator_input_queue_capacity(struct mutator_input_queue *q)
{
    return q ? q->capacity : 0;
}
