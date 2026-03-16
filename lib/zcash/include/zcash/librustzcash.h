/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure C23 API for Sapling proving and verification.
 * Drop-in replacement for the Rust librustzcash FFI.
 * All functions implemented in lib/zcash/src/librustzcash_c23.c */

#ifndef ZCL_ZCASH_LIBRUSTZCASH_H
#define ZCL_ZCASH_LIBRUSTZCASH_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Parameter loading (no-op — VKs loaded by zcash_init_params) */
void librustzcash_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash);

/* Verification context */
void *librustzcash_sapling_verification_ctx_init(void);
void librustzcash_sapling_verification_ctx_free(void *ctx);

bool librustzcash_sapling_check_spend(
    void *ctx, const uint8_t *cv, const uint8_t *anchor,
    const uint8_t *nullifier, const uint8_t *rk,
    const uint8_t *zkproof, const uint8_t *spend_auth_sig,
    const uint8_t *sighash_value);

bool librustzcash_sapling_check_output(
    void *ctx, const uint8_t *cv, const uint8_t *cm,
    const uint8_t *epk, const uint8_t *zkproof);

bool librustzcash_sapling_final_check(
    void *ctx, int64_t value_balance,
    const uint8_t *binding_sig, const uint8_t *sighash_value);

/* Proving context */
void *librustzcash_sapling_proving_ctx_init(void);
void librustzcash_sapling_proving_ctx_free(void *ctx);

bool librustzcash_sapling_output_proof(
    void *ctx,
    const unsigned char *esk,
    const unsigned char *diversifier,
    const unsigned char *pk_d,
    const unsigned char *rcm,
    const uint64_t value,
    unsigned char *cv,
    unsigned char *zkproof);

bool librustzcash_sapling_spend_proof(
    void *ctx,
    const unsigned char *ak,
    const unsigned char *nsk,
    const unsigned char *diversifier,
    const unsigned char *rcm,
    const unsigned char *ar,
    const uint64_t value,
    const unsigned char *anchor,
    const unsigned char *witness,
    unsigned char *cv,
    unsigned char *rk,
    unsigned char *zkproof);

bool librustzcash_sapling_binding_sig(
    const void *ctx,
    int64_t valueBalance,
    const unsigned char *sighash,
    unsigned char *result);

#endif
