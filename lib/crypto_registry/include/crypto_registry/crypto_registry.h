/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CRYPTO_REGISTRY_H
#define ZCL_CRYPTO_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum crypto_scheme_id {
    CRYPTO_HASH_SHA256          = 1,
    CRYPTO_HASH_SHA3_256        = 2,
    CRYPTO_HASH_BLAKE2B_256     = 3,

    CRYPTO_SIG_ECDSA_SECP256K1  = 100,
    CRYPTO_SIG_ED25519          = 101,

    CRYPTO_ZK_GROTH16_BLS12_381 = 200,

    CRYPTO_SCHEME_MAX           = 1000,
};

enum crypto_scheme_status {
    CRYPTO_STATUS_UNREGISTERED = 0,
    CRYPTO_STATUS_ACTIVE       = 1,
    CRYPTO_STATUS_DEPRECATED   = 2,
    CRYPTO_STATUS_RETIRED      = 3,
};

enum crypto_scheme_kind {
    CRYPTO_KIND_HASH = 1,
    CRYPTO_KIND_SIG  = 2,
    CRYPTO_KIND_ZK   = 3,
};

typedef int (*crypto_hash_fn)(const void *data, size_t len, uint8_t out[32]);

typedef bool (*crypto_sig_verify_fn)(const uint8_t *pubkey,
                                     size_t pubkey_len,
                                     const uint8_t *msg,
                                     size_t msg_len,
                                     const uint8_t *sig,
                                     size_t sig_len);

typedef bool (*crypto_zk_verify_fn)(const uint8_t *vk,
                                    size_t vk_len,
                                    const uint8_t *public_inputs,
                                    size_t pi_len,
                                    const uint8_t *proof,
                                    size_t proof_len);

struct crypto_scheme {
    enum crypto_scheme_id     id;
    enum crypto_scheme_kind   kind;
    enum crypto_scheme_status status;
    const char               *name;
    const char               *impl;
    union {
        crypto_hash_fn       hash;
        crypto_sig_verify_fn sig_verify;
        crypto_zk_verify_fn  zk_verify;
    } fn;
};

bool crypto_registry_register(const struct crypto_scheme *scheme);
const struct crypto_scheme *crypto_registry_lookup(enum crypto_scheme_id id);
bool crypto_registry_is_usable(enum crypto_scheme_id id);

size_t crypto_registry_count(void);
size_t crypto_registry_count_by_kind(enum crypto_scheme_kind kind);

struct json_value;
bool crypto_registry_dump_state_json(struct json_value *out, const char *key);

#endif
