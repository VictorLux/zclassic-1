/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Incremental UTXO set commitment using XOR-hash accumulator.
 *
 * For each UTXO, we compute SHA256(txid || vout_le || value_le || height_le)
 * and XOR it into a 32-byte accumulator. Since XOR is self-inverse,
 * adding and removing a UTXO are the same operation.
 *
 * This gives O(1) updates per UTXO change, versus O(n) for a full
 * Merkle tree rebuild. The commitment can be verified by computing
 * from scratch over the full UTXO set and comparing. */

#include "coins/utxo_commitment.h"
#include "crypto/sha256.h"
#include "crypto/sha3.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sqlite3.h>

_Atomic bool g_utxo_commitment_skip = false;

/* Hash a single UTXO to 32 bytes via SHA256(txid || vout || value || height) */
static void hash_utxo(uint8_t out[32],
                       const uint8_t txid[32], uint32_t vout,
                       int64_t value, int32_t height)
{
    /* txid(32) + vout(4) + value(8) + height(4) = 48 bytes */
    uint8_t buf[48];
    memcpy(buf, txid, 32);
    buf[32] = (uint8_t)(vout & 0xFF);
    buf[33] = (uint8_t)((vout >> 8) & 0xFF);
    buf[34] = (uint8_t)((vout >> 16) & 0xFF);
    buf[35] = (uint8_t)((vout >> 24) & 0xFF);
    uint64_t v = (uint64_t)value;
    for (int i = 0; i < 8; i++)
        buf[36 + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    uint32_t h = (uint32_t)height;
    buf[44] = (uint8_t)(h & 0xFF);
    buf[45] = (uint8_t)((h >> 8) & 0xFF);
    buf[46] = (uint8_t)((h >> 16) & 0xFF);
    buf[47] = (uint8_t)((h >> 24) & 0xFF);

    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_write(&ctx, buf, 48);
    sha256_finalize(&ctx, out);
}

/* XOR 32 bytes from src into dst */
static void xor32(uint8_t dst[32], const uint8_t src[32])
{
    for (int i = 0; i < 32; i++)
        dst[i] ^= src[i];
}

void utxo_commitment_init(struct utxo_commitment *uc)
{
    memset(uc->accumulator, 0, 32);
    uc->count = 0;
}

void utxo_commitment_add(struct utxo_commitment *uc,
                          const uint8_t txid[32], uint32_t vout,
                          int64_t value, int32_t height)
{
    if (atomic_load_explicit(&g_utxo_commitment_skip, memory_order_relaxed))
        return;
    uint8_t h[32];
    hash_utxo(h, txid, vout, value, height);
    xor32(uc->accumulator, h);
    uc->count++;
}

void utxo_commitment_remove(struct utxo_commitment *uc,
                             const uint8_t txid[32], uint32_t vout,
                             int64_t value, int32_t height)
{
    if (atomic_load_explicit(&g_utxo_commitment_skip, memory_order_relaxed))
        return;
    uint8_t h[32];
    hash_utxo(h, txid, vout, value, height);
    xor32(uc->accumulator, h);
    if (uc->count > 0) uc->count--;
}

void utxo_commitment_merge(struct utxo_commitment *dst,
                            const struct utxo_commitment *src)
{
    xor32(dst->accumulator, src->accumulator);
    dst->count += src->count;
}

void utxo_commitment_serialize(const struct utxo_commitment *uc,
                                uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE])
{
    memcpy(buf, uc->accumulator, 32);
    uint64_t c = uc->count;
    for (int i = 0; i < 8; i++)
        buf[32 + i] = (uint8_t)((c >> (8 * i)) & 0xFF);
}

bool utxo_commitment_deserialize(struct utxo_commitment *uc,
                                  const uint8_t *buf, size_t len)
{
    if (len < UTXO_COMMITMENT_SERIALIZED_SIZE) return false;
    memcpy(uc->accumulator, buf, 32);
    uc->count = 0;
    for (int i = 0; i < 8; i++)
        uc->count |= (uint64_t)buf[32 + i] << (8 * i);
    return true;
}

