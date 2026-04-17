/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Note encryption for Sprout and Sapling shielded transactions. */

#include "sapling/note_encryption.h"
#include "crypto/blake2b.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/curve25519.h"
#include "core/random.h"
#include "util/log_macros.h"
#include <string.h>

#define BLAKE2B_PERSONALBYTES 16

static const uint8_t zero_nonce[12] = {0};

bool sprout_kdf(uint8_t key[32],
                const uint8_t hsig[32],
                const uint8_t dhsecret[32],
                const uint8_t epk[32],
                const uint8_t pk_enc[32],
                uint8_t nonce)
{
    uint8_t block[128];
    memcpy(block, hsig, 32);
    memcpy(block + 32, dhsecret, 32);
    memcpy(block + 64, epk, 32);
    memcpy(block + 96, pk_enc, 32);

    uint8_t personal[BLAKE2B_PERSONALBYTES];
    memcpy(personal, "ZcashKDF", 8);
    memset(personal + 8, 0, 7);
    personal[15] = nonce;

    struct blake2b_ctx ctx;
    if (blake2b_init_salt_personal(&ctx, 32, NULL, 0, NULL, personal) != 0)
        LOG_FAIL("sapling", "sprout_kdf: blake2b_init_salt_personal failed (nonce=%u)",
                 (unsigned)nonce);
    blake2b_update(&ctx, block, 128);
    blake2b_final(&ctx, key, 32);
    return true;
}

bool sapling_kdf(uint8_t key[32],
                 const uint8_t dhsecret[32],
                 const uint8_t epk[32])
{
    uint8_t block[64];
    memcpy(block, dhsecret, 32);
    memcpy(block + 32, epk, 32);

    uint8_t personal[BLAKE2B_PERSONALBYTES];
    memcpy(personal, "Zcash_SaplingKDF", 16);

    struct blake2b_ctx ctx;
    if (blake2b_init_salt_personal(&ctx, 32, NULL, 0, NULL, personal) != 0)
        LOG_FAIL("sapling", "sapling_kdf: blake2b_init_salt_personal failed");
    blake2b_update(&ctx, block, 64);
    blake2b_final(&ctx, key, 32);
    return true;
}

bool sapling_prf_ock(uint8_t key[32],
                     const uint8_t ovk[32],
                     const uint8_t cv[32],
                     const uint8_t cm[32],
                     const uint8_t epk[32])
{
    uint8_t block[128];
    memcpy(block, ovk, 32);
    memcpy(block + 32, cv, 32);
    memcpy(block + 64, cm, 32);
    memcpy(block + 96, epk, 32);

    uint8_t personal[BLAKE2B_PERSONALBYTES];
    memcpy(personal, "Zcash_Derive_ock", 16);

    struct blake2b_ctx ctx;
    if (blake2b_init_salt_personal(&ctx, 32, NULL, 0, NULL, personal) != 0)
        LOG_FAIL("sapling", "sapling_prf_ock: blake2b_init_salt_personal failed");
    blake2b_update(&ctx, block, 128);
    blake2b_final(&ctx, key, 32);
    return true;
}

bool sprout_note_encryption_init(struct sprout_note_encryption *ctx)
{
    GetRandBytes(ctx->esk, 32);
    ctx->esk[0] &= 248;
    ctx->esk[31] &= 127;
    ctx->esk[31] |= 64;
    curve25519_scalarmult_base(ctx->epk, ctx->esk);
    ctx->nonce = 0;
    return true;
}

void sprout_note_encryption_init_with_esk(struct sprout_note_encryption *ctx,
                                           const uint8_t esk[32])
{
    memcpy(ctx->esk, esk, 32);
    ctx->esk[0] &= 248;
    ctx->esk[31] &= 127;
    ctx->esk[31] |= 64;
    curve25519_scalarmult_base(ctx->epk, ctx->esk);
    ctx->nonce = 0;
}

bool sprout_note_encrypt(struct sprout_note_encryption *ctx,
                         const uint8_t hsig[32],
                         const uint8_t pk_enc[32],
                         const uint8_t *plaintext, size_t plen,
                         uint8_t *ciphertext)
{
    if (ctx->nonce > 254)
        LOG_FAIL("sapling",
                 "sprout_note_encrypt: per-tx nonce budget exhausted (nonce=%u > 254)",
                 (unsigned)ctx->nonce);

    uint8_t dhsecret[32];
    curve25519_scalarmult(dhsecret, ctx->esk, pk_enc);

    uint8_t key[32];
    if (!sprout_kdf(key, hsig, dhsecret, ctx->epk, pk_enc, ctx->nonce)) {
        memset(dhsecret, 0, 32);
        LOG_FAIL("sapling", "sprout_note_encrypt: sprout_kdf failed (nonce=%u)",
                 (unsigned)ctx->nonce);
    }

    ctx->nonce++;

    bool ok = chacha20poly1305_encrypt(plaintext, plen, NULL, 0,
                                        zero_nonce, key, ciphertext);
    memset(dhsecret, 0, 32);
    memset(key, 0, 32);
    return ok;
}

bool sprout_note_decrypt(const uint8_t sk_enc[32],
                         const uint8_t epk[32],
                         const uint8_t hsig[32],
                         const uint8_t pk_enc[32],
                         uint8_t nonce,
                         const uint8_t *ciphertext, size_t clen,
                         uint8_t *plaintext)
{
    uint8_t dhsecret[32];
    curve25519_scalarmult(dhsecret, sk_enc, epk);

    uint8_t key[32];
    if (!sprout_kdf(key, hsig, dhsecret, epk, pk_enc, nonce)) {
        memset(dhsecret, 0, 32);
        LOG_FAIL("sapling", "sprout_note_decrypt: sprout_kdf failed (nonce=%u)",
                 (unsigned)nonce);
    }

    bool ok = chacha20poly1305_decrypt(ciphertext, clen, NULL, 0,
                                        zero_nonce, key, plaintext);
    memset(dhsecret, 0, 32);
    memset(key, 0, 32);
    return ok;
}

bool sapling_note_encrypt(const uint8_t key[32],
                          const uint8_t *plaintext, size_t plen,
                          uint8_t *ciphertext)
{
    return chacha20poly1305_encrypt(plaintext, plen, NULL, 0,
                                    zero_nonce, key, ciphertext);
}

bool sapling_note_decrypt(const uint8_t key[32],
                          const uint8_t *ciphertext, size_t clen,
                          uint8_t *plaintext)
{
    return chacha20poly1305_decrypt(ciphertext, clen, NULL, 0,
                                    zero_nonce, key, plaintext);
}

bool sapling_out_encrypt(const uint8_t key[32],
                         const uint8_t *plaintext, size_t plen,
                         uint8_t *ciphertext)
{
    return chacha20poly1305_encrypt(plaintext, plen, NULL, 0,
                                    zero_nonce, key, ciphertext);
}

bool sapling_out_decrypt(const uint8_t key[32],
                         const uint8_t *ciphertext, size_t clen,
                         uint8_t *plaintext)
{
    return chacha20poly1305_decrypt(ciphertext, clen, NULL, 0,
                                    zero_nonce, key, plaintext);
}
