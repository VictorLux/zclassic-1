/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_KEY_H
#define ZCL_KEY_H

#include "keys/pubkey.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stdint.h>

#define PRIVATE_KEY_SIZE 279
#define COMPRESSED_PRIVATE_KEY_SIZE 214

struct privkey {
    unsigned char vch[32];
    bool fValid;
    bool fCompressed;
};

struct ext_key {
    unsigned char nDepth;
    unsigned char vchFingerprint[4];
    unsigned int nChild;
    struct uint256 chaincode;
    struct privkey key;
};

static inline void privkey_init(struct privkey *k)
{
    memset(k, 0, sizeof(*k));
}

static inline bool privkey_is_valid(const struct privkey *k)
{
    return k->fValid;
}

static inline bool privkey_is_compressed(const struct privkey *k)
{
    return k->fCompressed;
}

void privkey_make_new(struct privkey *k, bool fCompressed);
bool privkey_get_pubkey(const struct privkey *k, struct pubkey *pk);
bool privkey_sign(const struct privkey *k, const struct uint256 *hash,
                  unsigned char *sig, size_t *siglen);
bool privkey_sign_compact(const struct privkey *k, const struct uint256 *hash,
                          unsigned char sig[COMPACT_SIGNATURE_SIZE]);
bool privkey_verify_pubkey(const struct privkey *k, const struct pubkey *pk);
bool privkey_derive(const struct privkey *k, struct privkey *child,
                    struct uint256 *cc_child, unsigned int nChild,
                    const struct uint256 *cc);

void ext_key_encode(const struct ext_key *ek,
                    unsigned char code[BIP32_EXTKEY_SIZE]);
void ext_key_decode(struct ext_key *ek,
                    const unsigned char code[BIP32_EXTKEY_SIZE]);
bool ext_key_derive(const struct ext_key *ek, struct ext_key *out,
                    unsigned int nChild);
void ext_key_set_master(struct ext_key *ek, const unsigned char *seed,
                        unsigned int nSeedLen);
void ext_key_neuter(const struct ext_key *ek, struct ext_pubkey *epk);

bool ecc_init_sanity_check(void);
void ecc_start(void);
void ecc_stop(void);

#endif