bool utxo_commitment_equal(const struct utxo_commitment *a,
                            const struct utxo_commitment *b)
{
    return a->count == b->count &&
           memcmp(a->accumulator, b->accumulator, 32) == 0;
}

/* ── Checkpoint: full UTXO set verification ──────────────── */

void utxo_commitment_compute_db(sqlite3 *db, struct utxo_commitment *out)
{
    utxo_commitment_init(out);
    if (!db) return;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid, vout, value, height FROM utxos "
            "ORDER BY txid, vout", -1, &s, NULL) != SQLITE_OK)
        return;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *txid = (const uint8_t *)sqlite3_column_blob(s, 0);
        if (!txid || sqlite3_column_bytes(s, 0) < 32) continue;
        uint32_t vout = (uint32_t)sqlite3_column_int(s, 1);
        int64_t value = sqlite3_column_int64(s, 2);
        int32_t height = sqlite3_column_int(s, 3);

        uint8_t h[32];
        /* Inline hash_utxo to avoid static function scope issue */
        uint8_t buf[48];
        memcpy(buf, txid, 32);
        buf[32] = (uint8_t)(vout & 0xFF);
        buf[33] = (uint8_t)((vout >> 8) & 0xFF);
        buf[34] = (uint8_t)((vout >> 16) & 0xFF);
        buf[35] = (uint8_t)((vout >> 24) & 0xFF);
        uint64_t v = (uint64_t)value;
        for (int i = 0; i < 8; i++)
            buf[36 + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
        uint32_t ht = (uint32_t)height;
        buf[44] = (uint8_t)(ht & 0xFF);
        buf[45] = (uint8_t)((ht >> 8) & 0xFF);
        buf[46] = (uint8_t)((ht >> 16) & 0xFF);
        buf[47] = (uint8_t)((ht >> 24) & 0xFF);
        struct sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_write(&ctx, buf, 48);
        sha256_finalize(&ctx, h);

        xor32(out->accumulator, h);
        out->count++;
    }
    sqlite3_finalize(s);
}

bool utxo_commitment_verify_db(sqlite3 *db,
                                const struct utxo_commitment *expected)
{
    if (!db || !expected) return false;
    struct utxo_commitment computed;
    utxo_commitment_compute_db(db, &computed);
    bool ok = utxo_commitment_equal(&computed, expected);
    if (!ok) {
        fprintf(stderr, "UTXO commitment mismatch: expected count=%lu, "
                "got count=%lu\n",
                (unsigned long)expected->count,
                (unsigned long)computed.count);
    }
    return ok;
}

bool utxo_commitment_save_checkpoint(sqlite3 *db,
                                      const struct utxo_commitment *uc)
{
    if (!db || !uc) return false;
    uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
    utxo_commitment_serialize(uc, buf);

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('utxo_commitment',?)", -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, buf, UTXO_COMMITMENT_SERIALIZED_SIZE, SQLITE_STATIC);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

bool utxo_commitment_load_checkpoint(sqlite3 *db,
                                      struct utxo_commitment *uc)
{
    if (!db || !uc) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM node_state WHERE key='utxo_commitment'",
            -1, &s, NULL) != SQLITE_OK)
        return false;

    bool ok = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(s, 0);
        int len = sqlite3_column_bytes(s, 0);
        if (blob && len >= UTXO_COMMITMENT_SERIALIZED_SIZE)
            ok = utxo_commitment_deserialize(uc, blob, (size_t)len);
    }
    sqlite3_finalize(s);
    return ok;
}

/* ── SHA3-256 full-set commitment ────────────────────────── */

