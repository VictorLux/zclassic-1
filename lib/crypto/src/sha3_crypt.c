/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SHA3 stream cipher: post-quantum authenticated encryption.
 * Zero new primitives — built entirely from SHA3-256 and HMAC-SHA3-512. */

#include "crypto/sha3_crypt.h"
#include "crypto/sha3.h"
#include "core/random.h"
#include <string.h>

/* Generate 32 bytes of keystream for a given block index. */
static void sha3_keystream_block(const uint8_t key[32],
                                  const uint8_t nonce[32],
                                  uint64_t counter,
                                  uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, key, 32);
    sha3_256_write(&ctx, nonce, 32);
    sha3_256_write(&ctx, (const unsigned char *)&counter, 8);
    sha3_256_finalize(&ctx, out);
}

/* XOR plaintext with SHA3 counter-mode keystream. */
static void sha3_ctr_xor(const uint8_t key[32],
                           const uint8_t nonce[32],
                           const uint8_t *in, uint8_t *out, size_t len)
{
    uint64_t counter = 0;
    size_t offset = 0;

    while (offset < len) {
        uint8_t block[32];
        sha3_keystream_block(key, nonce, counter, block);
        counter++;

        size_t chunk = len - offset;
        if (chunk > 32) chunk = 32;
        for (size_t i = 0; i < chunk; i++)
            out[offset + i] = in[offset + i] ^ block[i];
        offset += chunk;
    }
}

/* Compute HMAC-SHA3-512 authentication tag. */
static void sha3_compute_tag(const uint8_t key[32],
                               const uint8_t nonce[32],
                               const uint8_t *ciphertext, size_t ct_len,
                               uint8_t tag[64])
{
    /* tag = SHA3-512(key || nonce || ciphertext) */
    struct sha3_512_ctx ctx;
    sha3_512_init(&ctx);
    sha3_512_write(&ctx, key, 32);
    sha3_512_write(&ctx, nonce, 32);
    sha3_512_write(&ctx, ciphertext, ct_len);
    sha3_512_finalize(&ctx, tag);
}

size_t sha3_crypt_encrypt(const uint8_t key[32],
                           const uint8_t *plaintext, size_t len,
                           uint8_t *out)
{
    if (!key || !out) return 0;
    if (len == 0) {
        /* Still produce nonce + tag for empty messages */
    }

    /* Generate random nonce */
    uint8_t nonce[32];
    GetRandBytes(nonce, 32);

    /* Write nonce */
    memcpy(out, nonce, 32);

    /* Encrypt plaintext → ciphertext (after nonce + tag) */
    uint8_t *ct = out + SHA3_CRYPT_OVERHEAD;
    if (len > 0)
        sha3_ctr_xor(key, nonce, plaintext, ct, len);

    /* Compute tag over ciphertext and write it */
    uint8_t tag[64];
    sha3_compute_tag(key, nonce, ct, len, tag);
    memcpy(out + 32, tag, 64);

    return len + SHA3_CRYPT_OVERHEAD;
}

size_t sha3_crypt_decrypt(const uint8_t key[32],
                           const uint8_t *input, size_t input_len,
                           uint8_t *out)
{
    if (!key || !input || !out) return 0;
    if (input_len < SHA3_CRYPT_OVERHEAD) return 0;

    size_t ct_len = input_len - SHA3_CRYPT_OVERHEAD;
    const uint8_t *nonce = input;
    const uint8_t *tag = input + 32;
    const uint8_t *ct = input + SHA3_CRYPT_OVERHEAD;

    /* Verify tag BEFORE decryption (fail fast) */
    uint8_t expected_tag[64];
    sha3_compute_tag(key, nonce, ct, ct_len, expected_tag);

    /* Constant-time comparison to prevent timing attacks */
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= tag[i] ^ expected_tag[i];
    if (diff != 0)
        return 0; /* authentication failed */

    /* Decrypt */
    if (ct_len > 0)
        sha3_ctr_xor(key, nonce, ct, out, ct_len);

    return ct_len;
}

void sha3_crypt_derive_key(const uint8_t utxo_root[32],
                            const uint8_t nonce_a[32],
                            const uint8_t nonce_b[32],
                            uint8_t key_out[32])
{
    /* Sort nonces lexicographically for deterministic key derivation.
     * Both peers must derive the same key regardless of who is A vs B. */
    const uint8_t *lo = nonce_a;
    const uint8_t *hi = nonce_b;
    if (memcmp(nonce_a, nonce_b, 32) > 0) {
        lo = nonce_b;
        hi = nonce_a;
    }

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, utxo_root, 32);
    sha3_256_write(&ctx, lo, 32);
    sha3_256_write(&ctx, hi, 32);
    sha3_256_finalize(&ctx, key_out);
}
