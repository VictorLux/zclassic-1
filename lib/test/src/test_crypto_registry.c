/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "crypto_registry/crypto_registry.h"

int test_crypto_registry(void)
{
    int failures = 0;
    uint8_t out[32];

    printf("\n=== crypto_registry ===\n");

    printf("registry has expected schemes... ");
    const struct crypto_scheme *sha =
        crypto_registry_lookup(CRYPTO_HASH_SHA256);
    const struct crypto_scheme *blake =
        crypto_registry_lookup(CRYPTO_HASH_BLAKE2B_256);
    const struct crypto_scheme *ecdsa =
        crypto_registry_lookup(CRYPTO_SIG_ECDSA_SECP256K1);
    const struct crypto_scheme *groth =
        crypto_registry_lookup(CRYPTO_ZK_GROTH16_BLS12_381);
    if (sha && blake && ecdsa && groth &&
        crypto_registry_count() == 4 &&
        crypto_registry_count_by_kind(CRYPTO_KIND_HASH) == 2 &&
        crypto_registry_count_by_kind(CRYPTO_KIND_SIG) == 1 &&
        crypto_registry_count_by_kind(CRYPTO_KIND_ZK) == 1) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
        failures++;
    }

    printf("sha256 wrapper vector... ");
    if (sha && sha->fn.hash("hello", 5, out) == 0)
        failures += check_hex(out, 32,
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    else {
        printf("FAIL\n");
        failures++;
    }

    printf("blake2b-256 wrapper vector... ");
    if (blake && blake->fn.hash("abc", 3, out) == 0)
        failures += check_hex(out, 32,
            "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
    else {
        printf("FAIL\n");
        failures++;
    }

    printf("ecdsa wrapper verifies generated signature... ");
    {
        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);

        struct uint256 hash;
        memset(hash.data, 0x5a, sizeof(hash.data));

        uint8_t sig[SIGNATURE_SIZE];
        size_t sig_len = sizeof(sig);
        bool signed_ok = privkey_sign(&k, &hash, sig, &sig_len);
        bool verified = ecdsa && ecdsa->fn.sig_verify(pk.vch, pk.size,
                                                      hash.data, 32,
                                                      sig, sig_len);
        hash.data[0] ^= 0x01;
        bool rejected = ecdsa && !ecdsa->fn.sig_verify(pk.vch, pk.size,
                                                       hash.data, 32,
                                                       sig, sig_len);
        if (signed_ok && verified && rejected)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("groth16 wrapper rejects malformed inputs... ");
    {
        uint8_t bad_vk[4] = {0};
        uint8_t bad_proof[192] = {0};
        bool ok = groth && !groth->fn.zk_verify(bad_vk, sizeof(bad_vk),
                                                NULL, 0,
                                                bad_proof,
                                                sizeof(bad_proof) - 1);
        if (ok)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("diagnostics dump exposes schemes... ");
    {
        struct json_value dump;
        json_init(&dump);
        bool ok = crypto_registry_dump_state_json(&dump, NULL);
        const struct json_value *total = json_get(&dump, "total_registered");
        const struct json_value *schemes = json_get(&dump, "schemes");
        ok = ok && total && json_get_int(total) == 4 &&
             schemes && json_size(schemes) == 4;
        json_free(&dump);
        if (ok)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    return failures;
}
