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

struct block_index *active_chain_cached_tip(const struct active_chain *c)
{
    if (!c || !c->chain || c->height < 0 || c->height >= c->capacity)
        return NULL;
    return c->chain[c->height];
}

struct block_index *active_chain_tip(const struct active_chain *c)
{
    if (!c || !c->chain) return NULL;
    if (g_chain_authority.is_authoritative &&
        g_chain_authority.is_authoritative()) {
        uint8_t h32[32];
        if (g_chain_authority.get_hash && g_chain_block_map &&
            g_chain_authority.get_hash(h32)) {
            struct uint256 hash;
            memcpy(hash.data, h32, 32);
            struct block_index *bi = block_map_find(g_chain_block_map, &hash);
            if (bi) return bi;
        }
    }
    int h = active_chain_height(c);
    if (h < 0 || h >= c->capacity) return NULL;
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

/* Physically assemble chain[] so chain[bi->nHeight] == bi and every lower
 * slot holds the ancestor on bi's path (walking pprev), then set c->height
 * = bi->nHeight. This is the structural half of the active-chain window move;
 * the part every reader (active_chain_at, the stages) depends on. It never
 * publishes the authoritative tip; reducer/repair callers must do that
 * through their explicit authority API after their durable write succeeds.
 * Returns false only on a realloc failure (which LOG_FAILs and never
 * returns). Slots ABOVE new_height are left untouched (a later wider window
 * can still re-expose them). */
static bool active_chain_fill_window(struct active_chain *c,
                                     struct block_index *bi)
{
    int new_height = bi->nHeight;
    if (new_height >= c->capacity) {
        int old_cap = c->capacity;
        int new_cap = new_height + 1024;
        struct block_index **nc = zcl_realloc(c->chain,
            (size_t)new_cap * sizeof(struct block_index *), "active_chain");
        if (!nc)
            LOG_FAIL("chainstate", "active_chain_fill_window: realloc failed for height %d", new_height);
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
        if (!slot && h <= c->height)
            break; /* preserve the finalized lower window across pprev gaps */
        if (h <= c->height && c->chain[h] == slot)
            break;
        c->chain[h] = slot;
        if (p && p->nHeight == h) {
            if (--walk_budget < 0)
                p = NULL;
            else
                p = p->pprev;
        }
        h--;
    }
    c->height = new_height;
    return true;
}

bool active_chain_move_window_tip(struct active_chain *c, struct block_index *bi)
{
    if (!bi) {
        c->height = -1;
        return true;
    }

    if (!active_chain_fill_window(c, bi))
        return false;

    return true;
}

/* Extend the VISIBLE chain[] window forward to a most-work CANDIDATE that
 * builds on the current tip, WITHOUT moving the authoritative (finalized)
 * tip. This is the reducer's structural analogue of assembling chain[] out to
 * find_most_work_chain's candidate before a block-by-block validation pass.
 *
 * It exists because the reducer's tip_finalize uses a one-block lookahead:
 * it finalizes height H by reading active_chain_at(H+1), then collapses
 * c->height back to H via active_chain_move_window_tip(new_tip). Without a
 * separate window-extender, the lookahead for H+1 would never find H+2 and the
 * reducer would wedge after one block. Re-extending to the candidate each
 * tick keeps the lookahead supplied while the finalized tip advances under
 * it.
 *
 * Contract:
 *   - `candidate` MUST descend from (or equal) the current tip — the caller
 *     (a most-work selector that refuses below-tip candidates) guarantees
 *     this; we only assemble the path it names.
 *   - NEVER shrinks the window: if candidate->nHeight <= c->height this is a
 *     no-op (the finalized tip is the authority on retreat; window growth is
 *     monotonic forward between finalizations).
 *   - Does NOT publish authority — the finalized tip is owned by
 *     tip_finalize's explicit authority publication. This only widens what
 *     active_chain_at can see.
 * Returns true on success (incl. the no-op case), false only on realloc
 * failure (which LOG_FAILs upstream). */
bool active_chain_extend_window(struct active_chain *c,
                                struct block_index *candidate)
{
    if (!c || !candidate)
        return true;
    if (candidate->nHeight <= c->height)
        return true; /* monotonic forward only — never retreat the window */
    return active_chain_fill_window(c, candidate);
}

struct block_index *active_chain_most_work_candidate(struct active_chain *c,
                                                      struct block_map *m)
{
    if (!c || !m)
        return NULL;

    struct block_index *tip = active_chain_tip(c);
    struct block_index *best = tip;

    size_t iter = 0;
    struct block_index *pindex = NULL;
    while (block_map_next(m, &iter, NULL, &pindex)) {
        if (!pindex)
            continue;
        /* Mirror find_most_work_chain (process_block_core.c) eligibility:
         * skip failed blocks, require at least header-tree validation, and
         * require data availability (nChainTx>0 OR BLOCK_HAVE_DATA). */
        if (block_has_any_failure(pindex))
            continue;
        if (!block_index_is_valid(pindex, BLOCK_VALID_TREE))
            continue;
        if (pindex->nChainTx == 0 && !(pindex->nStatus & BLOCK_HAVE_DATA))
            continue;

        if (!best || arith_uint256_compare(&pindex->nChainWork,
                                           &best->nChainWork) > 0) {
            /* Ancestry must be failure-free up to the current best/tip. */
            bool chain_ok = true;
            struct block_index *check = pindex;
            int floor_h = best ? best->nHeight : -1;
            while (check && check->nHeight > floor_h) {
                if (block_has_any_failure(check)) {
                    chain_ok = false;
                    break;
                }
                if (!check->pprev && check->nHeight > 0)
                    break; /* pprev unlinked (post-import) — stop, accept */
                check = check->pprev;
            }
            if (chain_ok)
                best = pindex;
        }
    }

    /* Refuse a candidate strictly BELOW the current tip — the tip is
     * canonical (matches find_most_work_chain's stale-fork guard). The
     * window only ever grows forward; a lower-height higher-work fork is
     * never selected here. */
    if (tip && best && best != tip && best->nHeight < tip->nHeight)
        return tip;

    return best;
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
