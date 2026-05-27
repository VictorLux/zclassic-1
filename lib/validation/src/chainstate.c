/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/chainstate.h"
#include "util/log_macros.h"
#include "storage/progress_store.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

/* --- Block Map (open-addressing hash table) --- */

static uint64_t block_map_hash(const struct uint256 *h)
{
    uint64_t v;
    memcpy(&v, h->data, 8);
    return v;
}

static bool block_map_grow(struct block_map *m);

void block_map_init(struct block_map *m)
{
    m->buckets = NULL;
    m->size = 0;
    m->capacity = 0;
    pthread_rwlock_init(&m->rwlock, NULL);
}

void block_map_free(struct block_map *m)
{
    pthread_rwlock_wrlock(&m->rwlock);
    /* Only free individually-allocated entries. Arena-allocated entries
     * (from bulk flat-file load) are in a contiguous block — freeing
     * individual pointers from an arena causes double-free/invalid-free.
     * We detect arena entries by checking if adjacent entries in the
     * bucket array point to contiguous memory. For simplicity, skip
     * freeing entirely — block_index lives for process lifetime. */
    free(m->buckets);
    m->buckets = NULL;
    m->size = 0;
    m->capacity = 0;
    pthread_rwlock_unlock(&m->rwlock);
    pthread_rwlock_destroy(&m->rwlock);
}

struct block_index *block_map_find(const struct block_map *m,
                                    const struct uint256 *hash)
{
    if (m->capacity == 0) return NULL;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&m->rwlock);
    uint64_t h = block_map_hash(hash);
    size_t idx = h & (m->capacity - 1);
    struct block_index *result = NULL;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t slot = (idx + i) & (m->capacity - 1);
        if (!m->buckets[slot].occupied)
            break;
        if (uint256_eq(&m->buckets[slot].hash, hash)) {
            result = m->buckets[slot].index;
            break;
        }
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&m->rwlock);
    return result;
}

static bool block_map_insert_internal(struct block_map *m,
                                       const struct uint256 *hash,
                                       struct block_index *index)
{
    uint64_t h = block_map_hash(hash);
    size_t idx = h & (m->capacity - 1);
    for (size_t i = 0; i < m->capacity; i++) {
        size_t slot = (idx + i) & (m->capacity - 1);
        if (!m->buckets[slot].occupied) {
            m->buckets[slot].hash = *hash;
            m->buckets[slot].index = index;
            m->buckets[slot].occupied = true;
            return true;
        }
        if (uint256_eq(&m->buckets[slot].hash, hash))
            LOG_FAIL("chainstate", "block_map_insert_internal: duplicate hash in slot %zu", slot);
    }
    LOG_FAIL("chainstate", "block_map_insert_internal: hash table full (capacity=%zu)", m->capacity);
}

/* Pre-allocate hash map to avoid repeated rehashing during bulk load.
 * next_power_of_2(n * 2) gives ~50% load factor for good probe performance. */
bool block_map_reserve(struct block_map *m, size_t expected_count)
{
    size_t target = expected_count * 2;
    size_t cap = 4096;
    while (cap < target) cap *= 2;
    if (cap <= m->capacity) return true;

    pthread_rwlock_wrlock(&m->rwlock);
    struct block_map_entry *old = m->buckets;
    size_t old_cap = m->capacity;

    m->buckets = zcl_calloc(cap, sizeof(struct block_map_entry), "block_map_buckets");
    if (!m->buckets) {
        m->buckets = old;
        pthread_rwlock_unlock(&m->rwlock);
        LOG_FAIL("chainstate", "block_map_reserve: calloc failed for %zu entries", cap);
    }
    m->capacity = cap;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].occupied)
            block_map_insert_internal(m, &old[i].hash, old[i].index);
    }
    free(old);
    for (size_t i = 0; i < cap; i++) {
        if (m->buckets[i].occupied && m->buckets[i].index)
            m->buckets[i].index->phashBlock = &m->buckets[i].hash;
    }
    pthread_rwlock_unlock(&m->rwlock);
    return true;
}

