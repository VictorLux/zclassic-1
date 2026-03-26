/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility shim.
 *
 * The vendored libsecp256k1.a now exports the modern name
 * secp256k1_ec_seckey_tweak_add directly. No shim needed.
 *
 * If the library is ever downgraded to an older version that only
 * has privkey_tweak_add, flip the shim direction:
 *   seckey_tweak_add → calls privkey_tweak_add */

typedef int secp256k1_compat_unused;
