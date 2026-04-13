/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "net/secure_channel.h"
#include "crypto/sha3.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/curve25519.h"
#include "core/random.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include <string.h>

/* Overhead per encrypted message: 64-byte HMAC + 16-byte Poly1305 tag */
#define SECURE_OVERHEAD 80

bool secure_channel_init(struct secure_channel *ch)
{
    memset(ch, 0, sizeof(*ch));

    /* Generate ephemeral Curve25519 keypair */
    uint8_t privkey[32];
    GetRandBytes(privkey, 32);

    /* Curve25519 key clamping */
    privkey[0] &= 248;
    privkey[31] &= 127;
    privkey[31] |= 64;

    /* Compute public key: pubkey = privkey * basepoint */
    if (!curve25519_scalarmult_base(ch->local_pubkey, privkey)) {
        memory_cleanse(privkey, 32);
        LOG_FAIL("secure_chan", "curve25519 base scalarmult failed");
    }

    /* Store private key temporarily in mac_key (will be overwritten
     * when establish is called). This avoids a separate field. */
    memcpy(ch->mac_key, privkey, 32);
    memory_cleanse(privkey, 32);

    ch->send_counter = 0;
    ch->recv_counter = 0;
    ch->established = false;
    return true;
}

bool secure_channel_establish(struct secure_channel *ch,
                              const uint8_t remote_pubkey[32])
{
    memcpy(ch->remote_pubkey, remote_pubkey, 32);

    /* Recover ephemeral private key from mac_key (stored by init) */
    uint8_t privkey[32];
    memcpy(privkey, ch->mac_key, 32);

    /* Curve25519 Diffie-Hellman: shared = privkey * remote_pubkey */
    uint8_t shared_secret[32];
    if (!curve25519_scalarmult(shared_secret, privkey, remote_pubkey)) {
        memory_cleanse(privkey, 32);
        memory_cleanse(shared_secret, 32);
        LOG_FAIL("secure_chan", "curve25519 DH scalarmult failed");
    }
    memory_cleanse(privkey, 32);

    /* Derive keys via SHA3-512(shared_secret || min_pub || max_pub).
     * Pubkeys sorted lexicographically so both sides get the same hash. */
    const uint8_t *pub_lo, *pub_hi;
    if (memcmp(ch->local_pubkey, ch->remote_pubkey, 32) < 0) {
        pub_lo = ch->local_pubkey;
        pub_hi = ch->remote_pubkey;
    } else {
        pub_lo = ch->remote_pubkey;
        pub_hi = ch->local_pubkey;
    }

    struct sha3_512_ctx ctx;
    sha3_512_init(&ctx);
    sha3_512_write(&ctx, shared_secret, 32);
    sha3_512_write(&ctx, pub_lo, 32);
    sha3_512_write(&ctx, pub_hi, 32);

    uint8_t derived[64];
    sha3_512_finalize(&ctx, derived);
    memory_cleanse(shared_secret, 32);

    /* First 32 bytes = ChaCha20-Poly1305 encryption key */
    memcpy(ch->enc_key, derived, 32);

    /* Derive a separate 64-byte HMAC key via second SHA3-512 pass.
     * SHA3-512(derived) gives us a full-width HMAC key. */
    sha3_512(derived, 64, ch->mac_key);
    memory_cleanse(derived, 64);

    ch->established = true;
    return true;
}

/* Build 12-byte nonce from 8-byte counter */
static void counter_to_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    /* LE counter in first 8 bytes, last 4 bytes zero */
    for (int i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(counter >> (8 * i));
}

