/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHECKQUEUE_H
#define ZCL_CHECKQUEUE_H

#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*check_fn)(void *item);

#define CHECK_QUEUE_MAX_ITEMS 65536

struct check_queue {
    zcl_mutex_t mutex;
    zcl_cond_t cond_worker;
    zcl_cond_t cond_master;

    void **queue;
    size_t queue_size;
    size_t queue_cap;

    size_t item_size;
    check_fn check;

    int nIdle;
    int nTotal;
    bool fAllOk;
    unsigned int nTodo;
    bool fQuit;
    unsigned int nBatchSize;
};

static inline void check_queue_init(struct check_queue *cq,
                                    unsigned int batch_size,
                                    size_t item_size,
                                    check_fn fn)
{
    zcl_mutex_init(&cq->mutex);
    zcl_cond_init(&cq->cond_worker);
    zcl_cond_init(&cq->cond_master);
    cq->queue = NULL;
    cq->queue_size = 0;
    cq->queue_cap = 0;
    cq->item_size = item_size;
    cq->check = fn;
    cq->nIdle = 0;
    cq->nTotal = 0;
    cq->fAllOk = true;
    cq->nTodo = 0;
    cq->fQuit = false;
    cq->nBatchSize = batch_size;
}

static inline void check_queue_free(struct check_queue *cq)
{
    for (size_t i = 0; i < cq->queue_size; i++)
        free(cq->queue[i]);
    free(cq->queue);
    zcl_cond_destroy(&cq->cond_worker);
    zcl_cond_destroy(&cq->cond_master);
    zcl_mutex_destroy(&cq->mutex);
}

bool check_queue_loop(struct check_queue *cq, bool is_master);
void check_queue_add(struct check_queue *cq, void **items, size_t count);

static inline bool check_queue_wait(struct check_queue *cq)
{
    return check_queue_loop(cq, true);
}

static inline void check_queue_thread(struct check_queue *cq)
{
    check_queue_loop(cq, false);
}

static inline bool check_queue_is_idle(struct check_queue *cq)
{
    zcl_mutex_lock(&cq->mutex);
    bool idle = (cq->nTotal == cq->nIdle && cq->nTodo == 0 && cq->fAllOk);
    zcl_mutex_unlock(&cq->mutex);
    return idle;
}

#endif
