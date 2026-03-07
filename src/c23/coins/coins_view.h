/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_COINS_VIEW_H
#define ZCL_COINS_VIEW_H

#include "coins/coins.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct coins_cache_entry {
    struct coins coins;
    unsigned char flags;
};

#define COINS_MAP_BUCKET_COUNT 1024

struct coins_map_entry {
    struct uint256 txid;
    struct coins_cache_entry entry;
};

struct coins_map {
    struct coins_map_entry *entries;
    size_t size;
    size_t capacity;
};

static inline void coins_map_init(struct coins_map *m)
{
    m->entries = NULL;
    m->size = 0;
    m->capacity = 0;
}

static inline void coins_map_free(struct coins_map *m)
{
    for (size_t i = 0; i < m->size; i++)
        coins_free(&m->entries[i].entry.coins);
    free(m->entries);
    m->entries = NULL;
    m->size = 0;
    m->capacity = 0;
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
};

void coins_view_cache_init(struct coins_view_cache *c, struct coins_view *backing);
void coins_view_cache_free(struct coins_view_cache *c);

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
bool coins_view_cache_flush(struct coins_view_cache *c);

const struct tx_out *coins_view_cache_get_output_for(
    struct coins_view_cache *c, const struct tx_in *in);

#endif
