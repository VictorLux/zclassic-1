/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_SCRIPT_SIGCACHE_C_H
#define ZCL_SCRIPT_SIGCACHE_C_H

#include "core/uint256.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DEFAULT_MAX_SIG_CACHE_SIZE 32
#define SIG_CACHE_MAX_ENTRIES 65536

struct sig_cache {
    struct uint256 nonce;
    struct uint256 entries[SIG_CACHE_MAX_ENTRIES];
    bool occupied[SIG_CACHE_MAX_ENTRIES];
    size_t count;
    zcl_mutex_t mutex;
};

void sig_cache_init(struct sig_cache *cache);
void sig_cache_destroy(struct sig_cache *cache);

void sig_cache_compute_entry(const struct sig_cache *cache,
                             struct uint256 *entry,
                             const struct uint256 *hash,
                             const unsigned char *sig, size_t siglen,
                             const unsigned char *pubkey, size_t pklen);

bool sig_cache_get(struct sig_cache *cache, const struct uint256 *entry);
void sig_cache_set(struct sig_cache *cache, const struct uint256 *entry);
void sig_cache_erase(struct sig_cache *cache, const struct uint256 *entry);

struct sig_cache *sig_cache_instance(void);

#endif
