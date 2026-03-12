/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "coins/coins.h"
#include <stdlib.h>
#include <string.h>

void coins_init(struct coins *c)
{
    c->is_coinbase = false;
    c->vout = NULL;
    c->num_vout = 0;
    c->height = 0;
    c->version = 0;
}

void coins_free(struct coins *c)
{
    free(c->vout);
    c->vout = NULL;
    c->num_vout = 0;
}

bool coins_alloc(struct coins *c, size_t num_outputs)
{
    c->vout = calloc(num_outputs, sizeof(struct tx_out));
    if (num_outputs && !c->vout)
        return false;
    c->num_vout = num_outputs;
    for (size_t i = 0; i < num_outputs; i++)
        tx_out_set_null(&c->vout[i]);
    return true;
}

void coins_from_transaction(struct coins *c, const struct transaction *tx, int height)
{
    c->is_coinbase = transaction_is_coinbase(tx);
    c->height = height;
    c->version = tx->version;

    coins_free(c);
    if (!coins_alloc(c, tx->num_vout))
        return;

    for (size_t i = 0; i < tx->num_vout; i++)
        c->vout[i] = tx->vout[i];

    coins_cleanup(c);
}

bool coins_spend(struct coins *c, uint32_t pos)
{
    if (pos >= c->num_vout || tx_out_is_null(&c->vout[pos]))
        return false;
    tx_out_set_null(&c->vout[pos]);
    coins_cleanup(c);
    return true;
}

bool coins_is_available(const struct coins *c, unsigned int pos)
{
    return pos < c->num_vout && !tx_out_is_null(&c->vout[pos]);
}

bool coins_is_pruned(const struct coins *c)
{
    for (size_t i = 0; i < c->num_vout; i++) {
        if (!tx_out_is_null(&c->vout[i]))
            return false;
    }
    return true;
}

void coins_copy(struct coins *dst, const struct coins *src)
{
    dst->is_coinbase = src->is_coinbase;
    dst->height = src->height;
    dst->version = src->version;
    dst->num_vout = src->num_vout;
    if (src->num_vout > 0 && src->vout) {
        dst->vout = malloc(src->num_vout * sizeof(struct tx_out));
        if (dst->vout)
            memcpy(dst->vout, src->vout, src->num_vout * sizeof(struct tx_out));
        else
            dst->num_vout = 0;
    } else {
        dst->vout = NULL;
        dst->num_vout = 0;
    }
}

void coins_cleanup(struct coins *c)
{
    while (c->num_vout > 0 && tx_out_is_null(&c->vout[c->num_vout - 1]))
        c->num_vout--;
}

void coins_stats_init(struct coins_stats *s)
{
    s->height = 0;
    uint256_set_null(&s->hash_block);
    s->num_transactions = 0;
    s->num_tx_outputs = 0;
    s->serialized_size = 0;
    uint256_set_null(&s->hash_serialized);
    s->total_amount = 0;
}
