/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZIP 32 Sapling HD key derivation — pure C23 implementation. */

#ifndef ZCL_SAPLING_ZIP32_H
#define ZCL_SAPLING_ZIP32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ZIP32_HARDENED_KEY_LIMIT 0x80000000u

struct zip32_expsk {
    uint8_t ask[32]; /* spending key scalar (Fs, LE) */
    uint8_t nsk[32]; /* nullifier key scalar (Fs, LE) */
    uint8_t ovk[32]; /* outgoing viewing key */
};

struct zip32_fvk {
    uint8_t ak[32];  /* spending verification key (Jubjub compressed) */
    uint8_t nk[32];  /* nullifier verification key (Jubjub compressed) */
    uint8_t ovk[32]; /* outgoing viewing key */
};

struct zip32_xsk {
    uint8_t depth;
    uint32_t parent_fvk_tag;
    uint32_t child_index;
    uint8_t chain_code[32];
    struct zip32_expsk expsk;
    uint8_t dk[32]; /* diversifier key */
};

struct zip32_xfvk {
    uint8_t depth;
    uint32_t parent_fvk_tag;
    uint32_t child_index;
    uint8_t chain_code[32];
    struct zip32_fvk fvk;
    uint8_t dk[32]; /* diversifier key */
};

/* Derive master extended spending key from seed */
void zip32_xsk_master(struct zip32_xsk *xsk,
                      const uint8_t *seed, size_t seed_len);

/* Derive child extended spending key */
void zip32_xsk_derive(struct zip32_xsk *child,
                      const struct zip32_xsk *parent,
                      uint32_t i);

/* Convert extended spending key to extended full viewing key */
void zip32_xsk_to_xfvk(struct zip32_xfvk *xfvk,
                        const struct zip32_xsk *xsk);

/* Derive child from extended full viewing key (non-hardened only, returns false for hardened) */
bool zip32_xfvk_derive(struct zip32_xfvk *child,
                        const struct zip32_xfvk *parent,
                        uint32_t i);

/* Find default diversifier for a given diversifier key */
bool zip32_default_diversifier(const uint8_t dk[32], uint8_t diversifier[11]);

/* Find diversifier starting from index j (LE 11 bytes), returns index found */
bool zip32_diversifier(const uint8_t dk[32],
                       uint8_t j[11], uint8_t diversifier[11]);

/* Derive default payment address (diversifier + pk_d) from xfvk */
bool zip32_xfvk_address(const struct zip32_xfvk *xfvk,
                         uint8_t diversifier[11], uint8_t pk_d[32]);

#endif
