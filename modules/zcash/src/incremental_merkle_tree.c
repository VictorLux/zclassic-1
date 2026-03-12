/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Incremental Merkle tree — pure C23 implementation. */

#include "zcash/incremental_merkle_tree.h"
#include "zcash/pedersen_hash.h"
#include "crypto/sha256.h"
#include <assert.h>
#include <string.h>

void sha256_compress_combine(const struct uint256 *a,
                              const struct uint256 *b,
                              size_t depth,
                              struct uint256 *out)
{
    (void)depth;
    struct sha256_ctx hasher;
    sha256_init(&hasher);
    sha256_write(&hasher, a->data, 32);
    sha256_write(&hasher, b->data, 32);
    sha256_finalize_no_padding(&hasher, out->data, 0);
}

void sha256_compress_uncommitted(struct uint256 *out)
{
    memset(out->data, 0, 32);
}

static void tree_init(struct incremental_merkle_tree *t, size_t depth,
                       void (*combine)(const struct uint256 *, const struct uint256 *,
                                       size_t, struct uint256 *),
                       void (*uncommitted)(struct uint256 *))
{
    assert(depth <= MAX_TREE_DEPTH);
    t->depth = depth;
    t->has_left = false;
    t->has_right = false;
    memset(&t->left, 0, sizeof(struct uint256));
    memset(&t->right, 0, sizeof(struct uint256));
    memset(t->has_parent, 0, sizeof(t->has_parent));
    memset(t->parents, 0, sizeof(t->parents));
    t->num_parents = 0;
    t->combine = combine;
    t->uncommitted = uncommitted;
}

void sprout_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, INCREMENTAL_MERKLE_TREE_DEPTH,
              sha256_compress_combine, sha256_compress_uncommitted);
}

static void pedersen_combine(const struct uint256 *a,
                              const struct uint256 *b,
                              size_t depth,
                              struct uint256 *out)
{
    pedersen_merkle_hash(depth, a->data, b->data, out->data);
}

static void pedersen_uncommitted(struct uint256 *out)
{
    sapling_uncommitted(out->data);
}

void sapling_testing_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, INCREMENTAL_MERKLE_TREE_DEPTH_TESTING,
              pedersen_combine, pedersen_uncommitted);
}

void sapling_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
              pedersen_combine, pedersen_uncommitted);
}

/* Compute empty root at given depth by repeatedly combining uncommitted values */
static void empty_root_at_depth(const struct incremental_merkle_tree *t,
                                 size_t depth, struct uint256 *out)
{
    struct uint256 current;
    t->uncommitted(&current);
    for (size_t d = 0; d < depth; d++) {
        struct uint256 next;
        t->combine(&current, &current, d, &next);
        current = next;
    }
    *out = current;
}

void incremental_tree_empty_root(const struct incremental_merkle_tree *t,
                                  struct uint256 *out)
{
    empty_root_at_depth(t, t->depth, out);
}

static void filler_next(const struct incremental_merkle_tree *t,
                         const struct uint256 *filler, size_t *filler_idx,
                         size_t filler_count, size_t depth,
                         struct uint256 *out)
{
    if (*filler_idx < filler_count) {
        *out = filler[*filler_idx];
        (*filler_idx)++;
    } else {
        empty_root_at_depth(t, depth, out);
    }
}

void incremental_tree_append(struct incremental_merkle_tree *t,
                              const struct uint256 *obj)
{
    if (!t->has_left) {
        t->left = *obj;
        t->has_left = true;
    } else if (!t->has_right) {
        t->right = *obj;
        t->has_right = true;
    } else {
        struct uint256 combined;
        t->combine(&t->left, &t->right, 0, &combined);

        t->left = *obj;
        t->has_right = false;

        for (size_t i = 0; i < t->depth; i++) {
            if (i < t->num_parents) {
                if (t->has_parent[i]) {
                    struct uint256 next;
                    t->combine(&t->parents[i], &combined, i + 1, &next);
                    combined = next;
                    t->has_parent[i] = false;
                } else {
                    t->parents[i] = combined;
                    t->has_parent[i] = true;
                    return;
                }
            } else {
                t->parents[i] = combined;
                t->has_parent[i] = true;
                t->num_parents = i + 1;
                return;
            }
        }
    }
}

void incremental_tree_root(const struct incremental_merkle_tree *t,
                            struct uint256 *out)
{
    size_t filler_idx = 0;
    struct uint256 combine_left;
    if (t->has_left) {
        combine_left = t->left;
    } else {
        filler_next(t, NULL, &filler_idx, 0, 0, &combine_left);
    }

    struct uint256 combine_right;
    if (t->has_right) {
        combine_right = t->right;
    } else {
        filler_next(t, NULL, &filler_idx, 0, 0, &combine_right);
    }

