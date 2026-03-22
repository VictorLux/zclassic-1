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
