/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SQLite-backed wallet storage. Uses node.db tables directly.
 * Replaces wallet_db.c (LevelDB) for all runtime wallet operations. */

#include "wallet/wallet_sqlite.h"
#include "wallet/wallet_keystore.h"
#include "wallet/keystore.h"
#include "keys/key.h"
#include "core/serialize.h"
#include "support/cleanse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Encryption helpers (wave 8 wallet-at-rest) ──────────────── */

/* Returns the wallet passphrase if set, NULL otherwise.  When the
 * env var is empty we treat it as "no encryption" — a conscious
 * operator decision must supply a non-empty string. */
static const char *wallet_passphrase(void)
{
    const char *p = getenv("ZCL_WALLET_PASSPHRASE");
    return (p && *p) ? p : NULL;
}

/* Detect a WKS1 envelope header in a blob.  Safe with NULL. */
static bool is_wks1_blob(const void *data, size_t len)
{
    return data && len >= WKS_HEADER_LEN &&
           memcmp(data, WKS_MAGIC, WKS_MAGIC_LEN) == 0;
}

/* Encrypt `plain` (plen bytes) into a malloc'd envelope.  Sets
 * *out and *out_len on success; caller must free *out.  Returns
 * false on any encryption failure (out stays NULL). */
static bool wallet_encrypt_blob(const uint8_t *plain, size_t plen,
                                 uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    const char *pass = wallet_passphrase();
    if (!pass) return false;

    size_t cap = wks_envelope_size(plen);
    uint8_t *buf = malloc(cap);
    if (!buf) return false;

    size_t elen = 0;
    if (!wks_encrypt(plain, plen, pass, wks_default_iterations(),
                     buf, cap, &elen)) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = elen;
    return true;
}

/* Decrypt a WKS1 envelope into a malloc'd plaintext.  Sets *out
 * and *out_len on success; caller must memory_cleanse+free *out.
 * Returns false on wrong passphrase / tampered data. */
static bool wallet_decrypt_blob(const uint8_t *envelope, size_t env_len,
                                 uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    const char *pass = wallet_passphrase();
    if (!pass) return false;

    /* Plaintext can never be longer than the envelope. */
    uint8_t *buf = malloc(env_len);
    if (!buf) return false;

    size_t plen = 0;
    if (!wks_decrypt(envelope, env_len, pass, buf, env_len, &plen)) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_len = plen;
    return true;
}

/* ── Open / Close ──────────────────────────────────────────────── */

bool wallet_sqlite_open(struct wallet_sqlite *ws, sqlite3 *db)
{
    if (!db) return false;
    memset(ws, 0, sizeof(*ws));
    ws->db = db;

    int rc;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_keys"
        " (pubkey_hash, pubkey, privkey, compressed)"
        " VALUES(?,?,?,?)",
        -1, &ws->stmt_key_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT pubkey_hash, pubkey, privkey, compressed"
        " FROM wallet_keys",
        -1, &ws->stmt_key_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_transactions"
        " (txid, raw_tx, block_hash, block_height, time_received, from_me)"
        " VALUES(?,?,?,?,?,?)",
        -1, &ws->stmt_tx_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT txid, raw_tx, block_hash, block_height,"
        " time_received, from_me"
        " FROM wallet_transactions",
        -1, &ws->stmt_tx_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_seed(id, seed, next_child)"
        " VALUES(1, ?, ?)",
        -1, &ws->stmt_seed_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT seed, next_child FROM wallet_seed WHERE id=1",
        -1, &ws->stmt_seed_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_sapling_keys"
        " (ivk, xsk, xfvk, diversifier, pk_d, child_index, address)"
        " VALUES(?,?,?,?,?,?,'')",
        -1, &ws->stmt_zkey_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT ivk, xsk, xfvk, diversifier, pk_d, child_index"
        " FROM wallet_sapling_keys",
        -1, &ws->stmt_zkey_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_scripts"
        " (script_hash, redeem_script) VALUES(?,?)",
        -1, &ws->stmt_script_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT script_hash, redeem_script FROM wallet_scripts",
        -1, &ws->stmt_script_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO wallet_watch_only"
        " (address_hash, address, created_at) VALUES(?,?,?)",
        -1, &ws->stmt_watch_write, NULL);
    if (rc != SQLITE_OK) goto fail;

    rc = sqlite3_prepare_v2(db,
        "SELECT address_hash, address FROM wallet_watch_only",
        -1, &ws->stmt_watch_read, NULL);
    if (rc != SQLITE_OK) goto fail;

    ws->open = true;
    return true;