    struct uint256 root;
    t->combine(&combine_left, &combine_right, 0, &root);

    size_t d = 1;
    for (size_t i = 0; i < t->num_parents; i++) {
        struct uint256 next;
        if (t->has_parent[i]) {
            t->combine(&t->parents[i], &root, d, &next);
        } else {
            struct uint256 empty;
            empty_root_at_depth(t, d, &empty);
            t->combine(&root, &empty, d, &next);
        }
        root = next;
        d++;
    }

    while (d < t->depth) {
        struct uint256 next;
        struct uint256 empty;
        empty_root_at_depth(t, d, &empty);
        t->combine(&root, &empty, d, &next);
        root = next;
        d++;
    }

    *out = root;
}

size_t incremental_tree_size(const struct incremental_merkle_tree *t)
{
    size_t ret = 0;
    if (t->has_left) ret++;
    if (t->has_right) ret++;
    for (size_t i = 0; i < t->num_parents; i++) {
        if (t->has_parent[i])
            ret += ((size_t)1 << (i + 1));
    }
    return ret;
}

bool incremental_tree_is_complete(const struct incremental_merkle_tree *t)
{
    if (!t->has_left || !t->has_right)
        return false;
    if (t->num_parents != t->depth - 1)
        return false;
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!t->has_parent[i])
            return false;
    }
    return true;
}

/* Wire format: optional<hash> left, optional<hash> right, vector<optional<hash>> parents */
bool incremental_tree_serialize(const struct incremental_merkle_tree *t,
                                 struct byte_stream *s)
{
    /* left: discriminant + hash */
    if (!stream_write_u8(s, t->has_left ? 1 : 0)) return false;
    if (t->has_left && !stream_write(s, t->left.data, 32)) return false;

    /* right: discriminant + hash */
    if (!stream_write_u8(s, t->has_right ? 1 : 0)) return false;
    if (t->has_right && !stream_write(s, t->right.data, 32)) return false;

    /* parents: compact_size + array of optional<hash> */
    if (!stream_write_compact_size(s, t->num_parents)) return false;
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!stream_write_u8(s, t->has_parent[i] ? 1 : 0)) return false;
        if (t->has_parent[i] && !stream_write(s, t->parents[i].data, 32))
            return false;
    }
    return true;
}

static bool wfcheck(const struct incremental_merkle_tree *t)
{
    if (t->num_parents >= t->depth) return false;
    if (t->num_parents > 0 && !t->has_parent[t->num_parents - 1]) return false;
    if (!t->has_left && t->has_right) return false;
    if (!t->has_left && t->num_parents > 0) return false;
    return true;
}

bool incremental_tree_deserialize(struct incremental_merkle_tree *t,
                                   struct byte_stream *s)
{
    uint8_t disc;

    /* left */
    if (!stream_read(s, &disc, 1)) return false;
    t->has_left = (disc != 0);
    if (t->has_left) {
        if (!stream_read(s, t->left.data, 32)) return false;
    } else {
        memset(&t->left, 0, sizeof(struct uint256));
    }

    /* right */
    if (!stream_read(s, &disc, 1)) return false;
    t->has_right = (disc != 0);
    if (t->has_right) {
        if (!stream_read(s, t->right.data, 32)) return false;
    } else {
        memset(&t->right, 0, sizeof(struct uint256));
    }

    /* parents */
    uint64_t num;
    if (!stream_read_compact_size(s, &num)) return false;
    if (num > MAX_TREE_DEPTH) return false;
    t->num_parents = (size_t)num;
    memset(t->has_parent, 0, sizeof(t->has_parent));
    memset(t->parents, 0, sizeof(t->parents));
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!stream_read(s, &disc, 1)) return false;
        t->has_parent[i] = (disc != 0);
        if (t->has_parent[i]) {
            if (!stream_read(s, t->parents[i].data, 32)) return false;
        }
    }

    return wfcheck(t);
}

/* --- Incremental Witness --- */

static size_t next_depth(const struct incremental_merkle_tree *t, size_t skip)
{
    size_t d = 0;
    size_t s = skip;
    if (!t->has_right) {
        if (s == 0) return 0;
        s--;
    }
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!t->has_parent[i]) {
            if (s == 0) return d + 1;
            s--;
        }
        d++;
    }
    /* Above all existing parents */
    return d + 1 + s;
}

void incremental_witness_init(struct incremental_witness *w,
                               const struct incremental_merkle_tree *tree)
{
    w->tree = *tree;
    w->num_filled = 0;
    w->has_cursor = false;
    w->cursor_depth = next_depth(tree, 0);
}

