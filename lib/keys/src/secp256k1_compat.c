/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 shim: seckey_tweak_add → privkey_tweak_add.
 * Vendor .a has ONLY privkey. Header declares seckey. */

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
