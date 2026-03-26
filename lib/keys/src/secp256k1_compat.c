/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim.
 *
 * $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   T secp256k1_ec_privkey_tweak_add    <-- OLD name, exists
 *   T secp256k1_ec_pubkey_tweak_add
 *
 * Header declares secp256k1_ec_seckey_tweak_add (NEW name).
 * This shim provides seckey_tweak_add by calling privkey_tweak_add.
 *
 * DO NOT FLIP THE DIRECTION. DO NOT MAKE THIS FILE EMPTY. */

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
