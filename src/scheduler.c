/* Copyright (c) 2015 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "scheduler.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* Windows implementation using CRITICAL_SECTION + CONDITION_VARIABLE */

static int64_t win_time_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    return (int64_t)(t / 10000);
}

void scheduler_init(struct scheduler *s)
{
    memset(s, 0, sizeof(*s));
    InitializeCriticalSection(&s->mutex);
    InitializeConditionVariable(&s->cond);
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
    DeleteCriticalSection(&s->mutex);
}

static bool should_stop(struct scheduler *s)
{
    return s->stop_requested || (s->stop_when_empty && s->queue == NULL);
}

void scheduler_service_queue(struct scheduler *s)
{
    EnterCriticalSection(&s->mutex);
    s->threads_servicing++;

    while (!should_stop(s)) {
        while (!should_stop(s) && s->queue == NULL)
            SleepConditionVariableCS(&s->cond, &s->mutex, INFINITE);

        if (should_stop(s) || s->queue == NULL)
            continue;

        int64_t now_ms = win_time_ms();
        int64_t when_ms = s->queue->when_ms;

        if (when_ms > now_ms) {
            DWORD wait = (DWORD)(when_ms - now_ms);
            SleepConditionVariableCS(&s->cond, &s->mutex, wait);
            continue;
        }

        struct scheduler_task *task = s->queue;
        s->queue = task->next;

        LeaveCriticalSection(&s->mutex);
        task->func(task->ctx);
        free(task);
        EnterCriticalSection(&s->mutex);
    }

    s->threads_servicing--;
    LeaveCriticalSection(&s->mutex);
}

void scheduler_stop(struct scheduler *s, bool drain)
{
    EnterCriticalSection(&s->mutex);
    if (drain)
        s->stop_when_empty = true;
    else
        s->stop_requested = true;
    LeaveCriticalSection(&s->mutex);
    WakeAllConditionVariable(&s->cond);
}

void scheduler_schedule(struct scheduler *s, scheduler_func f, void *ctx, int64_t when_ms)
{
    struct scheduler_task *task = malloc(sizeof(*task));
    task->func = f;
    task->ctx = ctx;
    task->when_ms = when_ms;
    task->next = NULL;

    EnterCriticalSection(&s->mutex);
    struct scheduler_task **pp = &s->queue;
    while (*pp && (*pp)->when_ms <= when_ms)
        pp = &(*pp)->next;
    task->next = *pp;
    *pp = task;
    LeaveCriticalSection(&s->mutex);
    WakeConditionVariable(&s->cond);
}

void scheduler_schedule_from_now(struct scheduler *s, scheduler_func f, void *ctx, int64_t delta_seconds)
{
    scheduler_schedule(s, f, ctx, win_time_ms() + delta_seconds * 1000);
}

#else /* POSIX */

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

void scheduler_schedule(struct scheduler *s, scheduler_func f, void *ctx, int64_t when_ms)
{
    struct scheduler_task *task = malloc(sizeof(*task));
    task->func = f;
    task->ctx = ctx;
    task->when.tv_sec = when_ms / 1000;
    task->when.tv_nsec = (when_ms % 1000) * 1000000;
    task->next = NULL;

    pthread_mutex_lock(&s->mutex);
    struct scheduler_task **pp = &s->queue;
    while (*pp && timespec_cmp((*pp)->when, task->when) <= 0)
        pp = &(*pp)->next;
    task->next = *pp;
    *pp = task;
    pthread_mutex_unlock(&s->mutex);
    pthread_cond_signal(&s->cond);
}

void scheduler_schedule_from_now(struct scheduler *s, scheduler_func f, void *ctx, int64_t delta_seconds)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    scheduler_schedule(s, f, ctx, now_ms + delta_seconds * 1000);
}

#endif /* _WIN32 */

size_t scheduler_queue_size(struct scheduler *s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->mutex);
    size_t count = 0;
    for (struct scheduler_task *t = s->queue; t; t = t->next)
        count++;
    LeaveCriticalSection(&s->mutex);
    return count;
#else
    pthread_mutex_lock(&s->mutex);
    size_t count = 0;
    for (struct scheduler_task *t = s->queue; t; t = t->next)
        count++;
    pthread_mutex_unlock(&s->mutex);
    return count;
#endif
}
