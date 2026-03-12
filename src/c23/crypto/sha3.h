/* Copyright (c) 2020 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CRYPTO_SHA3_H
#define BITCOIN_CRYPTO_SHA3_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#define SHA3_256_OUTPUT_SIZE 32
#define SHA3_256_RATE_BITS 1088
#define SHA3_256_RATE_BUFFERS (SHA3_256_RATE_BITS / 64)

#define SHA3_512_OUTPUT_SIZE 64
#define SHA3_512_RATE_BITS 576
#define SHA3_512_RATE_BUFFERS (SHA3_512_RATE_BITS / 64)

struct sha3_256_ctx {
    uint64_t state[25];
    unsigned char buffer[8];
    unsigned bufsize;
    unsigned pos;
};

struct sha3_512_ctx {
    uint64_t state[25];
    unsigned char buffer[8];
    unsigned bufsize;
    unsigned pos;
};

void keccakf(uint64_t st[25]);

void sha3_256_init(struct sha3_256_ctx *ctx);
void sha3_256_write(struct sha3_256_ctx *ctx, const unsigned char *data, size_t len);
void sha3_256_finalize(struct sha3_256_ctx *ctx, unsigned char *output);
void sha3_256_reset(struct sha3_256_ctx *ctx);

void sha3_512_init(struct sha3_512_ctx *ctx);
void sha3_512_write(struct sha3_512_ctx *ctx, const unsigned char *data, size_t len);
void sha3_512_finalize(struct sha3_512_ctx *ctx, unsigned char output[64]);
void sha3_512_reset(struct sha3_512_ctx *ctx);

/* One-shot convenience */
void sha3_512(const unsigned char *data, size_t len, unsigned char output[64]);

/* HMAC-SHA3-512 for keyed authentication */
void hmac_sha3_512(const unsigned char *key, size_t key_len,
                   const unsigned char *data, size_t data_len,
                   unsigned char output[64]);

#endif
