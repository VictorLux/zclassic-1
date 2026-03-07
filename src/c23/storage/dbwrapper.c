/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/dbwrapper.h"
#include "util/util.h"
#include <leveldb/c.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool db_wrapper_open(struct db_wrapper *w, const char *path,
                     size_t cache_size, bool memory, bool wipe)
{
    memset(w, 0, sizeof(*w));

    if (wipe) {
        leveldb_options_t *opts = leveldb_options_create();
        char *err = NULL;
        leveldb_destroy_db(opts, path, &err);
        leveldb_options_destroy(opts);
        if (err) { leveldb_free(err); }
    }

    if (!memory)
        mkdir(path, 0755);

    w->options = leveldb_options_create();
    leveldb_options_set_create_if_missing(w->options, 1);
    leveldb_options_set_compression(w->options, leveldb_no_compression);
    leveldb_options_set_max_open_files(w->options, 64);

    if (cache_size > 0) {
        w->cache = leveldb_cache_create_lru(cache_size);
        leveldb_options_set_cache(w->options, w->cache);
    }

    w->filter_policy = leveldb_filterpolicy_create_bloom(10);
    leveldb_options_set_filter_policy(w->options, w->filter_policy);

    if (memory) {
        w->env = leveldb_create_default_env();
        leveldb_options_set_env(w->options, w->env);
    }

    char *err = NULL;
    w->db = leveldb_open(w->options, path, &err);
    if (err) {
        LogPrintf("LevelDB open failure: %s\n", err);
        leveldb_free(err);
        db_wrapper_close(w);
        return false;
    }

    w->read_options = leveldb_readoptions_create();
    w->iter_options = leveldb_readoptions_create();
    leveldb_readoptions_set_verify_checksums(w->iter_options, 1);
    leveldb_readoptions_set_fill_cache(w->read_options, 1);
    leveldb_readoptions_set_fill_cache(w->iter_options, 0);

    w->write_options = leveldb_writeoptions_create();
    w->sync_options = leveldb_writeoptions_create();
    leveldb_writeoptions_set_sync(w->sync_options, 1);

    return true;
}

void db_wrapper_close(struct db_wrapper *w)
{
    if (w->db) leveldb_close(w->db);
    if (w->read_options) leveldb_readoptions_destroy(w->read_options);
    if (w->iter_options) leveldb_readoptions_destroy(w->iter_options);
    if (w->write_options) leveldb_writeoptions_destroy(w->write_options);
    if (w->sync_options) leveldb_writeoptions_destroy(w->sync_options);
    if (w->filter_policy) leveldb_filterpolicy_destroy(w->filter_policy);
    if (w->cache) leveldb_cache_destroy(w->cache);
    if (w->options) leveldb_options_destroy(w->options);
    if (w->env) leveldb_env_destroy(w->env);
    memset(w, 0, sizeof(*w));
}

bool db_read(struct db_wrapper *w, const char *key, size_t keylen,
             char **val, size_t *vallen)
{
    char *err = NULL;
    *val = leveldb_get(w->db, w->read_options, key, keylen, vallen, &err);
    if (err) {
        LogPrintf("LevelDB read failure: %s\n", err);
        leveldb_free(err);
        return false;
    }
    return *val != NULL;
}

bool db_write(struct db_wrapper *w, const char *key, size_t keylen,
              const char *val, size_t vallen, bool sync)
{
    char *err = NULL;
    leveldb_put(w->db, sync ? w->sync_options : w->write_options,
                key, keylen, val, vallen, &err);
    if (err) {
        LogPrintf("LevelDB write failure: %s\n", err);
        leveldb_free(err);
        return false;
    }
    return true;
}

bool db_exists(struct db_wrapper *w, const char *key, size_t keylen)
{
    size_t vallen;
    char *val = NULL;
    char *err = NULL;
    val = leveldb_get(w->db, w->read_options, key, keylen, &vallen, &err);
    if (err) {
        leveldb_free(err);
        return false;
    }
    bool found = val != NULL;
    leveldb_free(val);
    return found;
}

bool db_erase(struct db_wrapper *w, const char *key, size_t keylen, bool sync)
{
    char *err = NULL;
    leveldb_delete(w->db, sync ? w->sync_options : w->write_options,
                   key, keylen, &err);
    if (err) {
        LogPrintf("LevelDB delete failure: %s\n", err);
        leveldb_free(err);
        return false;
    }
    return true;
}

bool db_is_empty(struct db_wrapper *w)
{
    leveldb_iterator_t *it = leveldb_create_iterator(w->db, w->iter_options);
    leveldb_iter_seek_to_first(it);
    bool empty = !leveldb_iter_valid(it);
    leveldb_iter_destroy(it);
    return empty;
}

/* --- Batch --- */

void db_batch_init(struct db_batch *b)
{
    b->batch = leveldb_writebatch_create();
}

void db_batch_free(struct db_batch *b)
{
    if (b->batch) leveldb_writebatch_destroy(b->batch);
    b->batch = NULL;
}

void db_batch_put(struct db_batch *b, const char *key, size_t keylen,
                  const char *val, size_t vallen)
{
    leveldb_writebatch_put(b->batch, key, keylen, val, vallen);
}

void db_batch_delete(struct db_batch *b, const char *key, size_t keylen)
{
    leveldb_writebatch_delete(b->batch, key, keylen);
}

void db_batch_clear(struct db_batch *b)
{
    leveldb_writebatch_clear(b->batch);
}

bool db_write_batch(struct db_wrapper *w, struct db_batch *b, bool sync)
{
    char *err = NULL;
    leveldb_write(w->db, sync ? w->sync_options : w->write_options,
                  b->batch, &err);
    if (err) {
        LogPrintf("LevelDB batch write failure: %s\n", err);
        leveldb_free(err);
        return false;
    }
    return true;
}

/* --- Iterator --- */

void db_iter_init(struct db_iterator *it, struct db_wrapper *w)
{
    it->iter = leveldb_create_iterator(w->db, w->iter_options);
}

void db_iter_free(struct db_iterator *it)
{
    if (it->iter) leveldb_iter_destroy(it->iter);
    it->iter = NULL;
}

bool db_iter_valid(struct db_iterator *it)
{
    return leveldb_iter_valid(it->iter) != 0;
}

void db_iter_seek_to_first(struct db_iterator *it)
{
    leveldb_iter_seek_to_first(it->iter);
}

void db_iter_seek(struct db_iterator *it, const char *key, size_t keylen)
{
    leveldb_iter_seek(it->iter, key, keylen);
}

void db_iter_next(struct db_iterator *it)
{
    leveldb_iter_next(it->iter);
}

const char *db_iter_key(struct db_iterator *it, size_t *keylen)
{
    return leveldb_iter_key(it->iter, keylen);
}

const char *db_iter_value(struct db_iterator *it, size_t *vallen)
{
    return leveldb_iter_value(it->iter, vallen);
}
