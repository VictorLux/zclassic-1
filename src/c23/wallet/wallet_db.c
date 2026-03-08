/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "wallet/wallet_db.h"
#include "core/serialize.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key prefixes for wallet DB entries */
#define PREFIX_KEY    "key"
#define PREFIX_TX     "tx"
#define PREFIX_BEST   "bestblock"

static void make_key_record(const struct key_id *kid,
                             char *buf, size_t *len)
{
    memcpy(buf, PREFIX_KEY, 3);
    memcpy(buf + 3, kid->id.data, 20);
    *len = 23;
}

static void make_tx_record(const struct uint256 *hash,
                            char *buf, size_t *len)
{
    memcpy(buf, PREFIX_TX, 2);
    memcpy(buf + 2, hash->data, 32);
    *len = 34;
}

bool wallet_db_open(struct wallet_db *wdb, const char *path)
{
    memset(wdb, 0, sizeof(*wdb));
    if (!db_wrapper_open(&wdb->db, path, 4 << 20, false, false))
        return false;
    wdb->open = true;
    return true;
}

void wallet_db_close(struct wallet_db *wdb)
{
    if (wdb->open) {
        db_wrapper_close(&wdb->db);
        wdb->open = false;
    }
}

bool wallet_db_write_key(struct wallet_db *wdb, const struct pubkey *pk,
                          const struct privkey *key)
{
    if (!wdb->open) return false;

    struct key_id kid = pubkey_get_id(pk);
    char dbkey[64];
    size_t dbkey_len;
    make_key_record(&kid, dbkey, &dbkey_len);

    /* Value: compressed flag (1 byte) + pubkey size (1 byte) + pubkey +
     * private key (32 bytes) */
    unsigned char val[1 + 1 + PUBLIC_KEY_SIZE + 32];
    size_t vlen = 0;
    val[vlen++] = key->fCompressed ? 1 : 0;
    val[vlen++] = (unsigned char)pk->size;
    memcpy(val + vlen, pk->vch, pk->size);
    vlen += pk->size;
    memcpy(val + vlen, key->vch, 32);
    vlen += 32;

    bool ok = db_write(&wdb->db, dbkey, dbkey_len,
                       (const char *)val, vlen, true);
    memory_cleanse(val + vlen - 32, 32);
    return ok;
}

bool wallet_db_read_keys(struct wallet_db *wdb, struct wallet *w)
{
    if (!wdb->open) return false;

    struct db_iterator it;
    db_iter_init(&it, &wdb->db);
    db_iter_seek(&it, PREFIX_KEY, 3);

    while (db_iter_valid(&it)) {
        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 3 || memcmp(k, PREFIX_KEY, 3) != 0)
            break;

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (vlen < 35) { /* 1 + 1 + 33 minimum */
            db_iter_next(&it);
            continue;
        }

        const unsigned char *vp = (const unsigned char *)v;
        bool compressed = vp[0] != 0;
        unsigned int pk_size = vp[1];
        if (pk_size > PUBLIC_KEY_SIZE || vlen < 2 + pk_size + 32) {
            db_iter_next(&it);
            continue;
        }

        struct pubkey pk;
        pubkey_set(&pk, vp + 2, pk_size);

        struct privkey key;
        privkey_init(&key);
        memcpy(key.vch, vp + 2 + pk_size, 32);
        key.fValid = true;
        key.fCompressed = compressed;

        keystore_add_key(&w->keystore, &key);
        memory_cleanse(key.vch, 32);

        db_iter_next(&it);
    }

    db_iter_free(&it);
    return true;
}

bool wallet_db_write_tx(struct wallet_db *wdb, const struct wallet_tx *wtx)
{
    if (!wdb->open) return false;

    char dbkey[64];
    size_t dbkey_len;
    make_tx_record(&wtx->tx.hash, dbkey, &dbkey_len);

    /* Serialize: hash_block(32) + time_received(8) + from_me(1) +
     * confirms(4) + serialized tx */
    struct byte_stream s;
    stream_init(&s, 512);

    stream_write_bytes(&s, wtx->hash_block.data, 32);
    stream_write_i64_le(&s, wtx->time_received);
    unsigned char from_me = wtx->from_me ? 1 : 0;
    stream_write_bytes(&s, &from_me, 1);
    stream_write_u32_le(&s, (uint32_t)wtx->confirms);
    transaction_serialize(&wtx->tx, &s);

    bool ok = db_write(&wdb->db, dbkey, dbkey_len,
                       (const char *)s.data, s.size, false);
    stream_free(&s);
    return ok;
}

bool wallet_db_read_txs(struct wallet_db *wdb, struct wallet *w)
{
    if (!wdb->open) return false;

    struct db_iterator it;
    db_iter_init(&it, &wdb->db);
    db_iter_seek(&it, PREFIX_TX, 2);

    while (db_iter_valid(&it)) {
        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 2 || memcmp(k, PREFIX_TX, 2) != 0)
            break;

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (vlen < 45) { /* minimum: 32+8+1+4 */
            db_iter_next(&it);
            continue;
        }

        struct byte_stream s;
        stream_init_from_data(&s, (const unsigned char *)v, vlen);

        struct wallet_tx wtx;
        memset(&wtx, 0, sizeof(wtx));
        stream_read_bytes(&s, wtx.hash_block.data, 32);
        stream_read_i64_le(&s, &wtx.time_received);
        unsigned char fm;
        stream_read_bytes(&s, &fm, 1);
        wtx.from_me = fm != 0;
        uint32_t conf;
        stream_read_u32_le(&s, &conf);
        wtx.confirms = (int)conf;

        transaction_init(&wtx.tx);
        if (transaction_deserialize(&wtx.tx, &s)) {
            wtx.used = true;
            wallet_add_to_wallet(w, &wtx);
        }

        stream_free(&s);
        db_iter_next(&it);
    }

    db_iter_free(&it);
    return true;
}

bool wallet_db_write_best_block(struct wallet_db *wdb,
                                  const struct uint256 *hash)
{
    if (!wdb->open) return false;
    return db_write(&wdb->db, PREFIX_BEST, 9,
                    (const char *)hash->data, 32, true);
}

bool wallet_db_read_best_block(struct wallet_db *wdb, struct uint256 *hash)
{
    if (!wdb->open) return false;
    char *val = NULL;
    size_t vlen = 0;
    if (!db_read(&wdb->db, PREFIX_BEST, 9, &val, &vlen))
        return false;
    if (vlen != 32) {
        free(val);
        return false;
    }
    memcpy(hash->data, val, 32);
    free(val);
    return true;
}

bool wallet_db_flush(struct wallet_db *wdb, struct wallet *w)
{
    if (!wdb->open) return false;

    /* Write all keys */
    zcl_mutex_lock(&w->cs);
    for (size_t i = 0; i < w->keystore.num_keys; i++) {
        if (!w->keystore.keys[i].used) continue;

        struct pubkey pk;
        if (privkey_get_pubkey(&w->keystore.keys[i].key, &pk)) {
            wallet_db_write_key(wdb, &pk, &w->keystore.keys[i].key);
        }
    }

    /* Write all wallet transactions */
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used) continue;
        wallet_db_write_tx(wdb, &w->map_wallet[i]);
    }
    zcl_mutex_unlock(&w->cs);

    return true;
}
