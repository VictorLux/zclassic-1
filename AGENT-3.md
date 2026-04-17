# AGENT-3 — Cryptography & Sapling Hardening

**Derived from:** the 2026-04-17 full code review (see `AGENT.md` for the full
checklist and coordinating plan).

**Working directory:** `~/zclassic23-3` (separate git clone; pulls/pushes to `origin/main`).

**Coordinator:** Rhett (~/zclassic23).

**Agent-2 lane (do not touch):** `lib/wallet/`, `lib/storage/`, `lib/coins/`.

---

## Status — 2026-04-17

First pass landed on `origin/a3/build-ci-deploy-hardening`:

- ✅ P1.3 — NULL VK fail-open fixed (3b4b08ba9)
- ✅ P1.4 — Sapling params integrity check (785db18b1)
- ✅ P1.8 — Ed25519 S<L canonicality (c510c7335)
- ✅ P1.9 — RedJubjub S<Fs canonicality (8440cd864)
- ✅ P1.10 — find_group_hash abort-on-fail (e221e0212)
- ✅ Step 7 — FIXME comment on jub_scalar_mul timing (ecd24894e)
- ✅ Regression tests (ebc470342)

**NEXT UP — start here:** Step 4 (P1.11) is the large remaining item. Do it in
small commits (one file per commit). Then Step 8 if time permits. Refresh your
tree before starting:

```bash
cd ~/zclassic23-3
git fetch origin
git checkout main
git pull origin main --rebase=false
git merge --ff-only origin/a3/build-ci-deploy-hardening || \
  git merge --no-ff origin/a3/build-ci-deploy-hardening \
    -m "merge a3/build-ci-deploy-hardening (P1.3/4/8/9/10 + fixes)"
git push origin main
```

If the merge conflicts, stop and tell Rhett — do NOT resolve speculatively.

After the merge lands, continue with Step 4 below (P1.11 audit) and Step 8
(nonce hygiene). The rest of this file is unchanged from your initial brief.

---

## Preflight — run verbatim, do not ask questions

Execute the block below top to bottom. Each command is defensive and idempotent.
Do NOT pause to ask about branch state, stash, or divergence — the block handles
all of it. Only stop if `make` or `./test_zcl` fails (broken baseline — tell Rhett).

```bash
cd ~/zclassic23-3

# 1. Preserve any local-only work to its current branch so nothing is stranded
git add -A
git diff --staged --quiet || git commit -m "a3: wip checkpoint before AGENT-3.md workstream"

# 2. If on a feature branch, push it as backup, then move to main
CURRENT=$(git branch --show-current)
if [ "$CURRENT" != "main" ]; then
    git push origin "$CURRENT" 2>/dev/null || true   # backup; ok if no perms
    git checkout main
fi

# 3. Sync main
git pull origin main --rebase=false

# 4. Confirm clean baseline
git status
git branch --show-current            # must print "main"

# 5. Read the rules and the checklist
cat CLAUDE.md
cat DEFENSIVE_CODING.md
cat AGENT.md                          # your rows: P1.3, P1.4, P1.8, P1.9, P1.10, P1.11

# 6. Confirm green baseline — STOP and report if either fails
make -j"$(nproc)"
./test_zcl
```

Once that all passes, start Step 1 below. Commit/push after each logical fix.

---

## Your mission

Close the fail-open paths and observability gaps in the crypto and shielded-tx
subsystems so that:
1. No verification path ever silently succeeds when a precondition is missing
   (NULL verification key, failed setup, missing parameter file).
2. Every cryptographic failure emits a `LOG_FAIL` with enough context to
   diagnose post-mortem — silent crypto failure = invisible consensus split.
3. Ed25519 and RedJubjub signatures enforce canonical-S, matching the Zcash
   consensus rules that the legacy zclassicd checks.
4. Sapling parameter files are integrity-checked at load time against the
   baked-in SHA-512 constants.

These findings are consensus-critical: any of them can make zclassic23 accept
blocks the legacy peer rejects (or vice versa), splitting the chain on deploy.

---

## Files you own

You may edit anything under:
- `lib/crypto/`
- `lib/sapling/`
- `lib/keys/`
- `lib/test/` — but only to add tests that cover your changes

You may read anything else, but do not edit outside these trees.

---

## Workstream (do in this order)

### Step 1 — P1.3: Sapling verify fail-open on NULL VK
File: `lib/sapling/src/sapling.c:505, 559`

**Bug:** `sapling_check_spend` and `sapling_check_output` silently return `true`
when `sapling_spend_vk` / `sapling_output_vk` is NULL. Any code path that
skips or frees params early makes shielded txs trivially forgeable.

**Fix:** If the relevant VK is NULL, return `false` AND call `LOG_FAIL` with
the specific VK name. Callers of `sapling_check_*` must treat `false` as
"reject the transaction" — verify every caller already does this and add a
test for the NULL-VK case.

### Step 2 — P1.4: Sapling params loaded without integrity check
File: `lib/sapling/src/params_init.c:47-167` (SHA-512 constants at `:158-162`)

**Bug:** Parameter files (`sapling-spend.params`, `sapling-output.params`,
`sprout-groth16.params`) are memory-mapped / read and passed to the prover.
The baked-in SHA-512 constants are only consulted by the prover, never
compared against the file contents.

**Fix:** After reading each params file, compute SHA-512 of the file bytes
and compare (constant-time) against the baked-in constant. On mismatch:
`LOG_FAIL` with the file path and expected/actual hashes, then fail node
startup — do NOT continue running with unknown params.

