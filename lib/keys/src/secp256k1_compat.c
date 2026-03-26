/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim.
 *
 * Actual vendor symbols (verified on THIS machine):
 *   $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   T secp256k1_ec_seckey_tweak_add
 *   T secp256k1_ec_pubkey_tweak_add
 *
 * Some builds have the OLD name (privkey) instead. This shim provides
 * whichever name is missing, so the build works regardless. */

#include <secp256k1.h>

/* The vendor lib provides secp256k1_ec_seckey_tweak_add natively.
 * Provide the old name as an alias in case any code references it. */
int secp256k1_ec_seckey_tweak_add(const secp256k1_context *ctx,
                                   unsigned char *seckey,
                                   const unsigned char *tweak);

int secp256k1_ec_privkey_tweak_add(const secp256k1_context *ctx,
                                    unsigned char *seckey,
                                    const unsigned char *tweak)
{
    return secp256k1_ec_seckey_tweak_add(ctx, seckey, tweak);
}
