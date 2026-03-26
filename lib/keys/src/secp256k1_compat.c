/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim. THIS FILE IS REQUIRED.
 *
 * PROOF (run this yourself):
 *   $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   0000000000013210 T secp256k1_ec_privkey_tweak_add   <-- OLD name
 *   0000000000013560 T secp256k1_ec_pubkey_tweak_add
 *
 * The vendor .a has ONLY secp256k1_ec_privkey_tweak_add.
 * The vendor header declares secp256k1_ec_seckey_tweak_add.
 * Without this shim, the build fails with:
 *   undefined reference to `secp256k1_ec_seckey_tweak_add'
 *
 * DO NOT DELETE THIS SHIM. DO NOT REPLACE WITH AN EMPTY FILE.
 * If you think the symbol exists natively, run the nm command above. */

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
