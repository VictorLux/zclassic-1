/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Ed25519 signature verification — pure C23 implementation.
 * Replaces libsodium crypto_sign_verify_detached. */

#ifndef ZCL_CRYPTO_ED25519_H
#define ZCL_CRYPTO_ED25519_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Verify an Ed25519 signature.
 * Returns true if the signature is valid. */
bool ed25519_verify(const uint8_t sig[64],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t pk[32]);

#endif
