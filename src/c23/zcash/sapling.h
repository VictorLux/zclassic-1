/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling key operations — pure C23 implementation.
 * group_hash, key derivation, commitment, nullifier. */

#ifndef ZCL_ZCASH_SAPLING_H
#define ZCL_ZCASH_SAPLING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "zcash/fr.h"

/* Derive a Jubjub point via group_hash:
 * BLAKE2s-256(personalization, GH_FIRST_BLOCK || tag) → decompress → mul_by_cofactor.
 * Returns false if the result is the identity point (invalid). */
bool group_hash(struct jub_point *result,
                const uint8_t *tag, size_t tag_len,
                const uint8_t personalization[8]);

/* Check if a diversifier is valid (i.e., group_hash("Zcash_gd", d) is not identity) */
bool sapling_check_diversifier(const uint8_t diversifier[11]);

/* Compute g_d = GH("Zcash_gd", diversifier) */
bool sapling_diversifier_to_gd(struct jub_point *g_d, const uint8_t diversifier[11]);

/* ask → ak: ak = ask * SpendingKeyGenerator */
void sapling_ask_to_ak(const uint8_t ask[32], uint8_t ak[32]);

/* nsk → nk: nk = nsk * ProofGenerationKey */
void sapling_nsk_to_nk(const uint8_t nsk[32], uint8_t nk[32]);

/* CRH^ivk(ak, nk) = BLAKE2s("Zcashivk", ak || nk) with top 5 bits dropped */
void sapling_crh_ivk(const uint8_t ak[32], const uint8_t nk[32], uint8_t ivk[32]);

/* ivk → pk_d: pk_d = ivk * g_d(diversifier) */
bool sapling_ivk_to_pkd(const uint8_t ivk[32], const uint8_t diversifier[11],
                         uint8_t pk_d[32]);

/* Sapling key agreement: result = [sk] [8] p */
bool sapling_ka_agree(const uint8_t p[32], const uint8_t sk[32], uint8_t result[32]);

/* Derive ephemeral public key: result = [esk] g_d(diversifier) */
bool sapling_ka_derivepublic(const uint8_t diversifier[11], const uint8_t esk[32],
                              uint8_t result[32]);

/* Compute Sapling note commitment cm */
bool sapling_compute_cm(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         uint8_t cm[32]);

/* Compute Sapling nullifier */
bool sapling_compute_nf(const uint8_t diversifier[11], const uint8_t pk_d[32],
                         uint64_t value, const uint8_t rcm[32],
                         const uint8_t ak[32], const uint8_t nk[32],
                         uint64_t position, uint8_t nf[32]);

#endif
