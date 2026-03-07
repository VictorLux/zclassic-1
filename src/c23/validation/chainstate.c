/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/chainstate.h"
#include <stdlib.h>
#include <string.h>

/* --- Block Map --- */

void block_map_init(struct block_map *m)
{
    m->entries = NULL;
    m->size = 0;
    m->capacity = 0;
}

void block_map_free(struct block_map *m)
{
    for (size_t i = 0; i < m->size; i++)
        free(m->entries[i].index);
    free(m->entries);
    m->entries = NULL;
    m->size = 0;
    m->capacity = 0;
}

struct block_index *block_map_find(const struct block_map *m,
                                    const struct uint256 *hash)
{
    for (size_t i = 0; i < m->size; i++)
        if (uint256_eq(&m->entries[i].hash, hash))
            return m->entries[i].index;
    return NULL;
}

bool block_map_insert(struct block_map *m, const struct uint256 *hash,
                      struct block_index *index)
{
    if (block_map_find(m, hash))
        return false;
    if (m->size >= m->capacity) {
        size_t new_cap = m->capacity ? m->capacity * 2 : 256;
        struct block_map_entry *ne = realloc(m->entries,
            new_cap * sizeof(struct block_map_entry));
        if (!ne) return false;
        m->entries = ne;
        m->capacity = new_cap;
    }
    m->entries[m->size].hash = *hash;
    m->entries[m->size].index = index;
    m->size++;
    return true;
}

size_t block_map_count(const struct block_map *m)
{
    return m->size;
}

/* --- Active Chain --- */

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
    if (c->height < 0) return NULL;
    return c->chain[c->height];
}

struct block_index *active_chain_at(const struct active_chain *c, int height)
{
    if (height < 0 || height > c->height) return NULL;
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
        int new_cap = new_height + 1024;
        struct block_index **nc = realloc(c->chain,
            (size_t)new_cap * sizeof(struct block_index *));
        if (!nc) return false;
        c->chain = nc;
        c->capacity = new_cap;
    }

    c->chain[new_height] = bi;
    struct block_index *p = bi->pprev;
    int h = new_height - 1;
    while (h >= 0 && (h > c->height || c->chain[h] != p)) {
        c->chain[h] = p;
        if (p) p = p->pprev;
        h--;
    }
    c->height = new_height;
    return true;
}

int active_chain_height(const struct active_chain *c)
{
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

    struct block_index *bi = calloc(1, sizeof(struct block_index));
    if (!bi) return NULL;
    block_index_init(bi);
    block_map_insert(&cs->map_block_index, hash, bi);
    return bi;
}
