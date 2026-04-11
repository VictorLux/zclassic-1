/* Copyright (c) 2015 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_SCHEDULER_H
#define BITCOIN_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

typedef void (*scheduler_func)(void *ctx);

struct scheduler_task {
#ifdef _WIN32
    int64_t when_ms;
#else
    struct timespec when;
#endif
    scheduler_func func;
    void *ctx;
    struct scheduler_task *next;
};

struct scheduler {
    struct scheduler_task *queue;
#ifdef _WIN32
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
    int threads_servicing;
    bool stop_requested;
    bool stop_when_empty;
};

void zcl_scheduler_init(struct scheduler *s);
#define scheduler_init zcl_scheduler_init
void scheduler_destroy(struct scheduler *s);
void scheduler_service_queue(struct scheduler *s);
void scheduler_stop(struct scheduler *s, bool drain);
void scheduler_schedule(struct scheduler *s, scheduler_func f, void *ctx, int64_t when_ms);
void scheduler_schedule_from_now(struct scheduler *s, scheduler_func f, void *ctx, int64_t delta_seconds);
size_t scheduler_queue_size(struct scheduler *s);

#endif
