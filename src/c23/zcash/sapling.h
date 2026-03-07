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

/* RedJubjub signature verification.
 * generator_idx: 5 for SpendingKey (spend_auth_sig), 4 for ValueCommitmentRandomness (binding_sig) */
bool redjubjub_verify(const uint8_t vk_bytes[32],
                       const uint8_t msg[64],
                       const uint8_t sig_rbar[32],
                       const uint8_t sig_sbar[32],
                       int generator_idx);

/* Sapling verification context (accumulates value commitments for balance check) */
struct sapling_verification_ctx {
    struct jub_point bvk; /* accumulated value commitment balance */
};

void sapling_verification_ctx_init(struct sapling_verification_ctx *ctx);

/* Set global verifying keys (call at init before verification) */
struct groth16_vk;
void sapling_set_spend_vk(struct groth16_vk *vk);
void sapling_set_output_vk(struct groth16_vk *vk);

/* Check spend: accumulate cv, verify spend_auth_sig, verify Groth16 proof */
bool sapling_check_spend(struct sapling_verification_ctx *ctx,
                          const uint8_t cv[32],
                          const uint8_t anchor[32],
                          const uint8_t nullifier[32],
                          const uint8_t rk[32],
                          const uint8_t zkproof[192],
                          const uint8_t spend_auth_sig[64],
                          const uint8_t sighash[32]);

/* Check output: accumulate -cv. Groth16 proof TODO. */
bool sapling_check_output(struct sapling_verification_ctx *ctx,
                           const uint8_t cv[32],
                           const uint8_t cm[32],
                           const uint8_t epk[32],
                           const uint8_t zkproof[192]);

/* Final check: verify binding signature matches value balance */
bool sapling_final_check(struct sapling_verification_ctx *ctx,
                          int64_t value_balance,
                          const uint8_t binding_sig[64],
                          const uint8_t sighash[32]);

#endif
