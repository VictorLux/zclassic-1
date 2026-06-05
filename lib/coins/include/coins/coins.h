/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_COINS_H
#define ZCL_COINS_H

#include "primitives/transaction.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define COINS_CACHE_DIRTY (1 << 0)
#define COINS_CACHE_FRESH (1 << 1)

struct coins {
    bool is_coinbase;
    struct tx_out *vout;
    size_t num_vout;
    int height;
    int version;
};

/* Produce a pruned/empty record (vout=NULL, num_vout=0); never fails. */
void coins_init(struct coins *c);
void coins_free(struct coins *c);
void coins_copy(struct coins *dst, const struct coins *src);
/* Allocate num_outputs null txouts. Returns false and leaves an empty
 * (vout=NULL, num_vout=0) record on OOM — never a partial/NULL-deref. */
bool coins_alloc(struct coins *c, size_t num_outputs);
/* Build a record from tx: skips OP_RETURN/unspendable outputs and caps at
 * 65536 outputs. On OOM (or over-cap) it leaves an empty (num_vout=0)
 * record, which callers MUST treat as failure — never as "fully pruned". */
void coins_from_transaction(struct coins *c, const struct transaction *tx, int height);
bool coins_spend(struct coins *c, uint32_t pos);
bool coins_is_available(const struct coins *c, unsigned int pos);
bool coins_is_pruned(const struct coins *c);
void coins_cleanup(struct coins *c);

struct coins_stats {
    int height;
    struct uint256 hash_block;
    uint64_t num_transactions;
    uint64_t num_tx_outputs;
    uint64_t serialized_size;
    struct uint256 hash_serialized;
    int64_t total_amount;
};

void coins_stats_init(struct coins_stats *s);

#endif
