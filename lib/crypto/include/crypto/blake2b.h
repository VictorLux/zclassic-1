/* BLAKE2b implementation in pure C23.
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Based on RFC 7693 and the BLAKE2 reference implementation. */

#ifndef BITCOIN_CRYPTO_BLAKE2B_H
#define BITCOIN_CRYPTO_BLAKE2B_H

#include <stdint.h>
#include <stdlib.h>

#define BLAKE2B_BLOCKBYTES 128
#define BLAKE2B_OUTBYTES   64
#define BLAKE2B_KEYBYTES   64
#define BLAKE2B_SALTBYTES  16
#define BLAKE2B_PERSONALBYTES 16

struct blake2b_param {
    uint8_t digest_length;
    uint8_t key_length;
    uint8_t fanout;
    uint8_t depth;
    uint32_t leaf_length;
    uint64_t node_offset;
    uint8_t node_depth;
    uint8_t inner_length;
    uint8_t reserved[14];
    uint8_t salt[BLAKE2B_SALTBYTES];
    uint8_t personal[BLAKE2B_PERSONALBYTES];
};

struct blake2b_ctx {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t buf[BLAKE2B_BLOCKBYTES];
    size_t buflen;
    uint8_t outlen;
};

int blake2b_init(struct blake2b_ctx *ctx, size_t outlen);
int blake2b_init_key(struct blake2b_ctx *ctx, size_t outlen,
                     const void *key, size_t keylen);
int blake2b_init_param(struct blake2b_ctx *ctx, const struct blake2b_param *p);
int blake2b_init_salt_personal(struct blake2b_ctx *ctx, size_t outlen,
                               const void *key, size_t keylen,
                               const uint8_t salt[BLAKE2B_SALTBYTES],
                               const uint8_t personal[BLAKE2B_PERSONALBYTES]);
int blake2b_update(struct blake2b_ctx *ctx, const void *in, size_t inlen);
int blake2b_final(struct blake2b_ctx *ctx, void *out, size_t outlen);

int blake2b(void *out, size_t outlen, const void *in, size_t inlen,
            const void *key, size_t keylen);

/* Vectorized BLAKE2b for Equihash batch hashing.
 * 3-tier dispatch: AVX-512 (8-way) -> AVX2 (4-way) -> scalar */
void equihash_generate_hash_batch4(
    const struct blake2b_ctx *base_state,
    const uint32_t indices[4],
    unsigned char *hash0, unsigned char *hash1,
    unsigned char *hash2, unsigned char *hash3,
    size_t hash_len);
void equihash_generate_hash_batch8(
    const struct blake2b_ctx *base_state,
    const uint32_t indices[8],
    unsigned char *hashes[8],
    size_t hash_len);

#endif
