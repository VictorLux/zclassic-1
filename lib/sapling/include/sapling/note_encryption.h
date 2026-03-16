/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Note encryption for Sprout and Sapling shielded transactions.
 * Sprout: Curve25519 DH + BLAKE2b KDF + ChaCha20-Poly1305 AEAD.
 * Sapling: Jubjub DH + BLAKE2b KDF + ChaCha20-Poly1305 AEAD. */

#ifndef ZCL_SAPLING_NOTE_ENCRYPTION_H
#define ZCL_SAPLING_NOTE_ENCRYPTION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sapling/constants.h"

/* Sprout KDF: BLAKE2b-256("ZcashKDF" || nonce, hSig || dhsecret || epk || pk_enc) */
bool sprout_kdf(uint8_t key[32],
                const uint8_t hsig[32],
                const uint8_t dhsecret[32],
                const uint8_t epk[32],
                const uint8_t pk_enc[32],
                uint8_t nonce);

/* Sapling KDF: BLAKE2b-256("Zcash_SaplingKDF", dhsecret || epk) */
bool sapling_kdf(uint8_t key[32],
                 const uint8_t dhsecret[32],
                 const uint8_t epk[32]);

/* Sapling outgoing cipher key: BLAKE2b-256("Zcash_Derive_ock", ovk || cv || cm || epk) */
bool sapling_prf_ock(uint8_t key[32],
                     const uint8_t ovk[32],
                     const uint8_t cv[32],
                     const uint8_t cm[32],
                     const uint8_t epk[32]);

/* Sprout note encryption context */
struct sprout_note_encryption {
    uint8_t esk[32];
    uint8_t epk[32];
    uint8_t nonce;
};

/* Initialize with random ephemeral key */
bool sprout_note_encryption_init(struct sprout_note_encryption *ctx);

/* Initialize with specific ephemeral key (for testing) */
void sprout_note_encryption_init_with_esk(struct sprout_note_encryption *ctx,
                                           const uint8_t esk[32]);

/* Encrypt a Sprout note plaintext.
 * plaintext: ZC_NOTEPLAINTEXT_SIZE bytes
 * ciphertext: ZC_NOTEPLAINTEXT_SIZE + NOTEENCRYPTION_AUTH_BYTES bytes
 * Returns false on failure. */
bool sprout_note_encrypt(struct sprout_note_encryption *ctx,
                         const uint8_t hsig[32],
                         const uint8_t pk_enc[32],
                         const uint8_t *plaintext, size_t plen,
                         uint8_t *ciphertext);

/* Decrypt a Sprout note ciphertext.
 * sk_enc: recipient's secret encryption key (clamped Curve25519 scalar)
 * ciphertext: plen + NOTEENCRYPTION_AUTH_BYTES bytes
 * plaintext: plen bytes output */
bool sprout_note_decrypt(const uint8_t sk_enc[32],
                         const uint8_t epk[32],
                         const uint8_t hsig[32],
                         const uint8_t pk_enc[32],
                         uint8_t nonce,
                         const uint8_t *ciphertext, size_t clen,
                         uint8_t *plaintext);

/* Sapling note encryption.
 * key: pre-derived symmetric key from sapling_kdf
 * plaintext: ZC_SAPLING_ENCCIPHERTEXT_SIZE - NOTEENCRYPTION_AUTH_BYTES bytes
 * ciphertext: ZC_SAPLING_ENCCIPHERTEXT_SIZE bytes */
bool sapling_note_encrypt(const uint8_t key[32],
                          const uint8_t *plaintext, size_t plen,
                          uint8_t *ciphertext);

bool sapling_note_decrypt(const uint8_t key[32],
                          const uint8_t *ciphertext, size_t clen,
                          uint8_t *plaintext);

/* Sapling outgoing ciphertext encryption.
 * key: pre-derived from sapling_prf_ock
 * plaintext: ZC_SAPLING_OUTCIPHERTEXT_SIZE - NOTEENCRYPTION_AUTH_BYTES bytes (64)
 * ciphertext: ZC_SAPLING_OUTCIPHERTEXT_SIZE bytes (80) */
bool sapling_out_encrypt(const uint8_t key[32],
                         const uint8_t *plaintext, size_t plen,
                         uint8_t *ciphertext);

bool sapling_out_decrypt(const uint8_t key[32],
                         const uint8_t *ciphertext, size_t clen,
                         uint8_t *plaintext);

#endif