fail:
    fprintf(stderr, "wallet_sqlite_open: prepare failed: %s\n",
            sqlite3_errmsg(db));
    wallet_sqlite_close(ws);
    return false;
}

void wallet_sqlite_close(struct wallet_sqlite *ws)
{
    if (ws->stmt_key_write)    { sqlite3_finalize(ws->stmt_key_write);    ws->stmt_key_write = NULL; }
    if (ws->stmt_key_read)     { sqlite3_finalize(ws->stmt_key_read);     ws->stmt_key_read = NULL; }
    if (ws->stmt_tx_write)     { sqlite3_finalize(ws->stmt_tx_write);     ws->stmt_tx_write = NULL; }
    if (ws->stmt_tx_read)      { sqlite3_finalize(ws->stmt_tx_read);      ws->stmt_tx_read = NULL; }
    if (ws->stmt_seed_write)   { sqlite3_finalize(ws->stmt_seed_write);   ws->stmt_seed_write = NULL; }
    if (ws->stmt_seed_read)    { sqlite3_finalize(ws->stmt_seed_read);    ws->stmt_seed_read = NULL; }
    if (ws->stmt_zkey_write)   { sqlite3_finalize(ws->stmt_zkey_write);   ws->stmt_zkey_write = NULL; }
    if (ws->stmt_zkey_read)    { sqlite3_finalize(ws->stmt_zkey_read);    ws->stmt_zkey_read = NULL; }
    if (ws->stmt_script_write) { sqlite3_finalize(ws->stmt_script_write); ws->stmt_script_write = NULL; }
    if (ws->stmt_script_read)  { sqlite3_finalize(ws->stmt_script_read);  ws->stmt_script_read = NULL; }
    if (ws->stmt_watch_write)  { sqlite3_finalize(ws->stmt_watch_write);  ws->stmt_watch_write = NULL; }
    if (ws->stmt_watch_read)   { sqlite3_finalize(ws->stmt_watch_read);   ws->stmt_watch_read = NULL; }
    ws->db = NULL;
    ws->open = false;
}

/* ── Keys ──────────────────────────────────────────────────────── */

bool wallet_sqlite_write_key(struct wallet_sqlite *ws, const struct pubkey *pk,
                              const struct privkey *key)
{
    if (!ws->open) return false;

    struct key_id kid = pubkey_get_id(pk);

    /* Encrypt the private key if a passphrase is configured. */
    uint8_t *enc_blob = NULL;
    size_t enc_len = 0;
    bool encrypted = wallet_encrypt_blob(key->vch, 32, &enc_blob, &enc_len);

    sqlite3_stmt *s = ws->stmt_key_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, kid.id.data, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, pk->vch, (int)pk->size, SQLITE_STATIC);

    if (encrypted) {
        sqlite3_bind_blob(s, 3, enc_blob, (int)enc_len, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_blob(s, 3, key->vch, 32, SQLITE_STATIC);
    }
    sqlite3_bind_int(s, 4, key->fCompressed ? 1 : 0);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (enc_blob) { memory_cleanse(enc_blob, enc_len); free(enc_blob); }
    return ok;
}

