/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim.
 *
 * Vendor libsecp256k1.a exports the OLD function name:
 *   secp256k1_ec_privkey_tweak_add
 *
 * The vendor header (secp256k1.h) declares the NEW name:
 *   secp256k1_ec_seckey_tweak_add
 *
 * This file provides the missing symbol. It MUST be compiled
 * into every binary that links libsecp256k1. Do NOT delete this
 * file or move this shim into key.c — it gets reverted by merges.
 *
 * Verify with: nm vendor/lib/libsecp256k1.a | grep tweak_add
 * If only privkey_tweak_add exists, this shim is required. */

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
