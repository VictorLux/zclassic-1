/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Incremental Merkle tree for Sprout (SHA256Compress) and Sapling (Pedersen). */

#ifndef ZCL_ZCASH_INCREMENTAL_MERKLE_TREE_H
#define ZCL_ZCASH_INCREMENTAL_MERKLE_TREE_H

#include "core/uint256.h"
#include "core/serialize.h"
#include "zcash/zcash.h"
#include <stdbool.h>
#include <stddef.h>

/* Hash combine functions */
void sha256_compress_combine(const struct uint256 *a,
                              const struct uint256 *b,
                              size_t depth,
                              struct uint256 *out);

void sha256_compress_uncommitted(struct uint256 *out);

/* Incremental Merkle Tree — runtime-parameterized depth.
 * Each node in parents[] is either present or absent (optional). */
#define MAX_TREE_DEPTH 32

struct incremental_merkle_tree {
    size_t depth;
    bool has_left;
    struct uint256 left;
    bool has_right;
    struct uint256 right;
    bool has_parent[MAX_TREE_DEPTH];
    struct uint256 parents[MAX_TREE_DEPTH];
    size_t num_parents;

    /* Function pointers for hash operations (SHA256 or Pedersen) */
    void (*combine)(const struct uint256 *a, const struct uint256 *b,
                    size_t depth, struct uint256 *out);
    void (*uncommitted)(struct uint256 *out);
};

void sprout_tree_init(struct incremental_merkle_tree *t);
void sapling_tree_init(struct incremental_merkle_tree *t);

void incremental_tree_append(struct incremental_merkle_tree *t,
                              const struct uint256 *obj);

void incremental_tree_root(const struct incremental_merkle_tree *t,
                            struct uint256 *out);

size_t incremental_tree_size(const struct incremental_merkle_tree *t);

bool incremental_tree_is_complete(const struct incremental_merkle_tree *t);

void incremental_tree_empty_root(const struct incremental_merkle_tree *t,
                                  struct uint256 *out);

/* Serialization (wire-compatible with C++ boost::optional encoding) */
bool incremental_tree_serialize(const struct incremental_merkle_tree *t,
                                 struct byte_stream *s);
bool incremental_tree_deserialize(struct incremental_merkle_tree *t,
                                   struct byte_stream *s);

/* Incremental witness — tracks a path to a specific leaf */
struct incremental_witness {
    struct incremental_merkle_tree tree;
    struct uint256 filled[MAX_TREE_DEPTH];
    size_t num_filled;
    bool has_cursor;
    struct incremental_merkle_tree cursor;
    size_t cursor_depth;
};

void incremental_witness_init(struct incremental_witness *w,
                               const struct incremental_merkle_tree *tree);

void incremental_witness_append(struct incremental_witness *w,
                                 const struct uint256 *obj);

void incremental_witness_root(const struct incremental_witness *w,
                               struct uint256 *out);

bool incremental_witness_serialize(const struct incremental_witness *w,
                                    struct byte_stream *s);
bool incremental_witness_deserialize(struct incremental_witness *w,
                                      struct byte_stream *s,
                                      size_t depth,
                                      void (*combine)(const struct uint256 *,
                                                      const struct uint256 *,
                                                      size_t, struct uint256 *),
                                      void (*uncommitted)(struct uint256 *));

#endif
