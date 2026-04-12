/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/checkqueue.h"
#include "util/log_macros.h"
#include <stdlib.h>

bool check_queue_loop(struct check_queue *cq, bool is_master)
{
    zcl_cond_t *cond = is_master ? &cq->cond_master : &cq->cond_worker;
    unsigned int nNow = 0;
    bool fOk = true;

    void **local_batch = malloc(cq->nBatchSize * sizeof(void *));
    if (!local_batch)
        LOG_FAIL("checkqueue", "malloc failed for batch (size=%u)", cq->nBatchSize);

    for (;;) {
        zcl_mutex_lock(&cq->mutex);

        if (nNow) {
            cq->fAllOk &= fOk;
            cq->nTodo -= nNow;
            if (cq->nTodo == 0 && !is_master)
                zcl_cond_signal(&cq->cond_master);
        } else {
            cq->nTotal++;
        }

        while (cq->queue_size == 0) {
            if ((is_master || cq->fQuit) && cq->nTodo == 0) {
                cq->nTotal--;
                bool fRet = cq->fAllOk;
                if (is_master)
                    cq->fAllOk = true;
                zcl_mutex_unlock(&cq->mutex);
                free(local_batch);
                return fRet;
            }
            cq->nIdle++;
            zcl_cond_wait(cond, &cq->mutex);
            cq->nIdle--;
        }

        unsigned int max_batch = cq->nBatchSize;
        unsigned int qsz = (unsigned int)cq->queue_size;
        unsigned int divisor = (unsigned int)(cq->nTotal + cq->nIdle + 1);
        nNow = qsz / divisor;
        if (nNow < 1) nNow = 1;
        if (nNow > max_batch) nNow = max_batch;

        for (unsigned int i = 0; i < nNow; i++) {
            local_batch[i] = cq->queue[cq->queue_size - 1];
            cq->queue_size--;
        }

        fOk = cq->fAllOk;
        zcl_mutex_unlock(&cq->mutex);

        for (unsigned int i = 0; i < nNow; i++) {
            if (fOk)
                fOk = cq->check(local_batch[i]);
            free(local_batch[i]);
        }
    }
}

void check_queue_add(struct check_queue *cq, void **items, size_t count)
{
    zcl_mutex_lock(&cq->mutex);

    size_t needed = cq->queue_size + count;
    if (needed > cq->queue_cap) {
        size_t new_cap = needed * 2;
        if (new_cap < 64) new_cap = 64;
        cq->queue = realloc(cq->queue, new_cap * sizeof(void *));
        cq->queue_cap = new_cap;
    }

    for (size_t i = 0; i < count; i++)
        cq->queue[cq->queue_size++] = items[i];

    cq->nTodo += (unsigned int)count;
    if (count == 1)
        zcl_cond_signal(&cq->cond_worker);
    else if (count > 1)
        zcl_cond_broadcast(&cq->cond_worker);

    zcl_mutex_unlock(&cq->mutex);
}
