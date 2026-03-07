/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2013 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_PRIMITIVES_BLOCK_C_H
#define ZCL_PRIMITIVES_BLOCK_C_H

#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_HEADER_SIZE (4 + 32 + 32 + 32 + 4 + 4 + 32)
#define MAX_SOLUTION_SIZE 1344

struct block_header {
    int32_t nVersion;
    struct uint256 hashPrevBlock;
    struct uint256 hashMerkleRoot;
    struct uint256 hashFinalSaplingRoot;
    uint32_t nTime;
    uint32_t nBits;
    struct uint256 nNonce;
    unsigned char nSolution[MAX_SOLUTION_SIZE];
    size_t nSolutionSize;
};

static inline void block_header_init(struct block_header *h)
{
    memset(h, 0, sizeof(*h));
    h->nVersion = 4;
}

static inline bool block_header_is_null(const struct block_header *h)
{
    return h->nBits == 0;
}

static inline int64_t block_header_get_time(const struct block_header *h)
{
    return (int64_t)h->nTime;
}

struct byte_stream;

bool block_header_serialize(const struct block_header *h, struct byte_stream *s);
bool block_header_deserialize(struct block_header *h, struct byte_stream *s);
void block_header_get_hash(const struct block_header *h, struct uint256 *out);

#define MAX_LOCATOR_HASHES 64

struct block_locator {
    struct uint256 *vhave;
    size_t num_hashes;
};

static inline void block_locator_init(struct block_locator *loc)
{
    loc->vhave = NULL;
    loc->num_hashes = 0;
}

static inline void block_locator_free(struct block_locator *loc)
{
    free(loc->vhave);
    loc->vhave = NULL;
    loc->num_hashes = 0;
}

bool block_locator_serialize(const struct block_locator *loc,
                             struct byte_stream *s);
bool block_locator_deserialize(struct block_locator *loc,
                               struct byte_stream *s);

#endif
