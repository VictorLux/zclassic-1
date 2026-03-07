/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Zcash shielded address types — Sprout and Sapling. */

#ifndef ZCL_ZCASH_ADDRESS_H
#define ZCL_ZCASH_ADDRESS_H

#include "core/uint256.h"
#include "zcash/zcash.h"
#include <stdbool.h>
#include <stddef.h>

struct sprout_payment_address {
    struct uint256 a_pk;
    struct uint256 pk_enc;
};

struct sprout_viewing_key {
    struct uint256 a_pk;
    struct uint256 sk_enc;
};

struct sprout_spending_key {
    unsigned char data[32];
};

struct sapling_payment_address {
    unsigned char d[ZC_DIVERSIFIER_SIZE];
    struct uint256 pk_d;
};

struct sapling_incoming_viewing_key {
    struct uint256 ivk;
};

struct sapling_full_viewing_key {
    struct uint256 ak;
    struct uint256 nk;
    struct uint256 ovk;
};

struct sapling_expanded_spending_key {
    struct uint256 ask;
    struct uint256 nsk;
    struct uint256 ovk;
};

struct sapling_spending_key {
    struct uint256 sk;
};

enum payment_address_type {
    ADDR_INVALID = 0,
    ADDR_SPROUT = 1,
    ADDR_SAPLING = 2
};

struct payment_address {
    enum payment_address_type type;
    union {
        struct sprout_payment_address sprout;
        struct sapling_payment_address sapling;
    };
};

void sprout_payment_address_get_hash(const struct sprout_payment_address *addr,
                                      struct uint256 *out);
void sapling_payment_address_get_hash(const struct sapling_payment_address *addr,
                                       struct uint256 *out);

void sprout_spending_key_to_viewing_key(const struct sprout_spending_key *sk,
                                         struct sprout_viewing_key *vk);
void sprout_viewing_key_to_address(const struct sprout_viewing_key *vk,
                                    struct sprout_payment_address *addr);
void sprout_spending_key_to_address(const struct sprout_spending_key *sk,
                                     struct sprout_payment_address *addr);

void sapling_spending_key_to_expanded(const struct sapling_spending_key *sk,
                                       struct sapling_expanded_spending_key *esk);

struct byte_stream;
bool sprout_payment_address_serialize(const struct sprout_payment_address *addr,
                                       struct byte_stream *s);
bool sprout_payment_address_deserialize(struct sprout_payment_address *addr,
                                         struct byte_stream *s);
bool sapling_payment_address_serialize(const struct sapling_payment_address *addr,
                                        struct byte_stream *s);
bool sapling_payment_address_deserialize(struct sapling_payment_address *addr,
                                          struct byte_stream *s);

#endif
