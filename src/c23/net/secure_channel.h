/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Authenticated encrypted P2P channel using:
 *   - Curve25519 Diffie-Hellman key exchange
 *   - SHA3-512 key derivation
 *   - ChaCha20-Poly1305 AEAD per-message encryption
 *   - HMAC-SHA3-512 message authentication
 *
 * Protocol:
 *   1. After TCP connect, both sides send 32-byte ephemeral pubkey
 *   2. Curve25519 DH → 32-byte shared secret
 *   3. SHA3-512(shared_secret) → 64 bytes:
 *      - bytes 0..31  = ChaCha20-Poly1305 encryption key
 *      - bytes 32..63 = HMAC-SHA3-512 authentication key
 *   4. Every P2P message: encrypt payload, prepend 64-byte HMAC tag
 *   5. Nonce = 8-byte message counter (LE) + 4 zero bytes */

#ifndef ZCL_NET_SECURE_CHANNEL_H
#define ZCL_NET_SECURE_CHANNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct secure_channel {
    uint8_t enc_key[32];       /* ChaCha20-Poly1305 key */
    uint8_t mac_key[64];       /* HMAC-SHA3-512 key (full SHA3-512 width) */
    uint8_t local_pubkey[32];  /* Our ephemeral Curve25519 public key */
    uint8_t remote_pubkey[32]; /* Peer's ephemeral public key */
    uint64_t send_counter;     /* Nonce counter for outgoing messages */
    uint64_t recv_counter;     /* Nonce counter for incoming messages */
    bool established;          /* True after key exchange complete */
};

/* Generate ephemeral keypair and prepare for handshake.
 * Returns the 32-byte public key to send to the peer. */
bool secure_channel_init(struct secure_channel *ch);

/* Complete handshake after receiving peer's 32-byte public key.
 * Derives encryption and authentication keys via SHA3-512. */
bool secure_channel_establish(struct secure_channel *ch,
                              const uint8_t remote_pubkey[32]);

/* Encrypt and authenticate a message.
 * Output: [16-byte poly1305 tag][ciphertext] — total out_len = len + 16.
 * Also computes HMAC-SHA3-512 over the authenticated ciphertext.
 * Full output: [64-byte HMAC][16-byte tag][ciphertext] — total = len + 80.
 * Caller must provide out buffer of at least len + 80 bytes. */
bool secure_channel_encrypt(struct secure_channel *ch,
                            const uint8_t *plaintext, size_t len,
                            uint8_t *out, size_t *out_len);

/* Verify HMAC and decrypt a message.
 * Input: [64-byte HMAC][16-byte tag][ciphertext].
 * Returns false if HMAC or AEAD tag verification fails.
 * On success, writes plaintext to out (len - 80 bytes). */
bool secure_channel_decrypt(struct secure_channel *ch,
                            const uint8_t *data, size_t len,
                            uint8_t *out, size_t *out_len);

/* Wipe all key material. */
void secure_channel_destroy(struct secure_channel *ch);

#endif
