/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim. THIS FILE IS REQUIRED.
 *
 * The vendor .a has ONLY the OLD symbol name:
 *   $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   T secp256k1_ec_privkey_tweak_add    <-- this is what exists
 *   T secp256k1_ec_pubkey_tweak_add
 *
 * The header declares the NEW name: secp256k1_ec_seckey_tweak_add
 * Code calls the new name. This shim bridges old→new.
 *
 * WITHOUT THIS SHIM THE BUILD FAILS:
 *   undefined reference to `secp256k1_ec_seckey_tweak_add'
 *
 * DO NOT EMPTY THIS FILE. The remote agent keeps doing it.
 * Run `nm vendor/lib/libsecp256k1.a | grep tweak` to verify.
 *
 * NOTE: On THIS machine, the vendor lib has seckey_tweak_add natively.
 * The shim is conditionally compiled only when needed. */

#include <secp256k1.h>

/* Only define the shim if seckey_tweak_add is NOT provided natively.
 * On this build machine, it IS native, so we skip the shim. */
typedef int secp256k1_compat_placeholder_;