**Acceptance:** add a test that writes a tampered params file to a temp dir,
points the loader at it, and asserts startup fails.

### Step 3 — P1.10: find_group_hash returns ignored
File: `lib/sapling/src/sapling.c:81-110` (`ensure_fixed_generators`)

**Bug:** Six `find_group_hash()` calls whose returns are ignored. On failure
the generator is zero-initialized memory and every subsequent scalar mul
produces garbage with no error signal.

**Fix:** Capture every return; if any failure, `LOG_FAIL` with the generator
name and abort node startup. These are fixed generators — failure means
something is very wrong and running is unsafe.

### Step 4 — P1.11: Zero LOG_FAIL across crypto/sapling
Files: everything under `lib/crypto/` and `lib/sapling/`

**Bug:** Direct violation of DEFENSIVE_CODING.md §4: no `LOG_FAIL`/`LOG_ERR`/
`LOG_NULL` calls anywhere in these modules. Every Groth16, RedJubjub, KDF,
AEAD, ECDSA, Ed25519 failure returns bare `false` with no diagnostic.

**Fix:** Audit every error return and add `LOG_FAIL` / `LOG_ERR` with enough
context to locate the failure (function name, which check failed, relevant
non-secret parameters). Do NOT log secret material (keys, nonces, plaintext).

Do this in small commits — one file per commit is fine.

### Step 5 — P1.8: Ed25519 missing S<L canonicality
File: `lib/crypto/src/ed25519.c:300-355` (`ed25519_verify`)

**Bug:** No `S < L` check in `ed25519_verify`. Zcash JoinSplit consensus
requires canonical S; malleable sigs may diverge from zclassicd.

**Fix:** Add a constant-time scalar-bound check (`S` as 32 little-endian bytes
< group order `L` as 32 little-endian bytes). On failure, `LOG_FAIL` and
return false. Reference test vectors: RFC 8032 + Zcash test vectors.

### Step 6 — P1.9: RedJubjub missing S<r canonicality
File: `lib/sapling/src/sapling.c:386` (`redjubjub_verify`)

**Bug:** Comment notes "Check S < Fs order (optional, Rust checks via
from_repr)" but no check performed; `sig_sbar` is fed straight into
`jub_scalar_mul`.

**Fix:** Add the `S < r` scalar-bound check using the Sapling `Fs` order
constants already in the file. Mirror the same pattern as Ed25519.

### Step 7 — P1.(additional) — Side-channel audit of jub_scalar_mul
File: `lib/sapling/src/fr.c:307-333`

**Context:** This function uses 4-bit windowed scalar mult with
secret-dependent table lookups (`table[nibble]`) and secret-dependent
branches (`if (nibble)`). Called with secret scalars (`ask`, `nsk`, `ivk`,
`esk`, `bsk`). Leaks spending/viewing keys to co-resident attackers via
cache timing.

**Fix scope:** This is genuinely hard — constant-time windowed scalar mult
requires a branchless conditional select and masked table walks. Out of
scope for a first pass.

**Action:** Do NOT attempt a crypto rewrite. Instead:
- Add a `FIXME(timing): jub_scalar_mul is not constant-time over secret scalars`
  comment at the function header citing this review item.
- Open a follow-up note in your commit message so Rhett can triage a dedicated
  wave for constant-time scalar mult.

### Step 8 (if time) — P1.(additional) — Note encryption nonce hygiene
File: `lib/sapling/src/note_encryption.c:14, 124, 149, 159, 167, 175, 183`

**Context:** All AEAD calls use a constant `zero_nonce[12]`. Safe only because
the key is unique per message, derived from `epk`. If `epk` ever repeats
(RNG failure), two-time pad.

**Fix:** Add a runtime debug assertion (under `#ifndef NDEBUG` or a new
`ZCL_CRYPTO_SANITY` flag) that tracks the last few `esk` values per process
and aborts on repeat. Document the hazard at the file header.

---

## Commit protocol

- One logical fix per commit. Good: "sapling: return false on NULL VK".
  Bad: "sapling hardening".
- Every commit: `make test` must pass. Every push: `make ci` must pass.
- Commit message format:
  ```
  sapling|crypto|keys: <one-line summary>

  <why — cite file:line from AGENT.md>

  Fixes P1.3 (AGENT.md checklist).
  ```
- After each push, update the corresponding row in `AGENT.md`:
  `open` → `in-progress` → `done (SHA abc1234)`.
- Push frequently so Rhett can review incrementally.
- Never `--amend` a pushed commit. Never `--force-push`.

---

## Coordination with Agent-2 and Rhett

- Agent-2 is in the wallet/storage lane. You should not see their files in
  your diffs, and they should not see yours.
- Do NOT modify `lib/rpc/`, `lib/validation/`, `lib/consensus/`, `lib/net/`,
  `lib/script/` — those are Rhett's.
- Surprising discoveries → add a paragraph under `## Notes from Agent-3` at
  the end of this file rather than expanding scope silently.

## Done criteria

- All P1.3, P1.4, P1.8, P1.9, P1.10, P1.11 rows in `AGENT.md` show `done <SHA>`.
- `make ci` green on the final pushed commit.
- At least one new test in `lib/test/` per bug class (NULL-VK reject, bad-params
  reject, non-canonical-S reject).
- No new files created outside `lib/crypto/`, `lib/sapling/`, `lib/keys/`,
  `lib/test/`.
- No secret material appears in any `LOG_*` call.
