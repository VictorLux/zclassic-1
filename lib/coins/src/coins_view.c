/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "coins/coins_view.h"
#include <stdio.h>
#include <string.h>

#define COINS_MAP_INITIAL_BUCKETS 4096
#define COINS_MAP_LOAD_FACTOR_NUM 3
#define COINS_MAP_LOAD_FACTOR_DEN 4

static void coins_map_rehash(struct coins_map *m, size_t new_num_buckets);

struct coins_cache_entry *coins_map_find(struct coins_map *m,
                                          const struct uint256 *txid)
{
    if (m->num_buckets == 0)
        return NULL;
    uint64_t h = coins_map_hash(txid);
    size_t idx = (size_t)(h % m->num_buckets);
    for (size_t i = 0; i < m->num_buckets; i++) {
        size_t slot = (idx + i) % m->num_buckets;
        if (!m->buckets[slot].occupied)
            return NULL;
        if (uint256_cmp(&m->buckets[slot].txid, txid) == 0)
            return &m->buckets[slot].entry;
    }
    return NULL;
}

struct coins_cache_entry *coins_map_insert(struct coins_map *m,
                                            const struct uint256 *txid)
{
    if (m->num_buckets > 0) {
        struct coins_cache_entry *existing = coins_map_find(m, txid);
        if (existing)
            return existing;
    }

    if (m->num_buckets == 0 ||
        m->size * COINS_MAP_LOAD_FACTOR_DEN >=
            m->num_buckets * COINS_MAP_LOAD_FACTOR_NUM) {
        size_t new_cap = m->num_buckets == 0
            ? COINS_MAP_INITIAL_BUCKETS : m->num_buckets * 2;
        coins_map_rehash(m, new_cap);
    }

    uint64_t h = coins_map_hash(txid);
    size_t idx = (size_t)(h % m->num_buckets);
    for (size_t i = 0; i < m->num_buckets; i++) {
        size_t slot = (idx + i) % m->num_buckets;
        if (!m->buckets[slot].occupied) {
            m->buckets[slot].txid = *txid;
            coins_init(&m->buckets[slot].entry.coins);
            m->buckets[slot].entry.flags = 0;
            m->buckets[slot].occupied = true;
            m->size++;
            return &m->buckets[slot].entry;
        }
    }
    return NULL;
}

bool coins_map_erase(struct coins_map *m, const struct uint256 *txid)
{
    if (m->num_buckets == 0)
        return false;
    uint64_t h = coins_map_hash(txid);
    size_t idx = (size_t)(h % m->num_buckets);
    for (size_t i = 0; i < m->num_buckets; i++) {
        size_t slot = (idx + i) % m->num_buckets;
        if (!m->buckets[slot].occupied)
            return false;
        if (uint256_cmp(&m->buckets[slot].txid, txid) == 0) {
            coins_free(&m->buckets[slot].entry.coins);
            m->buckets[slot].occupied = false;
            m->size--;
            /* Rehash subsequent entries to fill the gap */
            size_t next = (slot + 1) % m->num_buckets;
            while (m->buckets[next].occupied) {
                struct coins_map_entry tmp = m->buckets[next];
                m->buckets[next].occupied = false;
                m->size--;
                /* Re-insert */
                uint64_t rh = coins_map_hash(&tmp.txid);
                size_t ri = (size_t)(rh % m->num_buckets);
                for (size_t j = 0; j < m->num_buckets; j++) {
                    size_t rs = (ri + j) % m->num_buckets;
                    if (!m->buckets[rs].occupied) {
                        m->buckets[rs] = tmp;
                        m->size++;
                        break;
                    }
                }
                next = (next + 1) % m->num_buckets;
            }
            return true;
        }
    }
    return false;
}

size_t coins_map_count(const struct coins_map *m)
{
    return m->size;
}

static void coins_map_rehash(struct coins_map *m, size_t new_num_buckets)
{
    struct coins_map_entry *old = m->buckets;
    size_t old_num = m->num_buckets;

    m->buckets = calloc(new_num_buckets, sizeof(struct coins_map_entry));
    if (!m->buckets) {
        m->buckets = old;
        return;
    }
    m->num_buckets = new_num_buckets;
    m->size = 0;

    if (old) {
        for (size_t i = 0; i < old_num; i++) {
            if (old[i].occupied) {
                uint64_t h = coins_map_hash(&old[i].txid);
                size_t idx = (size_t)(h % new_num_buckets);
                for (size_t j = 0; j < new_num_buckets; j++) {
                    size_t slot = (idx + j) % new_num_buckets;
                    if (!m->buckets[slot].occupied) {
                        m->buckets[slot] = old[i];
                        m->size++;
                        break;
                    }
                }
            }
        }
        free(old);
    }
}

void coins_view_cache_init(struct coins_view_cache *c, struct coins_view *backing)
{
    c->base = *backing;
    coins_map_init(&c->cache_coins);
    uint256_set_null(&c->hash_block);
    c->cached_coins_usage = 0;
    utxo_commitment_init(&c->commitment);
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
    size_t written = 0, pruned = 0, skipped = 0;

    for (size_t i = 0; i < map_coins->num_buckets; i++) {
        struct coins_map_entry *e = &map_coins->buckets[i];
        if (!e->occupied)
            continue;
        if (e->entry.flags & COINS_CACHE_DIRTY) {
            if (coins_is_pruned(&e->entry.coins)) {
                coins_map_erase(&parent->cache_coins, &e->txid);
                pruned++;
            } else {
                struct coins_cache_entry *dest =
                    coins_map_insert(&parent->cache_coins, &e->txid);
                coins_free(&dest->coins);
                dest->coins = e->entry.coins;
                coins_init(&e->entry.coins);
                dest->flags |= COINS_CACHE_DIRTY;
                written++;
            }
        } else {
            skipped++;
        }
    }

    if (!uint256_is_null(hash_block))
        parent->hash_block = *hash_block;

    /* Diagnostic: log if no entries were written (indicates a bug) */
    if (written == 0 && map_coins->size > 0) {
        fprintf(stderr, "cvc_batch_write: WARNING wrote 0 entries "
                "(map_size=%zu pruned=%zu skipped=%zu)\n",
                map_coins->size, pruned, skipped);
    }

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

    /* Merge this cache's commitment delta into the parent cache.
     * If the parent is a coins_view_cache (child→parent flush), the
     * commitment accumulates upward. If it's LevelDB (top-level flush),
     * cvc_batch_write is not used and the commitment is persisted
     * separately by flush_coins_if_needed in process_block.c. */
    if (ok && c->base.vtable == &g_cache_vtable) {
        struct coins_view_cache *parent = (struct coins_view_cache *)c->base.impl;
        utxo_commitment_merge(&parent->commitment, &c->commitment);
    }

    coins_map_free(&c->cache_coins);
    coins_map_init(&c->cache_coins);
    /* Reset child commitment after flush (deltas merged into parent) */
    utxo_commitment_init(&c->commitment);
    return ok;
}

const struct tx_out *coins_view_cache_get_output_for(
    struct coins_view_cache *c, const struct tx_in *in)
{
    struct coins_cache_entry *entry =
        coins_map_find(&c->cache_coins, &in->prevout.hash);
    if (!entry) {
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
    if (tx->value_balance >= 0)
        value += tx->value_balance;
    for (size_t i = 0; i < tx->num_joinsplit; i++)
        value += tx->v_joinsplit[i].vpub_new;
    return value;
}

bool coins_view_cache_have_joinsplit_requirements(
    struct coins_view_cache *c, const struct transaction *tx)
{
    (void)c;
    (void)tx;
    return true;
}