bool wallet_sqlite_read_keys(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_key_read;
    sqlite3_reset(s);

    while (sqlite3_step(s) == SQLITE_ROW) {
        int pk_len = sqlite3_column_bytes(s, 1);
        const void *pk_data = sqlite3_column_blob(s, 1);
        int priv_len = sqlite3_column_bytes(s, 2);
        const void *priv_data = sqlite3_column_blob(s, 2);
        int compressed = sqlite3_column_int(s, 3);

        if (!pk_data || pk_len < 33 || !priv_data || priv_len < 32)
            continue;

        struct pubkey pk;
        pubkey_set(&pk, pk_data, (unsigned int)pk_len);

        struct privkey key;
        privkey_init(&key);

        if (is_wks1_blob(priv_data, (size_t)priv_len)) {
            /* Encrypted-at-rest: decrypt to a temp buffer. */
            uint8_t *plain = NULL;
            size_t plain_len = 0;
            if (!wallet_decrypt_blob(priv_data, (size_t)priv_len,
                                     &plain, &plain_len) ||
                plain_len < 32) {
                /* Wrong passphrase or tampered blob — skip key. */
                if (plain) { memory_cleanse(plain, plain_len); free(plain); }
                continue;
            }
            memcpy(key.vch, plain, 32);
            memory_cleanse(plain, plain_len);
            free(plain);
        } else {
            /* Plaintext legacy blob. */
            memcpy(key.vch, priv_data, 32);
        }
        key.fValid = true;
        key.fCompressed = (compressed != 0);

        keystore_add_key(&w->keystore, &key);
        memory_cleanse(key.vch, 32);
    }

    return true;
}

/* ── Transactions ──────────────────────────────────────────────── */

bool wallet_sqlite_write_tx(struct wallet_sqlite *ws,
                              const struct wallet_tx *wtx)
{
    if (!ws->open) return false;

    /* Serialize the transaction */
    struct byte_stream bs;
    stream_init(&bs, 512);
    transaction_serialize(&wtx->tx, &bs);

    sqlite3_stmt *s = ws->stmt_tx_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, wtx->tx.hash.data, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, bs.data, (int)bs.size, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, wtx->hash_block.data, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, wtx->confirms > 0 ? wtx->confirms : 0);
    sqlite3_bind_int64(s, 5, wtx->time_received);
    sqlite3_bind_int(s, 6, wtx->from_me ? 1 : 0);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    stream_free(&bs);
    return ok;
}

bool wallet_sqlite_read_txs(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_tx_read;
    sqlite3_reset(s);

    while (sqlite3_step(s) == SQLITE_ROW) {
        int raw_len = sqlite3_column_bytes(s, 1);
        const void *raw_data = sqlite3_column_blob(s, 1);
        if (!raw_data || raw_len < 10) continue;

        struct wallet_tx wtx;
        memset(&wtx, 0, sizeof(wtx));

        /* block_hash */
        const void *bh = sqlite3_column_blob(s, 2);
        if (bh && sqlite3_column_bytes(s, 2) >= 32)
            memcpy(wtx.hash_block.data, bh, 32);

        wtx.confirms = sqlite3_column_int(s, 3);
        wtx.time_received = sqlite3_column_int64(s, 4);
        wtx.from_me = sqlite3_column_int(s, 5) != 0;

        struct byte_stream bs;
        stream_init_from_data(&bs, raw_data, (size_t)raw_len);
        transaction_init(&wtx.tx);
        if (transaction_deserialize(&wtx.tx, &bs)) {
            wtx.used = true;
            wallet_add_to_wallet(w, &wtx);
        }
        stream_free(&bs);
    }

    return true;
}

/* ── Best block / scan height ──────────────────────────────────── */