void incremental_witness_append(struct incremental_witness *w,
                                 const struct uint256 *obj)
{
    if (w->has_cursor) {
        incremental_tree_append(&w->cursor, obj);
        if (incremental_tree_is_complete(&w->cursor)) {
            struct uint256 root;
            incremental_tree_root(&w->cursor, &root);
            w->filled[w->num_filled++] = root;
            w->has_cursor = false;
            w->cursor_depth = next_depth(&w->tree, w->num_filled);
        }
    } else {
        w->cursor_depth = next_depth(&w->tree, w->num_filled);
        if (w->cursor_depth == 0) {
            w->filled[w->num_filled++] = *obj;
            w->cursor_depth = next_depth(&w->tree, w->num_filled);
        } else {
            /* Initialize cursor subtree at cursor_depth */
            tree_init(&w->cursor, w->cursor_depth,
                      w->tree.combine, w->tree.uncommitted);
            incremental_tree_append(&w->cursor, obj);
            w->has_cursor = true;
        }
    }
}

void incremental_witness_root(const struct incremental_witness *w,
                               struct uint256 *out)
{
    /* Partial fill: combine tree's root computation with filled + cursor */
    const struct incremental_merkle_tree *t = &w->tree;

    struct uint256 combine_left;
    if (t->has_left) {
        combine_left = t->left;
    } else {
        t->uncommitted(&combine_left);
    }

    struct uint256 combine_right;
    if (t->has_right) {
        combine_right = t->right;
    } else {
        /* Use first filled or uncommitted */
        if (w->num_filled > 0 || w->has_cursor) {
            size_t fi = 0;
            if (fi < w->num_filled) {
                combine_right = w->filled[fi];
                fi++;
            } else {
                t->uncommitted(&combine_right);
            }
        } else {
            t->uncommitted(&combine_right);
        }
    }

    struct uint256 root;
    t->combine(&combine_left, &combine_right, 0, &root);

    size_t d = 1;
    size_t filled_idx = t->has_right ? 0 : (w->num_filled > 0 ? 1 : 0);

    for (size_t i = 0; i < t->num_parents || d < t->depth; i++) {
        struct uint256 next_val;
        if (i < t->num_parents && t->has_parent[i]) {
            t->combine(&t->parents[i], &root, d, &next_val);
        } else {
            struct uint256 filler;
            if (filled_idx < w->num_filled) {
                filler = w->filled[filled_idx++];
            } else if (w->has_cursor && filled_idx == w->num_filled) {
                incremental_tree_root(&w->cursor, &filler);
                filled_idx++;
            } else {
                empty_root_at_depth(t, d, &filler);
            }
            t->combine(&root, &filler, d, &next_val);
        }
        root = next_val;
        d++;
        if (d >= t->depth) break;
    }

    *out = root;
}

bool incremental_witness_serialize(const struct incremental_witness *w,
                                    struct byte_stream *s)
{
    if (!incremental_tree_serialize(&w->tree, s)) return false;

    /* filled: vector<hash> */
    if (!stream_write_compact_size(s, w->num_filled)) return false;
    for (size_t i = 0; i < w->num_filled; i++) {
        if (!stream_write(s, w->filled[i].data, 32)) return false;
    }

    /* cursor: optional<tree> */
    if (!stream_write_u8(s, w->has_cursor ? 1 : 0)) return false;
    if (w->has_cursor) {
        if (!incremental_tree_serialize(&w->cursor, s)) return false;
    }

    return true;
}

bool incremental_witness_deserialize(struct incremental_witness *w,
                                      struct byte_stream *s,
                                      size_t depth,
                                      void (*combine)(const struct uint256 *,
                                                      const struct uint256 *,
                                                      size_t, struct uint256 *),
                                      void (*uncommitted)(struct uint256 *))
{
    /* Initialize function pointers first */
    w->tree.depth = depth;
    w->tree.combine = combine;
    w->tree.uncommitted = uncommitted;

    if (!incremental_tree_deserialize(&w->tree, s)) return false;

    /* filled */
    uint64_t num;
    if (!stream_read_compact_size(s, &num)) return false;
    if (num > MAX_TREE_DEPTH) return false;
    w->num_filled = (size_t)num;
    for (size_t i = 0; i < w->num_filled; i++) {
        if (!stream_read(s, w->filled[i].data, 32)) return false;
    }

    /* cursor */
    uint8_t disc;
    if (!stream_read(s, &disc, 1)) return false;
    w->has_cursor = (disc != 0);
    if (w->has_cursor) {
        w->cursor.depth = depth;
        w->cursor.combine = combine;
        w->cursor.uncommitted = uncommitted;
        if (!incremental_tree_deserialize(&w->cursor, s)) return false;
    }

    w->cursor_depth = next_depth(&w->tree, w->num_filled);
    return true;
}