void utxo_commitment_sha3_compute(sqlite3 *db, uint8_t out[32],
                                   uint64_t *utxo_count)
{
    memset(out, 0, 32);
    if (utxo_count) *utxo_count = 0;
    if (!db) return;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid, vout, value, script, height, is_coinbase"
            " FROM utxos ORDER BY txid, vout",
            -1, &s, NULL) != SQLITE_OK)
        return;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint64_t count = 0;

    while (sqlite3_step(s) == SQLITE_ROW) {
        const uint8_t *txid = (const uint8_t *)sqlite3_column_blob(s, 0);
        if (!txid || sqlite3_column_bytes(s, 0) < 32) continue;

        uint32_t vout = (uint32_t)sqlite3_column_int(s, 1);
        int64_t value = sqlite3_column_int64(s, 2);
        const uint8_t *script = (const uint8_t *)sqlite3_column_blob(s, 3);
        int script_len = sqlite3_column_bytes(s, 3);
        int32_t height = sqlite3_column_int(s, 4);
        int is_coinbase = sqlite3_column_int(s, 5);

        /* Serialize: txid(32) || vout_le(4) || value_le(8) ||
         *            script_len_le(4) || script(var) ||
         *            height_le(4) || is_coinbase(1) */
        sha3_256_write(&ctx, txid, 32);

        uint8_t le4[4];
        le4[0] = (uint8_t)(vout); le4[1] = (uint8_t)(vout >> 8);
        le4[2] = (uint8_t)(vout >> 16); le4[3] = (uint8_t)(vout >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t le8[8];
        uint64_t v = (uint64_t)value;
        for (int i = 0; i < 8; i++) le8[i] = (uint8_t)(v >> (8 * i));
        sha3_256_write(&ctx, le8, 8);

        uint32_t slen = (uint32_t)(script_len > 0 ? script_len : 0);
        le4[0] = (uint8_t)(slen); le4[1] = (uint8_t)(slen >> 8);
        le4[2] = (uint8_t)(slen >> 16); le4[3] = (uint8_t)(slen >> 24);
        sha3_256_write(&ctx, le4, 4);
        if (script && script_len > 0)
            sha3_256_write(&ctx, script, (size_t)script_len);

        uint32_t ht = (uint32_t)height;
        le4[0] = (uint8_t)(ht); le4[1] = (uint8_t)(ht >> 8);
        le4[2] = (uint8_t)(ht >> 16); le4[3] = (uint8_t)(ht >> 24);
        sha3_256_write(&ctx, le4, 4);

        uint8_t cb = (uint8_t)(is_coinbase ? 1 : 0);
        sha3_256_write(&ctx, &cb, 1);

        count++;
    }
    sqlite3_finalize(s);

    sha3_256_finalize(&ctx, out);
    if (utxo_count) *utxo_count = count;
}

bool utxo_commitment_sha3_save(sqlite3 *db, const uint8_t hash[32],
                                int32_t height, uint64_t count)
{
    if (!db) return false;
    uint8_t buf[44];
    memcpy(buf, hash, 32);
    buf[32] = (uint8_t)(height); buf[33] = (uint8_t)(height >> 8);
    buf[34] = (uint8_t)(height >> 16); buf[35] = (uint8_t)(height >> 24);
    for (int i = 0; i < 8; i++) buf[36 + i] = (uint8_t)(count >> (8 * i));

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('utxo_sha3',?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(st, 1, buf, 44, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool utxo_commitment_sha3_load(sqlite3 *db, uint8_t hash[32],
                                int32_t *height, uint64_t *count)
{
    if (!db) return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM node_state WHERE key='utxo_sha3'",
            -1, &st, NULL) != SQLITE_OK)
        return false;

    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const uint8_t *blob = (const uint8_t *)sqlite3_column_blob(st, 0);
        int len = sqlite3_column_bytes(st, 0);
        if (blob && len >= 44) {
            memcpy(hash, blob, 32);
            if (height)
                *height = (int32_t)blob[32] | ((int32_t)blob[33] << 8) |
                          ((int32_t)blob[34] << 16) | ((int32_t)blob[35] << 24);
            if (count) {
                *count = 0;
                for (int i = 0; i < 8; i++)
                    *count |= (uint64_t)blob[36 + i] << (8 * i);
            }
            ok = true;
        }
    }
    sqlite3_finalize(st);
    return ok;
}
