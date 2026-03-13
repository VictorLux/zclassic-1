/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/block_index_db.h"
#include "core/hash.h"
#include "primitives/block.h"
#include <stdlib.h>
#include <string.h>

static const char DB_BLOCK_INDEX = 'b';

bool disk_block_index_serialize(const struct disk_block_index *d,
                                struct byte_stream *s)
{
    if (!stream_write_varint(s, (uint64_t)d->nVersion)) return false;
    if (!stream_write_varint(s, (uint64_t)d->nHeight)) return false;
    if (!stream_write_varint(s, (uint64_t)d->nStatus)) return false;
    if (!stream_write_varint(s, (uint64_t)d->nTx)) return false;
    if (d->nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO))
        if (!stream_write_varint(s, (uint64_t)d->nFile)) return false;
    if (d->nStatus & BLOCK_HAVE_DATA)
        if (!stream_write_varint(s, (uint64_t)d->nDataPos)) return false;
    if (d->nStatus & BLOCK_HAVE_UNDO)
        if (!stream_write_varint(s, (uint64_t)d->nUndoPos)) return false;
    if (d->nStatus & BLOCK_ACTIVATES_UPGRADE) {
        uint32_t branchId = (uint32_t)d->nCachedBranchId;
        if (!stream_write_u32_le(s, branchId)) return false;
    }
    if (!stream_write_bytes(s, d->hashSproutAnchor.data, 32)) return false;

    if (!stream_write_i32_le(s, d->nVersion)) return false;
    if (!stream_write_bytes(s, d->hashPrev.data, 32)) return false;
    if (!stream_write_bytes(s, d->hashMerkleRoot.data, 32)) return false;
    if (!stream_write_bytes(s, d->hashFinalSaplingRoot.data, 32)) return false;
    if (!stream_write_u32_le(s, d->nTime)) return false;
    if (!stream_write_u32_le(s, d->nBits)) return false;
    if (!stream_write_bytes(s, d->nNonce.data, 32)) return false;
    if (!stream_write_compact_size(s, d->nSolutionSize)) return false;
    if (d->nSolutionSize > 0)
        if (!stream_write_bytes(s, d->nSolution, d->nSolutionSize)) return false;

    if (d->has_sprout_value)
        if (!stream_write_i64_le(s, d->nSproutValue)) return false;
    if (!stream_write_i64_le(s, d->nSaplingValue)) return false;

    return true;
}

bool disk_block_index_deserialize(struct disk_block_index *d,
                                  struct byte_stream *s)
{
    uint64_t v;
    if (!stream_read_varint(s, &v)) return false;
    int stored_version = (int)v;

    if (!stream_read_varint(s, &v)) return false;
    d->nHeight = (int)v;
    if (!stream_read_varint(s, &v)) return false;
    d->nStatus = (unsigned int)v;
    if (!stream_read_varint(s, &v)) return false;
    d->nTx = (unsigned int)v;
    if (d->nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO)) {
        if (!stream_read_varint(s, &v)) return false;
        d->nFile = (int)v;
    }
    if (d->nStatus & BLOCK_HAVE_DATA) {
        if (!stream_read_varint(s, &v)) return false;
        d->nDataPos = (unsigned int)v;
    }
    if (d->nStatus & BLOCK_HAVE_UNDO) {
        if (!stream_read_varint(s, &v)) return false;
        d->nUndoPos = (unsigned int)v;
    }
    if (d->nStatus & BLOCK_ACTIVATES_UPGRADE) {
        uint32_t branchId;
        if (!stream_read_u32_le(s, &branchId)) return false;
        d->nCachedBranchId = (int64_t)branchId;
    }
    if (!stream_read_bytes(s, d->hashSproutAnchor.data, 32)) return false;

    if (!stream_read_i32_le(s, &d->nVersion)) return false;
    if (!stream_read_bytes(s, d->hashPrev.data, 32)) return false;
    if (!stream_read_bytes(s, d->hashMerkleRoot.data, 32)) return false;
    if (!stream_read_bytes(s, d->hashFinalSaplingRoot.data, 32)) return false;
    if (!stream_read_u32_le(s, &d->nTime)) return false;
    if (!stream_read_u32_le(s, &d->nBits)) return false;
    if (!stream_read_bytes(s, d->nNonce.data, 32)) return false;
    uint64_t sol_size;
    if (!stream_read_compact_size(s, &sol_size)) return false;
    if (sol_size > MAX_SOLUTION_SIZE) return false;
    d->nSolutionSize = (size_t)sol_size;
    if (d->nSolutionSize > 0)
        if (!stream_read_bytes(s, d->nSolution, d->nSolutionSize)) return false;

    d->has_sprout_value = (stored_version >= SPROUT_VALUE_VERSION);
    if (d->has_sprout_value) {
        if (!stream_read_i64_le(s, &d->nSproutValue)) return false;
    }
    if (stored_version >= SAPLING_VALUE_VERSION) {
        if (!stream_read_i64_le(s, &d->nSaplingValue)) return false;
    }

    return true;
}

