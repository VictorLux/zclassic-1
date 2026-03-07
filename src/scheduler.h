/* Copyright (c) 2015 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_SCHEDULER_H
#define BITCOIN_SCHEDULER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef void (*scheduler_func)(void *ctx);

struct scheduler_task {
    struct timespec when;
    scheduler_func func;
    void *ctx;
    struct scheduler_task *next;
};

struct scheduler {
    struct scheduler_task *queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int threads_servicing;
    bool stop_requested;
    bool stop_when_empty;
};

void scheduler_init(struct scheduler *s);
void scheduler_destroy(struct scheduler *s);
void scheduler_service_queue(struct scheduler *s);
void scheduler_stop(struct scheduler *s, bool drain);
void scheduler_schedule(struct scheduler *s, scheduler_func f, void *ctx, struct timespec when);
void scheduler_schedule_from_now(struct scheduler *s, scheduler_func f, void *ctx, int64_t delta_seconds);
size_t scheduler_queue_size(struct scheduler *s);

#endif
