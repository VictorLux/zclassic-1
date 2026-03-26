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
#include <string.h>
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
