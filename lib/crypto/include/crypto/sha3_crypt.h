/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SHA3 Stream Cipher — post-quantum authenticated encryption.
 *
 * Encryption: SHA3-256 in counter mode generates keystream.
 *   keystream[i] = SHA3-256(key || nonce || counter_i)
 *   ciphertext = plaintext XOR keystream
 *
 * Authentication: HMAC-SHA3-512 over (nonce || ciphertext).
 *
 * Wire format: [32-byte nonce][64-byte tag][ciphertext]
 * Overhead: 96 bytes per message.
 *
 * Quantum security: pure symmetric — 256-bit key = 128-bit post-quantum. */

#ifndef ZCL_SHA3_CRYPT_H
#define ZCL_SHA3_CRYPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SHA3_CRYPT_KEY_SIZE   32
#define SHA3_CRYPT_NONCE_SIZE 32
#define SHA3_CRYPT_TAG_SIZE   64
#define SHA3_CRYPT_OVERHEAD   (SHA3_CRYPT_NONCE_SIZE + SHA3_CRYPT_TAG_SIZE)

/* Encrypt plaintext. out must hold (len + 96) bytes. Returns total size. */
size_t sha3_crypt_encrypt(const uint8_t key[32],
                           const uint8_t *plaintext, size_t len,
                           uint8_t *out);

/* Decrypt. Returns plaintext size, or 0 on auth failure. */
size_t sha3_crypt_decrypt(const uint8_t key[32],
                           const uint8_t *input, size_t input_len,
                           uint8_t *out);

/* Derive shared key from blockchain state + peer nonces. */
void sha3_crypt_derive_key(const uint8_t utxo_root[32],
                            const uint8_t nonce_a[32],
                            const uint8_t nonce_b[32],
                            uint8_t key_out[32]);

#endif
