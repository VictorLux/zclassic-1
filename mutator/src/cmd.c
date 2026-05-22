/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton */

#include "mutator/cmd.h"

#include <pthread.h>

void mutator_cmd_completion_init(struct mutator_cmd_completion *c)
{
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->done = false;
    c->result = ZCL_OK;
}

void mutator_cmd_completion_destroy(struct mutator_cmd_completion *c)
{
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
}

struct zcl_result mutator_cmd_completion_wait(struct mutator_cmd_completion *c)
{
    pthread_mutex_lock(&c->mu);
    while (!c->done)
        pthread_cond_wait(&c->cv, &c->mu);
    struct zcl_result r = c->result;
    pthread_mutex_unlock(&c->mu);
    return r;
}

void mutator_cmd_completion_post(struct mutator_cmd_completion *c,
                                 struct zcl_result r)
{
    pthread_mutex_lock(&c->mu);
    if (!c->done) {
        c->result = r;
        c->done = true;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
}
