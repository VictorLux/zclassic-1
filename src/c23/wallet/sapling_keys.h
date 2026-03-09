/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_WALLET_SAPLING_KEYS_H
#define ZCL_WALLET_SAPLING_KEYS_H

#include "zcash/zip32.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SAPLING_KEYS 256
#define ZC_DIVERSIFIER_SIZE 11
#define ZC_MEMO_SIZE 512

struct sapling_key_entry {
    struct zip32_xsk xsk;
    struct zip32_xfvk xfvk;
    uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
    uint8_t pk_d[32];
    uint8_t ivk[32];
    uint32_t child_index;
    bool used;
};

struct sapling_keystore {
    zcl_mutex_t cs;
    uint8_t seed[32];
    bool has_seed;
    struct zip32_xsk master_xsk;
    uint32_t next_child_index;
    struct sapling_key_entry keys[MAX_SAPLING_KEYS];
    size_t num_keys;
};

void sapling_keystore_init(struct sapling_keystore *sks);
void sapling_keystore_free(struct sapling_keystore *sks);

bool sapling_keystore_generate_seed(struct sapling_keystore *sks);
bool sapling_keystore_set_seed(struct sapling_keystore *sks,
                                const uint8_t seed[32]);

bool sapling_keystore_new_address(struct sapling_keystore *sks,
                                   uint8_t diversifier_out[ZC_DIVERSIFIER_SIZE],
                                   uint8_t pk_d_out[32]);

bool sapling_encode_payment_address(const uint8_t diversifier[ZC_DIVERSIFIER_SIZE],
                                     const uint8_t pk_d[32],
                                     const char *hrp,
                                     char *out, size_t out_size);

bool sapling_decode_payment_address(const char *str,
                                     uint8_t diversifier_out[ZC_DIVERSIFIER_SIZE],
                                     uint8_t pk_d_out[32]);

bool sapling_keystore_have_spending_key(const struct sapling_keystore *sks,
                                         const uint8_t ivk[32]);

const struct sapling_key_entry *sapling_keystore_find_by_ivk(
    const struct sapling_keystore *sks, const uint8_t ivk[32]);

#endif
