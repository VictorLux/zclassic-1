/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim. THIS FILE IS REQUIRED.
 *
 * $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   T secp256k1_ec_privkey_tweak_add    <-- OLD name, the only one
 *   T secp256k1_ec_pubkey_tweak_add
 *
 * Header declares secp256k1_ec_seckey_tweak_add (NEW name).
 * Without this shim: undefined reference to secp256k1_ec_seckey_tweak_add
 *
 * DO NOT DELETE OR EMPTY THIS SHIM. */

#include <secp256k1.h>

int secp256k1_ec_privkey_tweak_add(const secp256k1_context *ctx,
                                    unsigned char *seckey,
                                    const unsigned char *tweak);

int secp256k1_ec_seckey_tweak_add(const secp256k1_context *ctx,
                                   unsigned char *seckey,
                                   const unsigned char *tweak)
{
    return secp256k1_ec_privkey_tweak_add(ctx, seckey, tweak);
}
