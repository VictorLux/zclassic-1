# Worker Assignment — Phase 5a-3: Rewire script_validate ECDSA through crypto registry

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 5 (Crypto agility + reproducible releases)
**Depends on:** Phase 5a-1 (registry skeleton) ✅ + Phase 5a-2 (Equihash rewire) ✅
**Plan reference:** [`docs/architecture/phase5-crypto-agility-and-releases.md`](../architecture/phase5-crypto-agility-and-releases.md)

**Owns:**
- EDIT `lib/keys/src/pubkey.c` — rewire `pubkey_verify` to route through the registry
- EDIT `lib/keys/include/keys/pubkey.h` — no API change, but document the registry routing
- EDIT `lib/test/src/test_crypto_registry.c` — add benchmark for the hot-path overhead
- (Maybe) edit `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c` if the existing wrapper doesn't match the `pubkey_verify` call shape

**MUST NOT touch:**
- Other crypto call sites (Equihash already done in 5a-2; proof_validate is 5a-4)
- Existing libsecp256k1 vendor — wrappers READ it, don't modify
- Wave S, Phase 3, Phase 4 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

5a-2 routed the Equihash PoW check (once per block, ~75s of CPU per
3M-block sync). It proved the indirection layer is zero-cost on a
low-frequency call site.

**5a-3 is the HOT PATH test.** ECDSA `pubkey_verify` is called once
per script-signature check, which can happen multiple times per
transaction. For a fully syncing node, that's millions of calls per
sync. If the registry indirection layer has ANY measurable overhead,
it shows up here.

