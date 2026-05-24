# Worker Assignment — Phase 5a-1: Crypto registry skeleton

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 5 (Crypto agility + reproducible releases)
**Depends on:** Nothing in flight. Can land in parallel with Phase 2
cutovers and Phase 3 dissolves — it's purely additive indirection.
**Plan reference:** [`docs/architecture/phase5-crypto-agility-and-releases.md`](../architecture/phase5-crypto-agility-and-releases.md)

**Owns:**
- NEW `lib/crypto_registry/include/crypto_registry/crypto_registry.h`
- NEW `lib/crypto_registry/src/crypto_registry.c`
- NEW `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c` — wraps existing impl
- NEW `lib/crypto_registry/src/scheme_groth16_bls12_381.c` — wraps existing impl
- NEW `lib/crypto_registry/src/scheme_sha256.c` — wraps existing impl
- NEW `lib/crypto_registry/src/scheme_blake2b.c` — wraps existing impl
- NEW `lib/test/src/test_crypto_registry.c`
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`
- EDIT `app/controllers/src/diagnostics_controller.c` — register `crypto_registry` in `g_dumpers`
- EDIT `tools/mcp/controllers/ops_controller.c` — add `crypto_registry` to `zcl_state.subsystem` enum

**MUST NOT touch:**
- Existing crypto impls — wrappers READ them, don't modify (`lib/crypto/src/*`,
  `lib/sapling/*`, etc.)
- Consensus paths — no call sites get rewired in this PR; the registry
  sits idle, callable but unused
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 5 prepares the node for post-quantum and for crypto-scheme
upgrades that won't require a fork-the-binary release. Today every
signature verification goes through a hard-coded call into
`ecdsa_verify_secp256k1` (or wherever). To swap in a new scheme — even
a defensive one for testing — you have to surgery the call sites.

This PR introduces ONE level of indirection: an enum scheme id + a
dispatch table. Existing impls are wrapped, not changed. No call sites
get rewired (yet). The registry sits idle but loaded, callable from
the unit test.

Phase 5a-2 (separate PR) will rewire ONE consensus call site (the
header PoW check) through the registry to prove the indirection is
zero-cost. Phase 5a-3 will rewire script_validate. Then we have the
real shape — and adding a new scheme is a new wrapper file, not a
codebase migration.

This is the smallest possible first step. Low risk. High option value.

---

## API

```c
/* lib/crypto_registry/include/crypto_registry/crypto_registry.h */
#ifndef ZCL_CRYPTO_REGISTRY_H
#define ZCL_CRYPTO_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scheme ids are PERMANENT — once allocated, never reused. New schemes
 * append to the end. Removal is also permanent (slot stays reserved
 * with status=RETIRED in the registry). */
enum crypto_scheme_id {
    /* Hash functions */
    CRYPTO_HASH_SHA256              = 1,
    CRYPTO_HASH_SHA3_256            = 2,
    CRYPTO_HASH_BLAKE2B_256         = 3,

    /* Signature schemes */
    CRYPTO_SIG_ECDSA_SECP256K1      = 100,
    CRYPTO_SIG_ED25519              = 101,

    /* Zero-knowledge proofs */
    CRYPTO_ZK_GROTH16_BLS12_381     = 200,

    /* Sentinel — do not use as a real scheme id. */
    CRYPTO_SCHEME_MAX               = 1000,
};

enum crypto_scheme_status {
    CRYPTO_STATUS_UNREGISTERED = 0,
    CRYPTO_STATUS_ACTIVE       = 1,
    CRYPTO_STATUS_DEPRECATED   = 2,   /* still works, warns on use */
    CRYPTO_STATUS_RETIRED      = 3,   /* refuses to operate */
};

enum crypto_scheme_kind {
    CRYPTO_KIND_HASH = 1,
    CRYPTO_KIND_SIG  = 2,
    CRYPTO_KIND_ZK   = 3,
};

