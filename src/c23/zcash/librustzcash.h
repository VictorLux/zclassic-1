/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * C-compatible header for librustzcash Sapling proving FFI. */

#ifndef ZCL_ZCASH_LIBRUSTZCASH_H
#define ZCL_ZCASH_LIBRUSTZCASH_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

void librustzcash_init_zksnark_params(
    const uint8_t *spend_path, size_t spend_path_len,
    const char *spend_hash,
    const uint8_t *output_path, size_t output_path_len,
    const char *output_hash,
    const uint8_t *sprout_path, size_t sprout_path_len,
    const char *sprout_hash);

void *librustzcash_sapling_proving_ctx_init(void);

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

void librustzcash_sapling_proving_ctx_free(void *ctx);

#endif
