/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility — NOT NEEDED.
 *
 * Our vendor libsecp256k1.a exports secp256k1_ec_seckey_tweak_add
 * natively (verified via: nm vendor/lib/libsecp256k1.a | grep tweak_add).
 *
 * The old name secp256k1_ec_privkey_tweak_add does NOT exist in our
 * vendor library. No shim is needed. This file is intentionally empty
 * to prevent merge conflicts from re-adding a broken shim. */
typedef int secp256k1_compat_empty_tu; /* ISO C requires non-empty TU */
