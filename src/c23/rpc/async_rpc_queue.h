/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_ASYNC_RPC_QUEUE_H
#define ZCL_ASYNC_RPC_QUEUE_H

#include "rpc/async_rpc_operation.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

#define MAX_ASYNC_OPS 256
#define MAX_ASYNC_WORKERS 8

struct async_rpc_queue {
    zcl_mutex_t lock;
    zcl_cond_t cond;
    _Atomic bool closed;
    _Atomic bool finishing;

    struct async_rpc_operation *ops[MAX_ASYNC_OPS];
    size_t num_ops;
    char op_queue[MAX_ASYNC_OPS][ASYNC_OP_ID_SIZE];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;

    pthread_t workers[MAX_ASYNC_WORKERS];
    size_t num_workers;
};

void async_queue_init(struct async_rpc_queue *q);
void async_queue_free(struct async_rpc_queue *q);

void async_queue_add_worker(struct async_rpc_queue *q);
size_t async_queue_num_workers(const struct async_rpc_queue *q);

bool async_queue_is_closed(const struct async_rpc_queue *q);
bool async_queue_is_finishing(const struct async_rpc_queue *q);

void async_queue_close(struct async_rpc_queue *q);
void async_queue_finish(struct async_rpc_queue *q);
void async_queue_close_and_wait(struct async_rpc_queue *q);
void async_queue_finish_and_wait(struct async_rpc_queue *q);
void async_queue_cancel_all(struct async_rpc_queue *q);

size_t async_queue_op_count(const struct async_rpc_queue *q);
void async_queue_add_op(struct async_rpc_queue *q,
                        struct async_rpc_operation *op);
struct async_rpc_operation *async_queue_get_op(struct async_rpc_queue *q,
                                               const char *id);
struct async_rpc_operation *async_queue_pop_op(struct async_rpc_queue *q,
                                               const char *id);

size_t async_queue_get_all_ids(const struct async_rpc_queue *q,
                               char ids[][ASYNC_OP_ID_SIZE],
                               size_t max_ids);

#endif