bool secure_channel_encrypt(struct secure_channel *ch,
                            const uint8_t *plaintext, size_t len,
                            uint8_t *out, size_t *out_len)
{
    if (!ch->established)
        LOG_FAIL("secure_chan", "encrypt: channel not established");

    /* Layout: [64-byte HMAC][16-byte poly1305 tag + ciphertext] */
    uint8_t *aead_out = out + 64; /* space for HMAC prefix */

    uint8_t nonce[12];
    counter_to_nonce(ch->send_counter++, nonce);

    /* ChaCha20-Poly1305 AEAD encrypt (appends 16-byte tag) */
    if (!chacha20poly1305_encrypt(plaintext, len,
                                  NULL, 0, /* no AAD */
                                  nonce, ch->enc_key,
                                  aead_out)) {
        LOG_FAIL("secure_chan", "chacha20poly1305 encrypt failed (len=%zu)", len);
    }
    size_t aead_len = len + 16; /* ciphertext + tag */

    /* HMAC-SHA3-512 over (nonce || aead_output) for integrity */
    struct sha3_512_ctx hctx;
    sha3_512_init(&hctx);
    /* Include nonce in HMAC to bind counter to ciphertext */

    /* HMAC = SHA3-512(mac_key_opad || SHA3-512(mac_key_ipad || nonce || aead)) */
    uint8_t hmac[64];
    uint8_t hmac_data[12 + 65536];
    size_t hmac_data_len = 12 + aead_len;
    if (hmac_data_len > sizeof(hmac_data))
        LOG_FAIL("secure_chan", "encrypt: HMAC data overflow (%zu > %zu)", hmac_data_len, sizeof(hmac_data));
    memcpy(hmac_data, nonce, 12);
    memcpy(hmac_data + 12, aead_out, aead_len);
    hmac_sha3_512(ch->mac_key, 64, hmac_data, hmac_data_len, hmac);

    /* Write HMAC prefix */
    memcpy(out, hmac, 64);

    *out_len = 64 + aead_len;
    return true;
}

bool secure_channel_decrypt(struct secure_channel *ch,
                            const uint8_t *data, size_t len,
                            uint8_t *out, size_t *out_len)
{
    if (!ch->established)
        LOG_FAIL("secure_chan", "decrypt: channel not established");
    if (len < SECURE_OVERHEAD)
        LOG_FAIL("secure_chan", "decrypt: data too short (%zu < %d)", len, SECURE_OVERHEAD);

    const uint8_t *received_hmac = data;
    const uint8_t *aead_data = data + 64;
    size_t aead_len = len - 64;

    uint8_t nonce[12];
    counter_to_nonce(ch->recv_counter, nonce);

    /* Verify HMAC-SHA3-512 first (reject before decryption) */
    uint8_t expected_hmac[64];
    uint8_t hmac_data[12 + 65536];
    size_t hmac_data_len = 12 + aead_len;
    if (hmac_data_len > sizeof(hmac_data))
        LOG_FAIL("secure_chan", "decrypt: HMAC data overflow (%zu > %zu)", hmac_data_len, sizeof(hmac_data));
    memcpy(hmac_data, nonce, 12);
    memcpy(hmac_data + 12, aead_data, aead_len);
    hmac_sha3_512(ch->mac_key, 64, hmac_data, hmac_data_len, expected_hmac);

    /* Constant-time comparison to prevent timing attacks */
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= received_hmac[i] ^ expected_hmac[i];
    if (diff != 0)
        LOG_FAIL("secure_chan", "decrypt: HMAC verification failed (counter=%llu)", (unsigned long long)ch->recv_counter);

    /* HMAC verified — now decrypt AEAD */
    if (!chacha20poly1305_decrypt(aead_data, aead_len,
                                  NULL, 0,
                                  nonce, ch->enc_key,
                                  out)) {
        LOG_FAIL("secure_chan", "chacha20poly1305 decrypt failed (len=%zu)", aead_len);
    }

    ch->recv_counter++;
    *out_len = aead_len - 16; /* plaintext = ciphertext - tag */
    return true;
}

void secure_channel_destroy(struct secure_channel *ch)
{
    memory_cleanse(ch->enc_key, 32);
    memory_cleanse(ch->mac_key, 64);
    memory_cleanse(ch, sizeof(*ch));
}
