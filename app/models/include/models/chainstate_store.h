/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ChainstateStore — thin wrapper over leveldb_store for chainstate. */

#ifndef ZCL_MODELS_CHAINSTATE_STORE_H
#define ZCL_MODELS_CHAINSTATE_STORE_H

#include "models/leveldb_store.h"

typedef struct leveldb_store chainstate_store;

static inline bool chainstate_store_validate(chainstate_store *cs,
                                              struct ar_errors *errors)
{
    return leveldb_store_validate(cs, errors);
}

static inline bool chainstate_store_save(chainstate_store *cs)
{
    if (!cs->label) cs->label = "chainstate_store";
    return leveldb_store_save(cs);
}

static inline void chainstate_store_summary(const chainstate_store *cs,
                                             char *out, size_t len)
{
    leveldb_store_summary(cs, out, len);
}

#endif
