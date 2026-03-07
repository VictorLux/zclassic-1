/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/coins_db.h"
#include "coins/undo.h"
#include "core/serialize.h"
#include <stdlib.h>
#include <string.h>

static const char DB_COINS = 'c';
static const char DB_BEST_BLOCK = 'B';

static void make_coins_key(char *buf, size_t *len,
                            const struct uint256 *txid)
{
    buf[0] = DB_COINS;
    memcpy(buf + 1, txid->data, 32);
    *len = 33;
}

static bool cvdb_get_coins_impl(void *self, const struct uint256 *txid,
                                struct coins *out)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_get_coins(cvdb, txid, out);
}

static bool cvdb_have_coins_impl(void *self, const struct uint256 *txid)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_have_coins(cvdb, txid);
}

static bool cvdb_get_best_block_impl(void *self, struct uint256 *hash)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_get_best_block(cvdb, hash);
}

static bool cvdb_batch_write_impl(void *self, struct coins_map *map_coins,
                                   const struct uint256 *hash_block)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_batch_write(cvdb, map_coins, hash_block);
}

static struct coins_view_vtable cvdb_vtable = {
    .get_coins = cvdb_get_coins_impl,
    .have_coins = cvdb_have_coins_impl,
    .get_best_block = cvdb_get_best_block_impl,
    .batch_write = cvdb_batch_write_impl,
    .get_stats = NULL,
};

bool coins_view_db_open(struct coins_view_db *cvdb, const char *path,
                        size_t cache_size, bool memory, bool wipe)
{
    if (!db_wrapper_open(&cvdb->db, path, cache_size, memory, wipe))
        return false;
    cvdb->view.vtable = &cvdb_vtable;
    cvdb->view.impl = cvdb;
    return true;
}

void coins_view_db_close(struct coins_view_db *cvdb)
{
    db_wrapper_close(&cvdb->db);
}

bool coins_view_db_get_coins(struct coins_view_db *cvdb,
                             const struct uint256 *txid,
                             struct coins *out)
{
    char key[64];
    size_t keylen;
    make_coins_key(key, &keylen, txid);

    char *val = NULL;
    size_t vallen = 0;
    if (!db_read(&cvdb->db, key, keylen, &val, &vallen))
        return false;

    struct byte_stream s;
    stream_init_from_data(&s, (unsigned char *)val, vallen);

    uint64_t nVersion = 0;
    stream_read_varint(&s, &nVersion);
    out->version = (int)nVersion;

    uint64_t nCode = 0;
    stream_read_varint(&s, &nCode);
    out->is_coinbase = (nCode & 1) != 0;
    bool vout0_present = (nCode & 2) != 0;
    bool vout1_present = (nCode & 4) != 0;
    unsigned int nMaskCode = (unsigned int)(nCode / 8) + ((!vout0_present && !vout1_present) ? 0 : 1);

    size_t num_outputs = 2;
    if (nMaskCode > 0)
        num_outputs += nMaskCode * 8;
    coins_alloc(out, num_outputs);

    if (vout0_present)
        compressed_txout_deserialize(&out->vout[0], &s);
    if (vout1_present)
        compressed_txout_deserialize(&out->vout[1], &s);

    uint64_t h = 0;
    stream_read_varint(&s, &h);
    out->height = (int)h;

    stream_free(&s);
    free(val);
    coins_cleanup(out);
    return true;
}

bool coins_view_db_have_coins(struct coins_view_db *cvdb,
                              const struct uint256 *txid)
{
    char key[64];
    size_t keylen;
    make_coins_key(key, &keylen, txid);
    return db_exists(&cvdb->db, key, keylen);
}

bool coins_view_db_get_best_block(struct coins_view_db *cvdb,
                                  struct uint256 *hash)
{
    char key = DB_BEST_BLOCK;
    char *val = NULL;
    size_t vallen = 0;
    if (!db_read(&cvdb->db, &key, 1, &val, &vallen))
        return false;
    if (vallen >= 32)
        memcpy(hash->data, val, 32);
    else
        uint256_set_null(hash);
    free(val);
    return true;
}

bool coins_view_db_batch_write(struct coins_view_db *cvdb,
                               struct coins_map *map_coins,
                               const struct uint256 *hash_block)
{
    struct db_batch batch;
    db_batch_init(&batch);

    for (size_t i = 0; i < map_coins->size; i++) {
        struct coins_map_entry *e = &map_coins->entries[i];
        if (e->entry.flags & COINS_CACHE_DIRTY) {
            char key[64];
            size_t keylen;
            make_coins_key(key, &keylen, &e->txid);

            if (coins_is_pruned(&e->entry.coins)) {
                db_batch_delete(&batch, key, keylen);
            } else {
                struct byte_stream s;
                stream_init(&s, 256);

                stream_write_varint(&s, (uint64_t)e->entry.coins.version);

                bool vout0 = e->entry.coins.num_vout > 0 &&
                             !tx_out_is_null(&e->entry.coins.vout[0]);
                bool vout1 = e->entry.coins.num_vout > 1 &&
                             !tx_out_is_null(&e->entry.coins.vout[1]);
                uint64_t nCode = (e->entry.coins.is_coinbase ? 1 : 0) |
                                 (vout0 ? 2 : 0) |
                                 (vout1 ? 4 : 0);
                stream_write_varint(&s, nCode);

                if (vout0)
                    compressed_txout_serialize(&e->entry.coins.vout[0], &s);
                if (vout1)
                    compressed_txout_serialize(&e->entry.coins.vout[1], &s);

                stream_write_varint(&s, (uint64_t)e->entry.coins.height);

                db_batch_put(&batch, key, keylen, (char *)s.data, s.size);
                stream_free(&s);
            }
        }
    }

    if (!uint256_is_null(hash_block)) {
        char key = DB_BEST_BLOCK;
        db_batch_put(&batch, &key, 1, (char *)hash_block->data, 32);
    }

    bool ok = db_write_batch(&cvdb->db, &batch, true);
    db_batch_free(&batch);
    return ok;
}