The expected outcome: zero observable regression in `zcl_kpi`
signature verification rate. If there IS overhead, fix the registry's
hot path (it should be a single atomic_load + branch — see
`lib/crypto_registry/src/crypto_registry.c`'s `crypto_registry_lookup`)
before declaring 5a-3 done.

After 5a-3 ships AND soaks 24h with no measurable regression:
- 5a-4 (proof_validate Sapling Groth16) can follow with confidence
- 5a-5 (BLAKE2b consumers — Equihash internal, key derivation) can follow
- After 5a-5: the registry covers all consensus crypto. New schemes
  (post-quantum candidates, defensive shadows) become single-PR adds.

---

## Current call site

`lib/keys/src/pubkey.c:14`:

```c
bool pubkey_verify(const struct pubkey *pk, const struct uint256 *hash,
                   const unsigned char *sig, size_t siglen)
{
    if (!pubkey_is_valid(pk))
        return false;
    secp256k1_pubkey pubkey;
    secp256k1_ecdsa_signature esig;
    if (!secp256k1_ec_pubkey_parse(secp256k1_ctx_verify, &pubkey,
                                    pk->vch, pk->size))
        return false;
    if (siglen == 0)
        return false;
    if (!secp256k1_ecdsa_signature_parse_der(secp256k1_ctx_verify, &esig,
                                              sig, siglen))
        return false;
    secp256k1_ecdsa_signature_normalize(secp256k1_ctx_verify, &esig, &esig);
    return secp256k1_ecdsa_verify(secp256k1_ctx_verify, &esig,
                                   hash->data, &pubkey);
}
```

This is the ONE entry point. All `script/interpreter.c` ECDSA verifies
go through it. Rewiring this one function rewires the whole hot path.

---

## What "rewire" means concretely

The function shape stays the same. Internally, it routes through the
registry's cached scheme pointer:

```c
#include "crypto_registry/crypto_registry.h"

static _Atomic(const struct crypto_scheme *) g_ecdsa_cache;

bool pubkey_verify(const struct pubkey *pk, const struct uint256 *hash,
                   const unsigned char *sig, size_t siglen)
{
    if (!pubkey_is_valid(pk))
        return false;
    if (siglen == 0)
        return false;

    /* Fast-path cache: registry lookup is O(1) but still a memory load.
     * Cache the scheme pointer in atomic; first caller populates. */
    const struct crypto_scheme *scheme = atomic_load_explicit(
        &g_ecdsa_cache, memory_order_relaxed);
    if (!scheme) {
        scheme = crypto_registry_lookup(CRYPTO_SIG_ECDSA_SECP256K1);
        atomic_store_explicit(&g_ecdsa_cache, scheme, memory_order_relaxed);
    }
    if (!scheme || !scheme->fn.sig_verify) {
        EMIT(EV_CRYPTO_REGISTRY_MISSING,
             "ecdsa-secp256k1 scheme unavailable");
        return false;
    }

    return scheme->fn.sig_verify(pk->vch, pk->size,
                                  hash->data, sizeof hash->data,
                                  sig, siglen);
}
```

The existing wrapper at `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c`
must do the DER parse + normalize + verify dance internally (i.e.,
its `sig_verify` callback wraps the same 4 secp256k1 calls). If the
wrapper currently expects a different shape, fix it.

---

## Tasks (in order)

### Task 1: Verify the wrapper matches the call shape

READ `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c`. The
`sig_verify` callback signature per `crypto_registry.h` is:

```c
typedef bool (*crypto_sig_verify_fn)(const uint8_t *pubkey, size_t pubkey_len,
                                     const uint8_t *msg, size_t msg_len,
                                     const uint8_t *sig, size_t sig_len);
```

If the existing wrapper:
- Takes raw pubkey bytes + raw sig DER bytes + raw msg → ALREADY MATCHES (good, skip to Task 2)
- Takes a different shape → fix it to match the typedef. Move the DER
  parse + normalize logic INTO the wrapper.

**Acceptance:** the wrapper compiles + a known-good test vector
verifies through `crypto_registry_lookup(CRYPTO_SIG_ECDSA_SECP256K1)
->fn.sig_verify(...)`.

### Task 2: Rewire `pubkey_verify` to the registry

EDIT `lib/keys/src/pubkey.c:14`. Replace the inline secp256k1 calls
with the registry-routed version above. Keep the same return-shape:
`true` on verify success, `false` on any failure (invalid pubkey,
empty sig, parse failure, verify failure).

The cache pattern (static `_Atomic` + check-then-populate) is
important: in IBD, `pubkey_verify` is called millions of times. A
raw `crypto_registry_lookup` per call is one extra memory load —
small but measurable. The cache reduces it to a single atomic_load
per call (predictable branch).

**Acceptance:**
- `make -j$(nproc)` compiles clean.
- Existing `test_keys` + `test_script` + `test_validation` tests
  PASS — they exercise this code path heavily.

### Task 3: Add a hot-path benchmark

EDIT `lib/test/src/test_crypto_registry.c`. Add a benchmark case:

```c
/* Measure: pubkey_verify (registry-routed) vs raw secp256k1_ecdsa_verify */
static void bench_pubkey_verify(int iterations) {
    /* Set up: known-good pubkey + hash + sig (any fixture) */
    /* Loop iterations times calling pubkey_verify */
    /* Loop iterations times calling the underlying secp256k1 chain directly */
    /* Report both timings + overhead percentage */
}
```

Run 100K iterations. **Registry overhead must be < 0.5%** of total
verify time. ECDSA verify takes ~100us on modern x86; the registry
adds one atomic_load (~1ns) + one indirect call (~2ns) = ~3ns ≈
0.003% overhead. Well under the threshold.

If overhead exceeds 0.5%: investigate. The registry's hot path is in
`crypto_registry_lookup` — it should be `return g_schemes[id]`
(single array indirection). If it's doing more, simplify.

**Acceptance:** benchmark passes the < 0.5% gate. Print numbers in
the test log.

### Task 4: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Live observability (post-merge)

Orchestrator polls `zcl_kpi.signature_verifies_per_sec` for 24h.
**Must be within ±2%** of the prior week's baseline. If it dips,
revert this PR and investigate.

The signature verify rate is most visible during IBD; on a synced
node, the rate is much lower (only new tx admissions). For test
purposes the benchmark in Task 3 is the proxy.

---

## What this does NOT do

- Does NOT touch `pubkey_recover_compact` (which calls
  `secp256k1_ecdsa_recover`, a different operation). 5a-3 only routes
  the verify path.
- Does NOT touch JoinSplit Ed25519 verify. That's a separate scheme
  id (`CRYPTO_SIG_ED25519`) and a separate (future) rewire.
- Does NOT add ed25519 or any new scheme.
- Does NOT touch the BLAKE2b hash chain (5a-5).
- Does NOT change the on-wire signature format.

---

## Commit cadence

One commit per task. Push after task 2.

---

## Status

**✅ DONE — pushed 2026-05-24** to main as commit `cde601acf`.

<!-- Worker: append a Completion section below when done. -->

## Completion (wt3, 2026-05-24)

### Summary
ECDSA/secp256k1 verification now routes through `crypto_registry` from
`pubkey_verify`, which covers the script-validation signature hot path.
The registry wrapper owns the raw DER parse, normalization, and secp256k1
verify sequence to avoid recursive dispatch.

### Commits
- `51a9783db` wt3: claim script validate crypto rewire
- `7c2c067a0` crypto_registry: route ECDSA verify hot path
- `cde601acf` crypto_registry: stabilize ECDSA benchmark

### Files added/modified
- `lib/crypto_registry/src/scheme_secp256k1_ecdsa.c`
- `lib/keys/include/keys/pubkey.h`
- `lib/keys/src/pubkey.c`
- `lib/test/src/test_crypto_registry.c`
- `docs/work/wt-phase5a3-script-validate-rewire.md`

### Acceptance verification
- [x] `make -j$(nproc)` — PASS.
- [x] `make test_parallel` — PASS.
- [x] `make lint` — PASS.
- [x] `ZCL_TEST_ONLY=crypto_registry ./test_zcl` — PASS.
- [x] `./test_parallel --jobs=$(nproc)` — PASS: 0/196 groups failed.
- [x] ECDSA hot-path benchmark — PASS in parallel run:
  direct `5048051928ns`, registry `5026082601ns`, overhead `-4352ppm`.

### Surprises / follow-ups
The first benchmark version measured all direct calls before all registry
calls, which was vulnerable to parallel-suite load drift. The shipped
benchmark interleaves direct and registry batches while preserving the
100K-call and 0.5% overhead gates.
