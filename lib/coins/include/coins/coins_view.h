/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_COINS_VIEW_H
#define ZCL_COINS_VIEW_H

#include "coins/coins.h"
#include "coins/utxo_commitment.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct coins_cache_entry {
    struct coins coins;
    unsigned char flags;
};

struct coins_map_entry {
    struct uint256 txid;
    struct coins_cache_entry entry;
    bool occupied;
};

struct coins_map {
    struct coins_map_entry *buckets;
    size_t num_buckets;
    size_t size;
};

static inline uint64_t coins_map_hash(const struct uint256 *txid)
{
    uint64_t h;
    memcpy(&h, txid->data, 8);
    return h;
}

static inline void coins_map_init(struct coins_map *m)
{
    m->buckets = NULL;
    m->num_buckets = 0;
    m->size = 0;
}

static inline void coins_map_free(struct coins_map *m)
{
    for (size_t i = 0; i < m->num_buckets; i++) {
        if (m->buckets[i].occupied)
            coins_free(&m->buckets[i].entry.coins);
    }
    free(m->buckets);
    m->buckets = NULL;
    m->num_buckets = 0;
    m->size = 0;
}

struct coins_cache_entry *coins_map_find(struct coins_map *m,
                                          const struct uint256 *txid);
struct coins_cache_entry *coins_map_insert(struct coins_map *m,
                                            const struct uint256 *txid);
bool coins_map_erase(struct coins_map *m, const struct uint256 *txid);
size_t coins_map_count(const struct coins_map *m);

struct coins_view_vtable {
    bool (*get_coins)(void *self, const struct uint256 *txid, struct coins *coins);
    bool (*have_coins)(void *self, const struct uint256 *txid);
    bool (*get_best_block)(void *self, struct uint256 *hash);
    bool (*batch_write)(void *self, struct coins_map *map_coins,
                        const struct uint256 *hash_block);
    bool (*get_stats)(void *self, struct coins_stats *stats);
};

struct coins_view {
    struct coins_view_vtable *vtable;
    void *impl;
};

static inline bool coins_view_get_coins(struct coins_view *cv,
                                        const struct uint256 *txid,
                                        struct coins *coins)
{
    if (cv->vtable && cv->vtable->get_coins)
        return cv->vtable->get_coins(cv->impl, txid, coins);
    return false;
}

static inline bool coins_view_have_coins(struct coins_view *cv,
                                         const struct uint256 *txid)
{
    if (cv->vtable && cv->vtable->have_coins)
        return cv->vtable->have_coins(cv->impl, txid);
    return false;
}

static inline bool coins_view_get_best_block(struct coins_view *cv,
                                             struct uint256 *hash)
{
    if (cv->vtable && cv->vtable->get_best_block)
        return cv->vtable->get_best_block(cv->impl, hash);
    return false;
}

struct coins_view_cache {
    struct coins_view base;
    struct coins_map cache_coins;
    struct uint256 hash_block;
    size_t cached_coins_usage;
    struct utxo_commitment commitment;  /* incremental UTXO set hash */
};

void coins_view_cache_init(struct coins_view_cache *c, struct coins_view *backing);
void coins_view_cache_free(struct coins_view_cache *c);
void coins_view_cache_as_view(struct coins_view *out,
                               struct coins_view_cache *cache);

/* Authoritative, PATH-INDEPENDENT UTXO commitment for this cache.
 *
 * Recomputes the XOR-hash accumulator + count directly from the live coin
 * SET (every available output of every non-pruned entry currently held in
 * cache_coins), rather than reading the incremental `c->commitment` field.
 *
 * Why this exists: the incremental `c->commitment` accumulator is maintained
 * only on the FORWARD path (update_coins add/remove). disconnect_block does
 * NOT decrement it, so after a chain reorg `c->commitment` is path-DEPENDENT:
 * it reflects the history of connects/disconnects rather than the resulting
 * coin set. Any commitment query that must hold across a reorg MUST therefore
 * recompute from the coin set — this function — and may
 * NOT trust the stale incremental field.
 *
 * Equivalence guarantee: for a coin set produced by forward-only connects
 * (no disconnect), the recomputed value is BYTE-IDENTICAL to the incremental
 * `c->commitment` (same per-UTXO hash inputs: txid, vout, value, creation
 * height; XOR is commutative so iteration order is irrelevant). The persisted
 * forward-only commitment value is therefore unchanged — existing snapshots
 * stay valid.
 *
 * Cost: O(N) over the cache's live entries. Call it on-demand / for a proof
 * or checkpoint — NOT on the per-block hot path (the incremental field
 * already serves the forward path at O(1) per change).
 *
 * Only the in-memory cache is iterated; the backing view is not consulted
 * (the live tip cache holds the full set during validation). */
void coins_view_cache_recompute_commitment(const struct coins_view_cache *c,
                                            struct utxo_commitment *out);

bool coins_view_cache_get_coins(struct coins_view_cache *c,
                                const struct uint256 *txid,
                                struct coins *out);
bool coins_view_cache_have_coins(struct coins_view_cache *c,
                                 const struct uint256 *txid);
void coins_view_cache_get_best_block(struct coins_view_cache *c,
                                     struct uint256 *out);
void coins_view_cache_set_best_block(struct coins_view_cache *c,
                                     const struct uint256 *hash);
struct coins_cache_entry *coins_view_cache_modify(struct coins_view_cache *c,
                                                   const struct uint256 *txid);
struct coins_cache_entry *coins_view_cache_modify_new(struct coins_view_cache *c,
                                                       const struct uint256 *txid);
#ifdef ZCL_TESTING
bool coins_view_cache_flush_for_testing(struct coins_view_cache *c);
#endif

/* Clear all cached entries without flushing to backing store.
 * Used during reorg recovery to discard stale UTXO cache entries.
 * Does NOT touch hash_block — caller must set it explicitly. */
void coins_view_cache_clear(struct coins_view_cache *c);

const struct tx_out *coins_view_cache_get_output_for(
    struct coins_view_cache *c, const struct tx_in *in);

bool coins_view_cache_have_inputs(struct coins_view_cache *c,
                                   const struct transaction *tx);

int64_t coins_view_cache_get_value_in(struct coins_view_cache *c,
                                       const struct transaction *tx);

bool coins_view_cache_have_joinsplit_requirements(
    struct coins_view_cache *c, const struct transaction *tx);

#endif
