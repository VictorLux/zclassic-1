/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * secp256k1 API compatibility — vendor lib has ONLY seckey_tweak_add.
 *
 * PROOF:
 *   $ nm vendor/lib/libsecp256k1.a | grep tweak_add
 *   000000000001af40 T secp256k1_ec_seckey_tweak_add   <-- exists
 *   (no secp256k1_ec_privkey_tweak_add symbol)
 *
 * The vendor header (secp256k1.h) declares secp256k1_ec_seckey_tweak_add.
 * The vendor .a exports secp256k1_ec_seckey_tweak_add.
 * No shim is needed — seckey_tweak_add works natively.
 *
 * This file is kept to prevent merge conflicts. */

#include <secp256k1.h>

/* Intentionally empty — seckey_tweak_add is provided by vendor lib. */
typedef int secp256k1_compat_empty_unit_;
