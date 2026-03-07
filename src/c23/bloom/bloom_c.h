/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_BLOOM_C_H
#define ZCL_BLOOM_C_H

#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_BLOOM_FILTER_SIZE 36000
#define MAX_BLOOM_HASH_FUNCS 50

enum bloom_flags {
    BLOOM_UPDATE_NONE = 0,
    BLOOM_UPDATE_ALL = 1,
    BLOOM_UPDATE_P2PUBKEY_ONLY = 2,
    BLOOM_UPDATE_MASK = 3
};

struct bloom_filter {
    unsigned char *data;
    size_t data_size;
    bool is_full;
    bool is_empty;
    unsigned int num_hash_funcs;
    unsigned int tweak;
    unsigned char flags;
};

bool bloom_filter_init(struct bloom_filter *f, unsigned int num_elements,
                       double fp_rate, unsigned int tweak, unsigned char flags);
void bloom_filter_free(struct bloom_filter *f);
void bloom_filter_insert(struct bloom_filter *f, const unsigned char *data, size_t len);
bool bloom_filter_contains(const struct bloom_filter *f, const unsigned char *data, size_t len);
void bloom_filter_insert_uint256(struct bloom_filter *f, const struct uint256 *hash);
bool bloom_filter_contains_uint256(const struct bloom_filter *f, const struct uint256 *hash);
void bloom_filter_clear(struct bloom_filter *f);
void bloom_filter_reset(struct bloom_filter *f, unsigned int new_tweak);
bool bloom_filter_is_within_size_constraints(const struct bloom_filter *f);
void bloom_filter_update_empty_full(struct bloom_filter *f);

struct rolling_bloom_filter {
    struct bloom_filter b1;
    struct bloom_filter b2;
    unsigned int bloom_size;
    unsigned int insertions;
};

bool rolling_bloom_init(struct rolling_bloom_filter *f, unsigned int num_elements, double fp_rate);
void rolling_bloom_free(struct rolling_bloom_filter *f);
void rolling_bloom_insert(struct rolling_bloom_filter *f, const unsigned char *data, size_t len);
bool rolling_bloom_contains(const struct rolling_bloom_filter *f, const unsigned char *data, size_t len);
void rolling_bloom_reset(struct rolling_bloom_filter *f);

#endif
