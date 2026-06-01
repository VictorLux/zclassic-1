/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Crypto scheme registry — singleton catalog for cryptographic verifier
 * implementations.
 *
 * One level of indirection between consensus-critical call sites and the
 * concrete crypto implementations they invoke. Consensus paths use this for
 * ECDSA public-key verification and Equihash proof verification; diagnostics
 * expose the registered scheme table for operator inspection.
 *
 * Scheme ids are PERMANENT — once allocated, never reused. New schemes
 * append to the end. Removal is also permanent (slot stays reserved
 * with status=RETIRED in the registry).
 *
 * Wrappers register themselves at process start via
 * __attribute__((constructor)), so the registry is fully populated by
 * the time main() runs. Lookup is lock-free (atomic loads).
 */

#ifndef ZCL_CRYPTO_REGISTRY_H
#define ZCL_CRYPTO_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum crypto_scheme_id {
    /* Hash functions */
    CRYPTO_HASH_SHA256              = 1,
    CRYPTO_HASH_SHA3_256            = 2,
    CRYPTO_HASH_BLAKE2B_256         = 3,

    /* Signature schemes */
    CRYPTO_SIG_ECDSA_SECP256K1      = 100,
    CRYPTO_SIG_ED25519              = 101,

    /* Zero-knowledge proofs */
    CRYPTO_ZK_GROTH16_BLS12_381     = 200,
    CRYPTO_PROOF_EQUIHASH_200_9     = 201,

    /* Sentinel — do not use as a real scheme id. Slot-array size. */
    CRYPTO_SCHEME_MAX               = 1000,
};

enum crypto_scheme_status {
    CRYPTO_STATUS_UNREGISTERED = 0,
    CRYPTO_STATUS_ACTIVE       = 1,
    CRYPTO_STATUS_DEPRECATED   = 2,   /* still works, warns on use */
    CRYPTO_STATUS_RETIRED      = 3,   /* refuses to operate */
};

enum crypto_scheme_kind {
    CRYPTO_KIND_HASH = 1,
    CRYPTO_KIND_SIG  = 2,
    CRYPTO_KIND_ZK   = 3,
};

/* Hash interface — variable-length input, fixed 32-byte output. */
typedef int (*crypto_hash_fn)(const void *data, size_t len, uint8_t out[32]);

/* Signature interface — verify only (signing lives in wallet layer). */
typedef bool (*crypto_sig_verify_fn)(const uint8_t *pubkey, size_t pubkey_len,
                                     const uint8_t *msg, size_t msg_len,
                                     const uint8_t *sig, size_t sig_len);

/* ZK interface — verify a proof against a verification key + public inputs. */
typedef bool (*crypto_zk_verify_fn)(const uint8_t *vk, size_t vk_len,
                                    const uint8_t *public_inputs, size_t pi_len,
                                    const uint8_t *proof, size_t proof_len);

struct crypto_scheme {
    enum crypto_scheme_id     id;
    enum crypto_scheme_kind   kind;
    enum crypto_scheme_status status;
    const char               *name;        /* "ecdsa-secp256k1", etc. */
    const char               *impl;        /* "libsecp256k1 v0.4.1", etc. */
    union {
        crypto_hash_fn       hash;
        crypto_sig_verify_fn sig_verify;
        crypto_zk_verify_fn  zk_verify;
    } fn;
};

/* Register a scheme. Returns false if the slot is already occupied
 * (registration is single-shot per id) or if the scheme/id is
 * malformed. Called from each scheme_<name>.c at static-init time via
 * __attribute__((constructor)). */
bool crypto_registry_register(const struct crypto_scheme *scheme);

/* Lookup. Returns NULL if id not registered. Lock-free atomic load. */
const struct crypto_scheme *crypto_registry_lookup(enum crypto_scheme_id id);

/* Returns false if UNREGISTERED or RETIRED; true for ACTIVE/DEPRECATED. */
bool crypto_registry_is_usable(enum crypto_scheme_id id);

/* Counters / introspection. */
size_t crypto_registry_count(void);                       /* total registered */
size_t crypto_registry_count_by_kind(enum crypto_scheme_kind kind);

/* Test-only reset — wipes the registry. Used by unit tests to exercise
 * the collision path deterministically. NOT for production use; the
 * constructors only fire once per process. */
void crypto_registry_test_reset(void);

/* Diagnostics dumper — see CLAUDE.md "Adding state introspection".
 * Reentrant-safe; caller calls json_set_object(out) before invoking. */
struct json_value;
bool crypto_registry_dump_state_json(struct json_value *out, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CRYPTO_REGISTRY_H */
