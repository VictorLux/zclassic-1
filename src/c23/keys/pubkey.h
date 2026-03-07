/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_PUBKEY_H
#define ZCL_PUBKEY_H

#include "core/hash.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PUBLIC_KEY_SIZE 65
#define COMPRESSED_PUBLIC_KEY_SIZE 33
#define SIGNATURE_SIZE 72
#define COMPACT_SIGNATURE_SIZE 65
#define BIP32_EXTKEY_SIZE 74

struct key_id {
    struct uint160 id;
};

struct pubkey {
    unsigned char vch[PUBLIC_KEY_SIZE];
    unsigned int size;
};

struct ext_pubkey {
    unsigned char nDepth;
    unsigned char vchFingerprint[4];
    unsigned int nChild;
    struct uint256 chaincode;
    struct pubkey pubkey;
};

static inline void pubkey_init(struct pubkey *pk)
{
    memset(pk, 0, sizeof(*pk));
}

static inline bool pubkey_is_valid(const struct pubkey *pk)
{
    return pk->size > 0;
}

static inline bool pubkey_is_compressed(const struct pubkey *pk)
{
    return pk->size == COMPRESSED_PUBLIC_KEY_SIZE;
}

static inline void pubkey_set(struct pubkey *pk,
                              const unsigned char *data, unsigned int len)
{
    if (len > PUBLIC_KEY_SIZE) len = PUBLIC_KEY_SIZE;
    memcpy(pk->vch, data, len);
    pk->size = len;
}

static inline struct key_id pubkey_get_id(const struct pubkey *pk)
{
    struct key_id kid;
    hash160(pk->vch, pk->size, kid.id.data);
    return kid;
}

bool pubkey_verify(const struct pubkey *pk, const struct uint256 *hash,
                   const unsigned char *sig, size_t siglen);

bool pubkey_recover_compact(struct pubkey *pk, const struct uint256 *hash,
                            const unsigned char sig[COMPACT_SIGNATURE_SIZE]);

bool pubkey_is_fully_valid(const struct pubkey *pk);

bool pubkey_decompress(struct pubkey *pk);

bool pubkey_derive(const struct pubkey *pk, struct pubkey *child,
                   struct uint256 *cc_child, unsigned int nChild,
                   const struct uint256 *cc);

bool pubkey_check_low_s(const unsigned char *sig, size_t siglen);

void ext_pubkey_encode(const struct ext_pubkey *epk,
                       unsigned char code[BIP32_EXTKEY_SIZE]);
void ext_pubkey_decode(struct ext_pubkey *epk,
                       const unsigned char code[BIP32_EXTKEY_SIZE]);
bool ext_pubkey_derive(const struct ext_pubkey *epk,
                       struct ext_pubkey *out, unsigned int nChild);

void ecc_verify_init(void);
void ecc_verify_destroy(void);

#endif
