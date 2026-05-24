/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "crypto_registry/crypto_registry.h"
#include "core/uint256.h"
#include "keys/pubkey.h"

static bool registry_ecdsa_verify(const uint8_t *pubkey, size_t pubkey_len,
                                  const uint8_t *msg, size_t msg_len,
                                  const uint8_t *sig, size_t sig_len)
{
    if (!pubkey || !msg || !sig || msg_len != 32 ||
        pubkey_len > PUBLIC_KEY_SIZE || pubkey_len == 0)
        return false;

    struct pubkey pk;
    pubkey_init(&pk);
    pubkey_set(&pk, pubkey, (unsigned int)pubkey_len);

    struct uint256 hash;
    memcpy(hash.data, msg, 32);
    return pubkey_verify(&pk, &hash, sig, sig_len);
}

static const struct crypto_scheme g_ecdsa_scheme = {
    .id = CRYPTO_SIG_ECDSA_SECP256K1,
    .kind = CRYPTO_KIND_SIG,
    .status = CRYPTO_STATUS_ACTIVE,
    .name = "ecdsa-secp256k1",
    .impl = "in-tree keys/pubkey.c over vendored libsecp256k1",
    .fn.sig_verify = registry_ecdsa_verify,
};

__attribute__((constructor))
static void register_ecdsa_scheme(void)
{
    crypto_registry_register(&g_ecdsa_scheme);
}