bool wallet_sqlite_write_best_block(struct wallet_sqlite *ws,
                                      const struct uint256 *hash)
{
    if (!ws->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ws->db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('wallet_best_block',?)",
        -1, &s, NULL);
    if (!s) return false;
    sqlite3_bind_blob(s, 1, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool wallet_sqlite_read_best_block(struct wallet_sqlite *ws,
                                     struct uint256 *hash)
{
    if (!ws->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ws->db,
        "SELECT value FROM node_state WHERE key='wallet_best_block'",
        -1, &s, NULL);
    if (!s) return false;
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(s, 0);
        if (data && sqlite3_column_bytes(s, 0) >= 32) {
            memcpy(hash->data, data, 32);
            ok = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}

bool wallet_sqlite_write_scan_height(struct wallet_sqlite *ws, int height)
{
    if (!ws->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ws->db,
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('wallet_scan_height',?)",
        -1, &s, NULL);
    if (!s) return false;
    int32_t h = (int32_t)height;
    sqlite3_bind_blob(s, 1, &h, 4, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool wallet_sqlite_read_scan_height(struct wallet_sqlite *ws, int *height)
{
    if (!ws->open) return false;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ws->db,
        "SELECT value FROM node_state WHERE key='wallet_scan_height'",
        -1, &s, NULL);
    if (!s) return false;
    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(s, 0);
        if (data && sqlite3_column_bytes(s, 0) >= 4) {
            int32_t h;
            memcpy(&h, data, 4);
            *height = h;
            ok = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}

/* ── Sapling seed & keys ───────────────────────────────────────── */

bool wallet_sqlite_write_sapling_seed(struct wallet_sqlite *ws,
                                        const uint8_t seed[32])
{
    if (!ws->open) return false;

    /* Read current next_child to preserve it */
    int next_child = 0;
    sqlite3_reset(ws->stmt_seed_read);
    if (sqlite3_step(ws->stmt_seed_read) == SQLITE_ROW)
        next_child = sqlite3_column_int(ws->stmt_seed_read, 1);

    /* Encrypt the seed if a passphrase is configured. */
    uint8_t *enc_blob = NULL;
    size_t enc_len = 0;
    bool encrypted = wallet_encrypt_blob(seed, 32, &enc_blob, &enc_len);

    sqlite3_reset(ws->stmt_seed_write);
    if (encrypted) {
        sqlite3_bind_blob(ws->stmt_seed_write, 1,
                          enc_blob, (int)enc_len, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_blob(ws->stmt_seed_write, 1, seed, 32, SQLITE_STATIC);
    }
    sqlite3_bind_int(ws->stmt_seed_write, 2, next_child);

    bool ok = sqlite3_step(ws->stmt_seed_write) == SQLITE_DONE;
    if (enc_blob) { memory_cleanse(enc_blob, enc_len); free(enc_blob); }
    return ok;
}

bool wallet_sqlite_read_sapling_seed(struct wallet_sqlite *ws,
                                       uint8_t seed[32])
{
    if (!ws->open) return false;
    sqlite3_reset(ws->stmt_seed_read);
    if (sqlite3_step(ws->stmt_seed_read) == SQLITE_ROW) {
        const void *data = sqlite3_column_blob(ws->stmt_seed_read, 0);
        int data_len = sqlite3_column_bytes(ws->stmt_seed_read, 0);
        if (!data || data_len < 32) return false;

        if (is_wks1_blob(data, (size_t)data_len)) {
            uint8_t *plain = NULL;
            size_t plain_len = 0;
            if (!wallet_decrypt_blob(data, (size_t)data_len,
                                     &plain, &plain_len) ||
                plain_len < 32) {
                if (plain) { memory_cleanse(plain, plain_len); free(plain); }
                return false;
            }
            memcpy(seed, plain, 32);
            memory_cleanse(plain, plain_len);
            free(plain);
            return true;
        }
        memcpy(seed, data, 32);
        return true;
    }
    return false;
}

bool wallet_sqlite_write_sapling_key(struct wallet_sqlite *ws,
                                       uint32_t child_index,
                                       const struct sapling_key_entry *entry)
{
    if (!ws->open) return false;

    /* Encrypt the extended spending key (xsk) — this is the secret
     * material that controls coin spending.  The xfvk, ivk, pk_d,
     * and diversifier are view-only and stored in plaintext so the
     * wallet can scan without the passphrase. */
    uint8_t *enc_xsk = NULL;
    size_t enc_xsk_len = 0;
    size_t xsk_raw_len = sizeof(struct zip32_xsk);
    bool encrypted = wallet_encrypt_blob((const uint8_t *)&entry->xsk,
                                          xsk_raw_len,
                                          &enc_xsk, &enc_xsk_len);

    sqlite3_stmt *s = ws->stmt_zkey_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, entry->ivk, 32, SQLITE_STATIC);
    if (encrypted) {
        sqlite3_bind_blob(s, 2, enc_xsk, (int)enc_xsk_len, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_blob(s, 2, &entry->xsk, (int)xsk_raw_len, SQLITE_STATIC);
    }
    sqlite3_bind_blob(s, 3, &entry->xfvk, (int)sizeof(struct zip32_xfvk),
                      SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, entry->diversifier, 11, SQLITE_STATIC);
    sqlite3_bind_blob(s, 5, entry->pk_d, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 6, (int)child_index);

    bool ok = sqlite3_step(s) == SQLITE_DONE;
    if (enc_xsk) { memory_cleanse(enc_xsk, enc_xsk_len); free(enc_xsk); }

    /* Update seed's next_child */
    sqlite3_stmt *upd = NULL;
    sqlite3_prepare_v2(ws->db,
        "UPDATE wallet_seed SET next_child=MAX(next_child,?+1) WHERE id=1",
        -1, &upd, NULL);
    if (upd) {
        sqlite3_bind_int(upd, 1, (int)child_index);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
    }

    return ok;
}

bool wallet_sqlite_read_sapling_keys(struct wallet_sqlite *ws,
                                       struct wallet *w)
{
    if (!ws->open) return false;

    /* Read seed first */
    uint8_t seed[32];
    if (wallet_sqlite_read_sapling_seed(ws, seed)) {
        sapling_keystore_set_seed(&w->sapling_keys, seed);
        memory_cleanse(seed, 32);
    }

    sqlite3_stmt *s = ws->stmt_zkey_read;
    sqlite3_reset(s);

    struct sapling_keystore *sks = &w->sapling_keys;

    while (sqlite3_step(s) == SQLITE_ROW) {
        if (sks->num_keys >= MAX_SAPLING_KEYS) break;

        const void *ivk = sqlite3_column_blob(s, 0);
        const void *xsk_blob = sqlite3_column_blob(s, 1);
        const void *xfvk = sqlite3_column_blob(s, 2);
        const void *div = sqlite3_column_blob(s, 3);
        const void *pkd = sqlite3_column_blob(s, 4);
        int child = sqlite3_column_int(s, 5);

        int xsk_blob_len = sqlite3_column_bytes(s, 1);

        if (!ivk || !xsk_blob || !div || !pkd) continue;

        /* Resolve xsk: may be an encrypted WKS1 envelope or raw. */
        const void *xsk_data = xsk_blob;
        size_t xsk_data_len = (size_t)xsk_blob_len;
        uint8_t *xsk_decrypted = NULL;
        size_t xsk_plain_len = 0;

        if (is_wks1_blob(xsk_blob, (size_t)xsk_blob_len)) {
            if (!wallet_decrypt_blob(xsk_blob, (size_t)xsk_blob_len,
                                     &xsk_decrypted, &xsk_plain_len) ||
                xsk_plain_len < sizeof(struct zip32_xsk)) {
                if (xsk_decrypted) {
                    memory_cleanse(xsk_decrypted, xsk_plain_len);
                    free(xsk_decrypted);
                }
                continue;  /* wrong passphrase or corrupt — skip key */
            }
            xsk_data = xsk_decrypted;
            xsk_data_len = xsk_plain_len;
        }

        if (xsk_data_len < sizeof(struct zip32_xsk)) {
            if (xsk_decrypted) {
                memory_cleanse(xsk_decrypted, xsk_plain_len);
                free(xsk_decrypted);
            }
            continue;
        }

        struct sapling_key_entry *entry = &sks->keys[sks->num_keys];
        memcpy(entry->ivk, ivk, 32);
        memcpy(&entry->xsk, xsk_data, sizeof(struct zip32_xsk));
        if (xfvk && sqlite3_column_bytes(s, 2) >= (int)sizeof(struct zip32_xfvk))
            memcpy(&entry->xfvk, xfvk, sizeof(struct zip32_xfvk));
        else
            zip32_xsk_to_xfvk(&entry->xfvk, &entry->xsk);
        memcpy(entry->diversifier, div, 11);
        memcpy(entry->pk_d, pkd, 32);
        entry->child_index = (uint32_t)child;
        entry->used = true;
        sks->num_keys++;

        if ((uint32_t)child >= sks->next_child_index)
            sks->next_child_index = (uint32_t)child + 1;

        if (xsk_decrypted) {
            memory_cleanse(xsk_decrypted, xsk_plain_len);
            free(xsk_decrypted);
        }
    }

    return true;
}

/* ── Scripts ───────────────────────────────────────────────────── */

bool wallet_sqlite_write_script(struct wallet_sqlite *ws,
                                  const struct uint160 *script_id,
                                  const struct script *redeem_script)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_script_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, script_id->data, 20, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, redeem_script->data, (int)redeem_script->size,
                      SQLITE_STATIC);
    return sqlite3_step(s) == SQLITE_DONE;
}

bool wallet_sqlite_read_scripts(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_script_read;
    sqlite3_reset(s);

    while (sqlite3_step(s) == SQLITE_ROW) {
        const void *hash = sqlite3_column_blob(s, 0);
        const void *data = sqlite3_column_blob(s, 1);
        int data_len = sqlite3_column_bytes(s, 1);

        if (!hash || !data || data_len == 0 || data_len > MAX_SCRIPT_SIZE)
            continue;

        struct script scr;
        script_init(&scr);
        memcpy(scr.data, data, (size_t)data_len);
        scr.size = (size_t)data_len;
        keystore_add_cscript(&w->keystore, &scr);
    }

    return true;
}

/* ── Watch-only addresses ──────────────────────────────────────── */

bool wallet_sqlite_write_watch_only(struct wallet_sqlite *ws,
                                      const uint8_t address_hash[20],
                                      const char *address)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_watch_write;
    sqlite3_reset(s);
    sqlite3_bind_blob(s, 1, address_hash, 20, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, address, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, (int64_t)time(NULL));
    return sqlite3_step(s) == SQLITE_DONE;
}

bool wallet_sqlite_read_watch_only(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws->open) return false;

    sqlite3_stmt *s = ws->stmt_watch_read;
    sqlite3_reset(s);

    while (sqlite3_step(s) == SQLITE_ROW) {
        const void *hash = sqlite3_column_blob(s, 0);
        if (!hash || sqlite3_column_bytes(s, 0) != 20)
            continue;

        struct key_id kid;
        memcpy(kid.id.data, hash, 20);
        keystore_add_watch_only_id(&w->keystore, &kid);
    }

    return true;
}

/* ── Flush all wallet state to SQLite ──────────────────────────── */

bool wallet_sqlite_flush(struct wallet_sqlite *ws, struct wallet *w)
{
    if (!ws->open) return false;

    sqlite3_exec(ws->db, "BEGIN", NULL, NULL, NULL);

    zcl_mutex_lock(&w->cs);

    /* Write all keys */
    for (size_t i = 0; i < w->keystore.num_keys; i++) {
        if (!w->keystore.keys[i].used) continue;
        if (!w->keystore.keys[i].key.fValid) continue;

        struct pubkey pk;
        if (privkey_get_pubkey(&w->keystore.keys[i].key, &pk))
            wallet_sqlite_write_key(ws, &pk, &w->keystore.keys[i].key);
    }

    /* Write all wallet transactions */
    for (size_t i = 0; i < MAX_WALLET_TX; i++) {
        if (!w->map_wallet[i].used) continue;
        wallet_sqlite_write_tx(ws, &w->map_wallet[i]);
    }

    /* Write Sapling seed and keys */
    struct sapling_keystore *sks = &w->sapling_keys;
    if (sks->has_seed)
        wallet_sqlite_write_sapling_seed(ws, sks->seed);
    for (size_t i = 0; i < sks->num_keys; i++) {
        if (sks->keys[i].used)
            wallet_sqlite_write_sapling_key(ws, sks->keys[i].child_index,
                                              &sks->keys[i]);
    }

    /* Write redeem scripts */
    for (size_t i = 0; i < w->keystore.num_scripts; i++) {
        if (w->keystore.scripts[i].used)
            wallet_sqlite_write_script(ws, &w->keystore.scripts[i].script_id,
                                         &w->keystore.scripts[i].redeem_script);
    }

    /* Write scan height */
    wallet_sqlite_write_scan_height(ws, w->best_block_height);

    zcl_mutex_unlock(&w->cs);

    sqlite3_exec(ws->db, "COMMIT", NULL, NULL, NULL);
    return true;
}