static bool block_map_grow(struct block_map *m)
{
    size_t new_cap = m->capacity ? m->capacity * 2 : 4096;
    struct block_map_entry *old = m->buckets;
    size_t old_cap = m->capacity;

    m->buckets = zcl_calloc(new_cap, sizeof(struct block_map_entry), "block_map_buckets_grow");
    if (!m->buckets) {
        m->buckets = old;
        LOG_FAIL("chainstate", "block_map_grow: calloc failed for %zu entries", new_cap);
    }
    m->capacity = new_cap;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].occupied)
            block_map_insert_internal(m, &old[i].hash, old[i].index);
    }
    free(old);

    /* Fix phashBlock pointers: they pointed into old buckets which are now freed.
     * Re-point each block_index's phashBlock to the new bucket location. */
    for (size_t i = 0; i < new_cap; i++) {
        if (m->buckets[i].occupied && m->buckets[i].index)
            m->buckets[i].index->phashBlock = &m->buckets[i].hash;
    }

    return true;
}

bool block_map_insert(struct block_map *m, const struct uint256 *hash,
                      struct block_index *index)
{
    pthread_rwlock_wrlock(&m->rwlock);
    if (m->size * 4 >= m->capacity * 3) {
        if (!block_map_grow(m)) {
            pthread_rwlock_unlock(&m->rwlock);
            LOG_FAIL("chainstate", "block_map_insert: grow failed (size=%zu)", m->size);
        }
    }
    if (!block_map_insert_internal(m, hash, index)) {
        pthread_rwlock_unlock(&m->rwlock);
        LOG_FAIL("chainstate", "block_map_insert: duplicate block hash (size=%zu)", m->size);
    }
    m->size++;
    pthread_rwlock_unlock(&m->rwlock);
    return true;
}

const struct uint256 *block_map_find_hash(const struct block_map *m,
                                           const struct uint256 *hash)
{
    if (m->capacity == 0) return NULL;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&m->rwlock);
    uint64_t h = block_map_hash(hash);
    size_t idx = h & (m->capacity - 1);
    const struct uint256 *result = NULL;
    for (size_t i = 0; i < m->capacity; i++) {
        size_t slot = (idx + i) & (m->capacity - 1);
        if (!m->buckets[slot].occupied)
            break;
        if (uint256_eq(&m->buckets[slot].hash, hash)) {
            result = &m->buckets[slot].hash;
            break;
        }
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&m->rwlock);
    return result;
}

size_t block_map_count(const struct block_map *m)
{
    return m->size;
}

bool block_map_next(const struct block_map *m, size_t *iter,
                    const struct uint256 **hash_out,
                    struct block_index **index_out)
{
    pthread_rwlock_rdlock((pthread_rwlock_t *)&m->rwlock);
    while (*iter < m->capacity) {
        size_t i = (*iter)++;
        if (m->buckets[i].occupied) {
            if (hash_out) *hash_out = &m->buckets[i].hash;
            if (index_out) *index_out = m->buckets[i].index;
            pthread_rwlock_unlock((pthread_rwlock_t *)&m->rwlock);
            return true;
        }
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&m->rwlock);
    return false;
}

/* --- Active Chain --- */

static struct active_chain_authority g_chain_authority = {0};
static struct block_map *g_chain_block_map = NULL;

void active_chain_register_authority(const struct active_chain_authority *auth)
{
    if (auth) g_chain_authority = *auth;
}

void active_chain_register_block_map(struct block_map *m)
{
    g_chain_block_map = m;
}

void active_chain_init(struct active_chain *c)
{
    c->chain = NULL;
    c->height = -1;
    c->capacity = 0;
}

void active_chain_free(struct active_chain *c)
{
    free(c->chain);
    c->chain = NULL;
    c->height = -1;
    c->capacity = 0;
}

struct block_index *active_chain_tip(const struct active_chain *c)
{
    if (!c || !c->chain) return NULL;
    if (g_chain_authority.is_authoritative &&
        g_chain_authority.is_authoritative()) {
        uint8_t h32[32];
        if (g_chain_authority.get_hash && g_chain_authority.get_hash(h32)) {
            struct uint256 hash;
            memcpy(hash.data, h32, 32);
            struct block_index *bi = block_map_find(g_chain_block_map, &hash);
            if (bi) return bi;
        }
    }
    int h = active_chain_height(c);
    if (h < 0) return NULL;
    return c->chain[h];
}

