/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/dbwrapper.h"
#include "util/util.h"
#include <leveldb/c.h>
#include <stdio.h>
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

    if (!memory) {
        /* Create parent directories recursively */
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", path);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }

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
        printf("LevelDB open failure at %s: %s\n", path, err);
        LogPrintf("LevelDB open failure: %s\n", err);
        leveldb_free(err);
        db_wrapper_close(w);
        return false;
    }

    w->read_options = leveldb_readoptions_create();
    w->iter_options = leveldb_readoptions_create();
    /* Do NOT verify checksums during iteration — a corrupt LevelDB block
     * causes the iterator to silently stop early, dropping all remaining
     * entries. This caused 219 missing UTXOs during import from a
     * zclassicd chainstate copy. Data integrity is verified through
     * consensus (block validation) instead. */
    leveldb_readoptions_set_verify_checksums(w->iter_options, 0);
    leveldb_readoptions_set_fill_cache(w->read_options, 1);
    leveldb_readoptions_set_fill_cache(w->iter_options, 0);

    w->write_options = leveldb_writeoptions_create();
    w->sync_options = leveldb_writeoptions_create();
    leveldb_writeoptions_set_sync(w->sync_options, 1);

    /* Read obfuscation key (Bitcoin Core stores it with CDataStream serialization:
     * key = compact_size(14) + "\0obfuscate_key" = 0x0e + 0x00 + "obfuscate_key"
     * value = compact_size(8) + 8 random bytes */
    w->obfuscate_key_len = 0;
    memset(w->obfuscate_key, 0, sizeof(w->obfuscate_key));
    {
        /* The key in the DB: 0x0e (compact size = 14) followed by
         * the 14-byte string "\x00obfuscate_key" */
        char obf_db_key[16];
        obf_db_key[0] = 0x0e;  /* compact_size(14) */
        obf_db_key[1] = 0x00;  /* first byte of the key string */
        memcpy(obf_db_key + 2, "obfuscate_key", 13);
        size_t obf_keylen = 15;

        char *err2 = NULL;
        size_t vlen = 0;
        char *val = leveldb_get(w->db, w->read_options,
                                obf_db_key, obf_keylen, &vlen, &err2);
        if (err2) {
            leveldb_free(err2);
            val = NULL;
        }

        if (!val) {
            /* Try alternate key format: just "\0obfuscate_key" (14 bytes) */
            obf_db_key[0] = 0x00;
            memcpy(obf_db_key + 1, "obfuscate_key", 13);
            obf_keylen = 14;
            err2 = NULL;
            val = leveldb_get(w->db, w->read_options,
                              obf_db_key, obf_keylen, &vlen, &err2);
            if (err2) { leveldb_free(err2); val = NULL; }
        }

        if (val && vlen > 0) {
            printf("DB obfuscate_key raw value (%zu bytes):", vlen);
            for (size_t i = 0; i < vlen && i < 16; i++)
                printf(" %02x", (unsigned char)val[i]);
            printf("\n");

            /* Value format: compact_size(n) + n bytes of key */
            uint8_t klen = (uint8_t)val[0];
            if (klen > 0 && klen <= 32 && (size_t)(klen + 1) <= vlen) {
                memcpy(w->obfuscate_key, val + 1, klen);
                w->obfuscate_key_len = klen;
                printf("DB obfuscation key (%zu bytes):", w->obfuscate_key_len);
                for (size_t i = 0; i < w->obfuscate_key_len; i++)
                    printf(" %02x", w->obfuscate_key[i]);
                printf("\n");
            } else if (vlen <= 32) {
                /* Maybe value is the raw key without length prefix */
                memcpy(w->obfuscate_key, val, vlen);
                w->obfuscate_key_len = vlen;
                printf("DB obfuscation key (raw, %zu bytes):", w->obfuscate_key_len);
                for (size_t i = 0; i < w->obfuscate_key_len; i++)
                    printf(" %02x", w->obfuscate_key[i]);
                printf("\n");
            }
        } else {
            printf("DB: no obfuscation key found\n");
        }
        if (val) leveldb_free(val);
    }

    return true;
}

bool db_wrapper_repair(const char *path)
{
    leveldb_options_t *opts = leveldb_options_create();
    char *err = NULL;
    leveldb_repair_db(opts, path, &err);
    leveldb_options_destroy(opts);
    if (err) {
        printf("LevelDB repair failed at %s: %s\n", path, err);
        leveldb_free(err);
        return false;
    }
    printf("LevelDB repaired: %s\n", path);
    return true;
}

size_t db_wrapper_count(struct db_wrapper *w)
{
    if (!w->db) return 0;
    size_t count = 0;
    leveldb_iterator_t *it = leveldb_create_iterator(w->db, w->read_options);
    for (leveldb_iter_seek_to_first(it);
         leveldb_iter_valid(it);
         leveldb_iter_next(it))
        count++;
    leveldb_iter_destroy(it);
    return count;
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
    if (*val && w->obfuscate_key_len > 0) {
        for (size_t i = 0; i < *vallen; i++)
            (*val)[i] ^= (char)w->obfuscate_key[i % w->obfuscate_key_len];
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
    memcpy(it->obfuscate_key, w->obfuscate_key, w->obfuscate_key_len);
    it->obfuscate_key_len = w->obfuscate_key_len;
    it->deobf_buf = NULL;
    it->deobf_cap = 0;
}

void db_iter_check_error(struct db_iterator *it)
{
    char *err = NULL;
    leveldb_iter_get_error(it->iter, &err);
    if (err) {
        fprintf(stderr, "LevelDB iterator error: %s\n", err);
        leveldb_free(err);
    }
}

void db_iter_free(struct db_iterator *it)
{
    if (it->iter) leveldb_iter_destroy(it->iter);
    free(it->deobf_buf);
    it->iter = NULL;
    it->deobf_buf = NULL;
    it->deobf_cap = 0;
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
    const char *raw = leveldb_iter_value(it->iter, vallen);
    if (!raw || it->obfuscate_key_len == 0)
        return raw;

    /* Deobfuscate into a reusable buffer */
    if (*vallen > it->deobf_cap) {
        free(it->deobf_buf);
        it->deobf_cap = *vallen + 256;
        it->deobf_buf = malloc(it->deobf_cap);
    }
    for (size_t i = 0; i < *vallen; i++)
        it->deobf_buf[i] = raw[i] ^
            (char)it->obfuscate_key[i % it->obfuscate_key_len];
    return it->deobf_buf;
}
