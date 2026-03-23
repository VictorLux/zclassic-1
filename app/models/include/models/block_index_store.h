/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BlockIndexStore — thin wrapper over leveldb_store for block index. */

#ifndef ZCL_MODELS_BLOCK_INDEX_STORE_H
#define ZCL_MODELS_BLOCK_INDEX_STORE_H

#include "models/leveldb_store.h"

typedef struct leveldb_store block_index_store;

static inline bool block_index_store_validate(block_index_store *idx,
                                               struct ar_errors *errors)
{
    return leveldb_store_validate(idx, errors);
}

static inline bool block_index_store_save(block_index_store *idx)
{
    if (!idx->label) idx->label = "block_index_store";
    return leveldb_store_save(idx);
}

static inline void block_index_store_summary(const block_index_store *idx,
                                              char *out, size_t len)
{
    leveldb_store_summary(idx, out, len);
}

#endif