/* Hash interface — variable-length input, fixed 32-byte output */
typedef int (*crypto_hash_fn)(const void *data, size_t len, uint8_t out[32]);

/* Signature interface — verify only (signing lives in wallet layer) */
typedef bool (*crypto_sig_verify_fn)(const uint8_t *pubkey, size_t pubkey_len,
                                     const uint8_t *msg, size_t msg_len,
                                     const uint8_t *sig, size_t sig_len);

/* ZK interface — verify a proof against a verification key + public inputs */
typedef bool (*crypto_zk_verify_fn)(const uint8_t *vk, size_t vk_len,
                                    const uint8_t *public_inputs, size_t pi_len,
                                    const uint8_t *proof, size_t proof_len);

struct crypto_scheme {
    enum crypto_scheme_id     id;
    enum crypto_scheme_kind   kind;
    enum crypto_scheme_status status;
    const char               *name;        /* "ecdsa-secp256k1", etc. */
    const char               *impl;        /* "libsecp256k1 v0.4.1", etc. */
    union {
        crypto_hash_fn       hash;
        crypto_sig_verify_fn sig_verify;
        crypto_zk_verify_fn  zk_verify;
    } fn;
};

/* Registry — singleton. Initialized once at boot. */

/* Register a scheme. Returns false if the slot is already occupied
 * (registration is single-shot per id). Called from each
 * scheme_<name>.c at static-init time via a constructor. */
bool crypto_registry_register(const struct crypto_scheme *scheme);

/* Lookup. Returns NULL if id not registered. */
const struct crypto_scheme *crypto_registry_lookup(enum crypto_scheme_id id);

/* Status helpers — convenience for callers that only need to verify
 * the scheme is usable. Returns false if UNREGISTERED or RETIRED. */
bool crypto_registry_is_usable(enum crypto_scheme_id id);

/* Counters / introspection */
size_t crypto_registry_count(void);                       /* registered */
size_t crypto_registry_count_by_kind(enum crypto_scheme_kind kind);

/* Diagnostics dumper — see CLAUDE.md "Adding state introspection". */
struct json_value;
bool crypto_registry_dump_state_json(struct json_value *out, const char *key);

#endif
```

---

## Tasks (in order)

### Task 1: Registry skeleton + header

NEW `lib/crypto_registry/include/crypto_registry/crypto_registry.h` per
the API above.

NEW `lib/crypto_registry/src/crypto_registry.c`:
- Static array `static const struct crypto_scheme *g_schemes[CRYPTO_SCHEME_MAX];`
- `crypto_registry_register` does CAS on `g_schemes[scheme->id]` — first
  writer wins; subsequent registrations of the same id return false.
- `crypto_registry_lookup` reads with `atomic_load`.
- `is_usable` lookup + status check.

Counters are atomic uint64_t per kind.

Add `Makefile` or build-system glue so `lib/crypto_registry/` builds as
a separate static library object set, linked into both the main binary
and `test_zcl`.

**Acceptance:** compiles. Empty registry returns NULL for any lookup.
`count()` == 0.

### Task 2: SHA256 wrapper

NEW `lib/crypto_registry/src/scheme_sha256.c`:

```c
#include "crypto_registry/crypto_registry.h"
#include "crypto/sha256.h"   /* existing impl */

static int sha256_hash_fn(const void *data, size_t len, uint8_t out[32]) {
    sha256_compute(data, len, out);   /* or whatever the existing entry is */
    return 0;
}

static const struct crypto_scheme g_sha256 = {
    .id     = CRYPTO_HASH_SHA256,
    .kind   = CRYPTO_KIND_HASH,
    .status = CRYPTO_STATUS_ACTIVE,
    .name   = "sha256",
    .impl   = "in-tree (lib/crypto/src/sha256.c)",
    .fn.hash = sha256_hash_fn,
};

