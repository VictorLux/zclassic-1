/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "crypto_registry/crypto_registry.h"
#include "crypto/blake2b.h"
#include "crypto/equihash.h"

static bool equihash_params_for_solution(size_t solution_len,
                                         unsigned int *n,
                                         unsigned int *k)
{
    if (!n || !k)
        return false;

    switch (solution_len) {
    case 1344:
        *n = 200; *k = 9; return true;
    case 400:
        *n = 192; *k = 7; return true;
    case 68:
        *n = 96; *k = 5; return true;
    case 36:
        *n = 48; *k = 5; return true;
    default:
        return false;
    }
}

static bool registry_equihash_200_9_verify(const uint8_t *vk,
                                           size_t vk_len,
                                           const uint8_t *public_inputs,
                                           size_t pi_len,
                                           const uint8_t *proof,
                                           size_t proof_len)
{
    (void)vk;
    (void)vk_len;

    if (!public_inputs || pi_len == 0 || !proof || proof_len == 0)
        return false;

    unsigned int n = 0;
    unsigned int k = 0;
    if (!equihash_params_for_solution(proof_len, &n, &k))
        return false;

    struct equihash_params ep;
    equihash_params_init(&ep, n, k);

    struct blake2b_ctx state;
    equihash_initialise_state(&ep, &state);
    blake2b_update(&state, public_inputs, pi_len);

    return equihash_is_valid_solution(&ep, &state, proof, proof_len);
}

static const struct crypto_scheme g_equihash_scheme = {
    .id = CRYPTO_PROOF_EQUIHASH_200_9,
    .kind = CRYPTO_KIND_ZK,
    .status = CRYPTO_STATUS_ACTIVE,
    .name = "equihash-200-9",
    .impl = "in-tree lib/crypto/src/equihash.c",
    .fn.zk_verify = registry_equihash_200_9_verify,
};

__attribute__((constructor))
static void register_equihash_scheme(void)
{
    crypto_registry_register(&g_equihash_scheme);
}
