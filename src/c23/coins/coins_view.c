/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "coins/coins_view.h"
#include <string.h>

struct coins_cache_entry *coins_map_find(struct coins_map *m,
                                          const struct uint256 *txid)
{
    for (size_t i = 0; i < m->size; i++) {
        if (uint256_cmp(&m->entries[i].txid, txid) == 0)
            return &m->entries[i].entry;
    }
    return NULL;
}

struct coins_cache_entry *coins_map_insert(struct coins_map *m,
                                            const struct uint256 *txid)
{
    struct coins_cache_entry *existing = coins_map_find(m, txid);
    if (existing)
        return existing;

    if (m->size >= m->capacity) {
        size_t new_cap = m->capacity == 0 ? 64 : m->capacity * 2;
        m->entries = realloc(m->entries, new_cap * sizeof(struct coins_map_entry));
        m->capacity = new_cap;
    }

    struct coins_map_entry *e = &m->entries[m->size++];
    e->txid = *txid;
    coins_init(&e->entry.coins);
    e->entry.flags = 0;
    return &e->entry;
}

bool coins_map_erase(struct coins_map *m, const struct uint256 *txid)
{
    for (size_t i = 0; i < m->size; i++) {
        if (uint256_cmp(&m->entries[i].txid, txid) == 0) {
            coins_free(&m->entries[i].entry.coins);
            m->entries[i] = m->entries[m->size - 1];
            m->size--;
            return true;
        }
    }
    return false;
}

size_t coins_map_count(const struct coins_map *m)
{
    return m->size;
}

void coins_view_cache_init(struct coins_view_cache *c, struct coins_view *backing)
{
    c->base = *backing;
    coins_map_init(&c->cache_coins);
    uint256_set_null(&c->hash_block);
    c->cached_coins_usage = 0;
}

void coins_view_cache_free(struct coins_view_cache *c)
{
    coins_map_free(&c->cache_coins);
}

bool coins_view_cache_get_coins(struct coins_view_cache *c,
                                const struct uint256 *txid,
                                struct coins *out)
{
    struct coins_cache_entry *entry = coins_map_find(&c->cache_coins, txid);
    if (entry) {
        if (coins_is_pruned(&entry->coins))
            return false;
        *out = entry->coins;
        return true;
    }

    struct coins fetched;
    coins_init(&fetched);
    if (coins_view_get_coins(&c->base, txid, &fetched)) {
        struct coins_cache_entry *new_entry =
            coins_map_insert(&c->cache_coins, txid);
        new_entry->coins = fetched;
        if (coins_is_pruned(&new_entry->coins))
            return false;
        *out = new_entry->coins;
        return true;
    }
    return false;
}

bool coins_view_cache_have_coins(struct coins_view_cache *c,
                                 const struct uint256 *txid)
{
    struct coins_cache_entry *entry = coins_map_find(&c->cache_coins, txid);
    if (entry)
        return !coins_is_pruned(&entry->coins);
    struct coins tmp;
    coins_init(&tmp);
    bool has = coins_view_get_coins(&c->base, txid, &tmp);
    coins_free(&tmp);
    return has;
}

void coins_view_cache_get_best_block(struct coins_view_cache *c,
                                     struct uint256 *out)
{
    if (uint256_is_null(&c->hash_block))
        coins_view_get_best_block(&c->base, &c->hash_block);
    *out = c->hash_block;
}

void coins_view_cache_set_best_block(struct coins_view_cache *c,
                                     const struct uint256 *hash)
{
    c->hash_block = *hash;
}

struct coins_cache_entry *coins_view_cache_modify(struct coins_view_cache *c,
                                                   const struct uint256 *txid)
{
    struct coins_cache_entry *entry = coins_map_find(&c->cache_coins, txid);
    if (entry) {
        entry->flags |= COINS_CACHE_DIRTY;
        return entry;
    }

    struct coins_cache_entry *new_entry = coins_map_insert(&c->cache_coins, txid);
    coins_view_get_coins(&c->base, txid, &new_entry->coins);
    new_entry->flags |= COINS_CACHE_DIRTY;
    return new_entry;
}

struct coins_cache_entry *coins_view_cache_modify_new(struct coins_view_cache *c,
                                                       const struct uint256 *txid)
{
    struct coins_cache_entry *entry = coins_map_insert(&c->cache_coins, txid);
    entry->flags |= COINS_CACHE_DIRTY | COINS_CACHE_FRESH;
    return entry;
}

bool coins_view_cache_flush(struct coins_view_cache *c)
{
    if (!c->base.vtable || !c->base.vtable->batch_write)
        return false;
    bool ok = c->base.vtable->batch_write(c->base.impl, &c->cache_coins,
                                          &c->hash_block);
    coins_map_free(&c->cache_coins);
    coins_map_init(&c->cache_coins);
    return ok;
}

const struct tx_out *coins_view_cache_get_output_for(
    struct coins_view_cache *c, const struct tx_in *in)
{
    struct coins_cache_entry *entry =
        coins_map_find(&c->cache_coins, &in->prevout.hash);
    if (!entry)
        return NULL;
    if (in->prevout.n >= entry->coins.num_vout)
        return NULL;
    return &entry->coins.vout[in->prevout.n];
}
