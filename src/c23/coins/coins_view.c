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
        struct coins_map_entry *ne = realloc(m->entries,
            new_cap * sizeof(struct coins_map_entry));
        if (!ne) return NULL;
        m->entries = ne;
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
        coins_copy(out, &entry->coins);
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
        coins_copy(out, &new_entry->coins);
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

/* vtable for using a coins_view_cache as a backing store */
static bool cvc_get_coins(void *self, const struct uint256 *txid,
                           struct coins *coins)
{
    return coins_view_cache_get_coins((struct coins_view_cache *)self,
                                      txid, coins);
}

static bool cvc_have_coins(void *self, const struct uint256 *txid)
{
    return coins_view_cache_have_coins((struct coins_view_cache *)self, txid);
}

static bool cvc_get_best_block(void *self, struct uint256 *hash)
{
    coins_view_cache_get_best_block((struct coins_view_cache *)self, hash);
    return true;
}

static bool cvc_batch_write(void *self, struct coins_map *map_coins,
                             const struct uint256 *hash_block)
{
    struct coins_view_cache *parent = (struct coins_view_cache *)self;

    for (size_t i = 0; i < map_coins->size; i++) {
        struct coins_map_entry *e = &map_coins->entries[i];
        if (e->entry.flags & COINS_CACHE_DIRTY) {
            struct coins_cache_entry *dest =
                coins_map_insert(&parent->cache_coins, &e->txid);
            if (coins_is_pruned(&e->entry.coins)) {
                coins_free(&dest->coins);
                coins_init(&dest->coins);
            } else {
                coins_free(&dest->coins);
                dest->coins = e->entry.coins;
                coins_init(&e->entry.coins);
            }
            dest->flags |= COINS_CACHE_DIRTY;
        }
    }

    if (!uint256_is_null(hash_block))
        parent->hash_block = *hash_block;

    return true;
}

static struct coins_view_vtable g_cache_vtable = {
    .get_coins = cvc_get_coins,
    .have_coins = cvc_have_coins,
    .get_best_block = cvc_get_best_block,
    .batch_write = cvc_batch_write,
    .get_stats = NULL,
};

void coins_view_cache_as_view(struct coins_view *out,
                               struct coins_view_cache *cache)
{
    out->vtable = &g_cache_vtable;
    out->impl = cache;
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
    if (!entry) {
        /* Try fetching from backing store */
        struct coins coins;
        coins_init(&coins);
        if (!coins_view_get_coins(&c->base, &in->prevout.hash, &coins)) {
            coins_free(&coins);
            return NULL;
        }
        struct coins_cache_entry *new_entry =
            coins_map_insert(&c->cache_coins, &in->prevout.hash);
        new_entry->coins = coins;
        new_entry->flags = 0;
        entry = new_entry;
    }
    if (in->prevout.n >= entry->coins.num_vout)
        return NULL;
    if (tx_out_is_null(&entry->coins.vout[in->prevout.n]))
        return NULL;
    return &entry->coins.vout[in->prevout.n];
}

bool coins_view_cache_have_inputs(struct coins_view_cache *c,
                                   const struct transaction *tx)
{
    if (transaction_is_coinbase(tx))
        return true;
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct tx_out *out = coins_view_cache_get_output_for(c, &tx->vin[i]);
        if (!out)
            return false;
    }
    return true;
}

int64_t coins_view_cache_get_value_in(struct coins_view_cache *c,
                                       const struct transaction *tx)
{
    if (transaction_is_coinbase(tx))
        return 0;
    int64_t value = 0;
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct tx_out *out = coins_view_cache_get_output_for(c, &tx->vin[i]);
        if (out)
            value += out->value;
    }
    /* Add Sapling value balance (positive = inputs to transparent pool) */
    if (tx->value_balance >= 0)
        value += tx->value_balance;
    /* Add JoinSplit vpub_new (moves from shielded to transparent) */
    for (size_t i = 0; i < tx->num_joinsplit; i++)
        value += tx->v_joinsplit[i].vpub_new;
    return value;
}

bool coins_view_cache_have_joinsplit_requirements(
    struct coins_view_cache *c, const struct transaction *tx)
{
    /* For now, JoinSplit anchor validation requires the incremental merkle
     * tree infrastructure in the coins view. This is a placeholder that
     * returns true — full anchor checking will be implemented when the
     * merkle tree is integrated into the coins view. */
    (void)c;
    (void)tx;
    return true;
}
