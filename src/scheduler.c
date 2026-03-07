#define _POSIX_C_SOURCE 200809L
/* Copyright (c) 2015 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "scheduler.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int timespec_cmp(struct timespec a, struct timespec b)
{
    if (a.tv_sec < b.tv_sec) return -1;
    if (a.tv_sec > b.tv_sec) return 1;
    if (a.tv_nsec < b.tv_nsec) return -1;
    if (a.tv_nsec > b.tv_nsec) return 1;
    return 0;
}

void scheduler_init(struct scheduler *s)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
}

void scheduler_destroy(struct scheduler *s)
{
    assert(s->threads_servicing == 0);
    struct scheduler_task *t = s->queue;
    while (t) {
        struct scheduler_task *next = t->next;
        free(t);
        t = next;
    }
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
}

static bool should_stop(struct scheduler *s)
{
    return s->stop_requested || (s->stop_when_empty && s->queue == NULL);
}

void scheduler_service_queue(struct scheduler *s)
{
    pthread_mutex_lock(&s->mutex);
    s->threads_servicing++;

    while (!should_stop(s)) {
        while (!should_stop(s) && s->queue == NULL)
            pthread_cond_wait(&s->cond, &s->mutex);

        if (should_stop(s) || s->queue == NULL)
            continue;

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        if (timespec_cmp(s->queue->when, now) > 0) {
            pthread_cond_timedwait(&s->cond, &s->mutex, &s->queue->when);
            continue;
        }

        struct scheduler_task *task = s->queue;
        s->queue = task->next;

        pthread_mutex_unlock(&s->mutex);
        task->func(task->ctx);
        free(task);
        pthread_mutex_lock(&s->mutex);
    }

    s->threads_servicing--;
    pthread_mutex_unlock(&s->mutex);
}

void scheduler_stop(struct scheduler *s, bool drain)
{
    pthread_mutex_lock(&s->mutex);
    if (drain)
        s->stop_when_empty = true;
    else
        s->stop_requested = true;
    pthread_mutex_unlock(&s->mutex);
    pthread_cond_broadcast(&s->cond);
}

void scheduler_schedule(struct scheduler *s, scheduler_func f, void *ctx, struct timespec when)
{
    struct scheduler_task *task = malloc(sizeof(*task));
    task->func = f;
    task->ctx = ctx;
    task->when = when;
    task->next = NULL;

    pthread_mutex_lock(&s->mutex);
    struct scheduler_task **pp = &s->queue;
    while (*pp && timespec_cmp((*pp)->when, when) <= 0)
        pp = &(*pp)->next;
    task->next = *pp;
    *pp = task;
    pthread_mutex_unlock(&s->mutex);
    pthread_cond_signal(&s->cond);
}

void scheduler_schedule_from_now(struct scheduler *s, scheduler_func f, void *ctx, int64_t delta_seconds)
{
    struct timespec when;
    clock_gettime(CLOCK_REALTIME, &when);
    when.tv_sec += delta_seconds;
    scheduler_schedule(s, f, ctx, when);
}

size_t scheduler_queue_size(struct scheduler *s)
{
    pthread_mutex_lock(&s->mutex);
    size_t count = 0;
    for (struct scheduler_task *t = s->queue; t; t = t->next)
        count++;
    pthread_mutex_unlock(&s->mutex);
    return count;
}