void disk_block_index_get_hash(const struct disk_block_index *d,
                               struct uint256 *out)
{
    struct block_header h;
    block_header_init(&h);
    h.nVersion = d->nVersion;
    h.hashPrevBlock = d->hashPrev;
    h.hashMerkleRoot = d->hashMerkleRoot;
    h.hashFinalSaplingRoot = d->hashFinalSaplingRoot;
    h.nTime = d->nTime;
    h.nBits = d->nBits;
    h.nNonce = d->nNonce;
    memcpy(h.nSolution, d->nSolution, d->nSolutionSize);
    h.nSolutionSize = d->nSolutionSize;
    block_header_get_hash(&h, out);
}

bool block_tree_db_write_block_index(struct block_tree_db *btdb,
                                     const struct disk_block_index *d)
{
    struct uint256 hash;
    disk_block_index_get_hash(d, &hash);

    char key[64];
    key[0] = DB_BLOCK_INDEX;
    memcpy(key + 1, hash.data, 32);
    size_t keylen = 33;

    struct byte_stream s;
    stream_init(&s, 512);
    if (!disk_block_index_serialize(d, &s)) {
        stream_free(&s);
        return false;
    }

    bool ok = db_write(&btdb->db, key, keylen, (char *)s.data, s.size, false);
    stream_free(&s);
    return ok;
}

bool block_tree_db_load_block_index_guts(struct block_tree_db *btdb,
                                         insert_block_index_fn insert_fn,
                                         void *ctx)
{
    char seek_key[33];
    seek_key[0] = DB_BLOCK_INDEX;
    memset(seek_key + 1, 0, 32);

    struct db_iterator it;
    db_iter_init(&it, &btdb->db);
    db_iter_seek(&it, seek_key, 33);

    while (db_iter_valid(&it)) {
        size_t key_len;
        const char *key_data = db_iter_key(&it, &key_len);

        if (key_len < 1 || key_data[0] != DB_BLOCK_INDEX)
            break;

        size_t val_len;
        const char *val_data = db_iter_value(&it, &val_len);

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        struct byte_stream s;
        stream_init_from_data(&s, (unsigned char *)val_data, val_len);

        if (!disk_block_index_deserialize(&dbi, &s)) {
            stream_free(&s);
            db_iter_free(&it);
            return false;
        }
        stream_free(&s);

        /* Use hash from LevelDB key (bytes 1..32) to avoid recomputing
         * double-SHA256 of the full block header + equihash solution.
         * This cuts block index load from minutes to seconds. */
        struct uint256 block_hash;
        if (key_len >= 33)
            memcpy(block_hash.data, key_data + 1, 32);
        else
            disk_block_index_get_hash(&dbi, &block_hash);

        struct block_index *pindex = insert_fn(ctx, &block_hash);
        if (!pindex) {
            db_iter_free(&it);
            return false;
        }

        struct uint256 prev_hash = dbi.hashPrev;
        struct block_index *pprev = insert_fn(ctx, &prev_hash);

        pindex->pprev = pprev;
        pindex->nHeight = dbi.nHeight;
        pindex->nFile = dbi.nFile;
        pindex->nDataPos = dbi.nDataPos;
        pindex->nUndoPos = dbi.nUndoPos;
        pindex->hashSproutAnchor = dbi.hashSproutAnchor;
        pindex->nVersion = dbi.nVersion;
        pindex->hashMerkleRoot = dbi.hashMerkleRoot;
        pindex->hashFinalSaplingRoot = dbi.hashFinalSaplingRoot;
        pindex->nTime = dbi.nTime;
        pindex->nBits = dbi.nBits;
        pindex->nNonce = dbi.nNonce;
        memcpy(pindex->nSolution, dbi.nSolution, dbi.nSolutionSize);
        pindex->nSolutionSize = dbi.nSolutionSize;
        pindex->nStatus = dbi.nStatus;
        pindex->nCachedBranchId = dbi.nCachedBranchId;
        pindex->nTx = dbi.nTx;
        if (dbi.has_sprout_value) {
            pindex->nSproutValue = dbi.nSproutValue;
            pindex->has_sprout_value = true;
        }
        pindex->nSaplingValue = dbi.nSaplingValue;

        db_iter_next(&it);
    }

    db_iter_free(&it);
    return true;
}