struct block_index *active_chain_at(const struct active_chain *c, int height)
{
    if (!c || !c->chain || height < 0 || height > c->height) return NULL;
    return c->chain[height];
}

bool active_chain_contains(const struct active_chain *c,
                           const struct block_index *bi)
{
    if (bi->nHeight < 0 || bi->nHeight > c->height) return false;
    return c->chain[bi->nHeight] == bi;
}

bool active_chain_set_tip(struct active_chain *c, struct block_index *bi)
{
    if (!bi) {
        c->height = -1;
        return true;
    }

    int new_height = bi->nHeight;
    if (new_height >= c->capacity) {
        int old_cap = c->capacity;
        int new_cap = new_height + 1024;
        struct block_index **nc = zcl_realloc(c->chain,
            (size_t)new_cap * sizeof(struct block_index *), "active_chain");
        if (!nc)
            LOG_FAIL("chainstate", "active_chain_set_tip: realloc failed for height %d", new_height);
        c->chain = nc;
        memset(&c->chain[old_cap], 0,
               (size_t)(new_cap - old_cap) * sizeof(struct block_index *));
        c->capacity = new_cap;
    }

    c->chain[new_height] = bi;
    struct block_index *p = bi->pprev;
    int h = new_height - 1;
    int walk_budget = new_height + 1;
    while (h >= 0) {
        while (p && p->nHeight > h) {
            if (--walk_budget < 0) {
                p = NULL;
                break;
            }
            p = p->pprev;
        }
        struct block_index *slot = (p && p->nHeight == h) ? p : NULL;
        if (h <= c->height && c->chain[h] == slot)
            break;
        c->chain[h] = slot;
        if (p && p->nHeight == h) {
            if (--walk_budget < 0)
                p = NULL;
            else
                p = p->pprev;
        }
        if (!p && slot == NULL && h <= c->height)
            break;
        h--;
    }
    c->height = new_height;

    if (g_chain_authority.is_authoritative &&
        g_chain_authority.set_tip &&
        g_chain_authority.is_authoritative()) {
        const uint8_t *h32 = bi ? bi->phashBlock->data : NULL;
        g_chain_authority.set_tip(new_height, h32);
    }

    return true;
}

int active_chain_height(const struct active_chain *c)
{
    if (!c) return -1;
    if (g_chain_authority.is_authoritative &&
        g_chain_authority.get_height &&
        g_chain_authority.is_authoritative()) {
        int64_t ah = g_chain_authority.get_height();
        if (ah >= 0) return (int)ah;
        return -1;
    }

    sqlite3 *db = progress_store_db();
    if (!db) return c->height;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT MAX(height) FROM tip_finalize_log WHERE ok = 1", -1, &st, NULL) != SQLITE_OK)
        return c->height;
    int h = -1;
    int rc = sqlite3_step(st);  // raw-sql-ok:kernel-primitive
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        h = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (h > c->height) return h;
    return c->height;
}

/* --- Chainstate --- */

void chainstate_init(struct chainstate *cs)
{
    zcl_mutex_init(&cs->cs_main);
    block_map_init(&cs->map_block_index);
    active_chain_init(&cs->chain_active);
    cs->pindex_best_header = NULL;
    cs->f_tx_index = false;
    cs->f_reindex = false;
    cs->f_importing = false;
    cs->f_have_pruned = false;
    cs->f_prune_mode = false;
    cs->n_prune_target = 0;
}

void chainstate_free(struct chainstate *cs)
{
    active_chain_free(&cs->chain_active);
    block_map_free(&cs->map_block_index);
    zcl_mutex_destroy(&cs->cs_main);
}

struct block_index *chainstate_insert_block_index(struct chainstate *cs,
                                                   const struct uint256 *hash)
{
    if (uint256_is_null(hash))
        return NULL;

    struct block_index *existing = block_map_find(&cs->map_block_index, hash);
    if (existing) return existing;

    struct block_index *bi = zcl_calloc(1, sizeof(struct block_index), "block_index");
    if (!bi) return NULL;
    block_index_init(bi);
    block_map_insert(&cs->map_block_index, hash, bi);
    bi->phashBlock = block_map_find_hash(&cs->map_block_index, hash);
    return bi;
}
