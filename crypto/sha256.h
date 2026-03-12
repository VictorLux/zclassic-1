/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CRYPTO_SHA256_H
#define BITCOIN_CRYPTO_SHA256_H

#include <stdint.h>
#include <stdlib.h>

#define SHA256_OUTPUT_SIZE 32
#define SHA256_BLOCK_SIZE 64

struct sha256_ctx {
    uint32_t s[8];
    unsigned char buf[SHA256_BLOCK_SIZE];
    size_t bytes;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_write(struct sha256_ctx *ctx, const unsigned char *data, size_t len);
void sha256_finalize(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE]);
int sha256_finalize_no_padding(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE],
                               int enforce_compression);
void sha256_reset(struct sha256_ctx *ctx);

#endif