__attribute__((constructor))
static void register_sha256(void) {
    crypto_registry_register(&g_sha256);
}
```

Find the actual sha256 entry point in `lib/crypto/include/crypto/sha256.h`
before writing the wrapper.

**Acceptance:** registry lookup returns the scheme; calling
`scheme->fn.hash("hello", 5, out)` produces the standard SHA256("hello")
digest (`2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824`).

### Task 3: BLAKE2b wrapper

Same pattern. `CRYPTO_HASH_BLAKE2B_256`. Wraps whatever blake2b impl is
in tree (probably `lib/crypto/src/blake2b.c`).

**Acceptance:** test vector check.

### Task 4: ECDSA secp256k1 wrapper

NEW `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c`. Wraps the
existing `script_verify` ECDSA call site — find the lowest-level entry
point (probably `secp256k1_ecdsa_verify` directly, via
`vendor/libsecp256k1/`).

**Acceptance:** known-good signature verifies through the registry;
known-bad signature fails.

### Task 5: Groth16/BLS12-381 wrapper

NEW `lib/crypto_registry/src/scheme_groth16_bls12_381.c`. Wraps whatever
Sapling proof-verify entry point exists in tree (probably
`lib/sapling/src/zk_verify.c` or `vendor/librustzcash/`).

**Acceptance:** known Sapling spend proof verifies through the registry.

### Task 6: Diagnostics + MCP

- `crypto_registry_dump_state_json` returns:
  ```json
  {
    "total_registered": 5,
    "by_kind": { "hash": 2, "sig": 1, "zk": 1, "ed25519_pending": 1 },
    "schemes": [
      { "id": 1,   "name": "sha256",            "kind": "hash", "status": "active" },
      { "id": 3,   "name": "blake2b-256",       "kind": "hash", "status": "active" },
      { "id": 100, "name": "ecdsa-secp256k1",   "kind": "sig",  "status": "active" },
      { "id": 200, "name": "groth16-bls12-381", "kind": "zk",   "status": "active" }
    ]
  }
  ```
- Register in `g_dumpers` table.
- Add `crypto_registry` to `zcl_state.subsystem` enum_csv.

**Acceptance:** `zcl_state(subsystem="crypto_registry")` returns the
JSON.

### Task 7: test_crypto_registry.c

Test cases:
1. **`register_lookup_basic`** — after process init, all 4 schemes
   present. `count()` == 4.
2. **`register_collision_rejected`** — try to register a 5th scheme
   with id=CRYPTO_HASH_SHA256; `register` returns false; registry
   still has the original.
3. **`lookup_unregistered_returns_null`** — `lookup(999)` returns NULL.
4. **`is_usable_false_for_unregistered`** — `is_usable(999)` returns
   false.
5. **`hash_vectors`** — each registered hash scheme produces correct
   output for known test vectors.
6. **`sig_verify_known_good`** — ECDSA verifies a known signature.
7. **`sig_verify_known_bad_rejected`** — modified signature fails.
8. **`zk_verify_known_good`** — Groth16 verifies a known proof.

Add to test_parallel + test_helpers.

**Acceptance:** `./test_parallel --jobs=$(nproc)` all green.

### Task 8: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin main
```

Append Completion section.

---

## What this does NOT do

- Does NOT rewire any consensus call site. Existing script_verify,
  proof_verify, hash computations call the same direct functions
  they always did.
- Does NOT change any wire format.
- Does NOT add any new scheme (no ed25519, no post-quantum candidates).
- Does NOT enable any kind of dynamic scheme loading or hot-swap.

This is **pure indirection in standby**. The next PR (Phase 5a-2)
rewires ONE call site to use the registry. By landing the skeleton
first we eliminate any "is the indirection layer too slow / wrong
shape" risk before touching consensus paths.

---

## Commit cadence

One commit per task. Push after tasks 2, 5, 7.

---

## Status

**READY** (status updated 2026-05-24) — independent of Wave S and
Phase 3. Any worker may claim by marking IN PROGRESS (wt<N>).

<!-- Worker: append a Completion section below when done. -->
