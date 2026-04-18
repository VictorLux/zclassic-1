# AGENT-3 — Cryptography & Sapling Hardening

**Derived from:** the 2026-04-17 full code review. See `AGENT.md` for the
cross-agent priority table. Last brief rewrite: 2026-04-17.

**Working directory:** `~/zclassic23-3` (separate git clone; pushes to `origin/main`).
**Coordinator:** Rhett (`~/zclassic23`).
**Sibling:** Agent-2 (`~/zclassic23-2`), in the wallet / storage / app-layer lane.

---

## Lane — what you may edit

**Full edit access:**
- `lib/crypto/`
- `lib/sapling/`
- `lib/keys/`
- `lib/test/` — only to add/modify tests covering your changes

**Full edit access (additional, per 2026-04-18 narrow expansion):**
- `lib/core/src/random.c` (P1.16 only — the root-cause fix for the
  `GetRandBytes` fail-open you flagged during P1.15)

**Read-only / off-limits:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/` — Agent-2's lane
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/` — Agent-2's lane
- `tools/mcp/` — Agent-2's lane (MCP JSON-injection fixes are at P3.1/P3.2)
- `lib/rpc/`, `lib/validation/`, `lib/consensus/`, `lib/net/`,
  `lib/script/` — Rhett's lane
- `lib/core/` — Rhett's lane EXCEPT for `random.c` per the line above
- `vendor/` — Rhett (submodule pins, CVE cherry-picks) — you may *read*
  constant-time reference implementations under `vendor/tor/src/ext/ed25519/`

---

## Current status — 2026-04-18

**Done and on main (10 rows):** P1.3, P1.4, P1.8, P1.9, P1.10, P1.11,
P1.11b, P1.12, P1.13, P1.14, P1.15. AGENT.md shows the SHAs.

Every row in your original brief plus the entire Wave 2 (curve25519 CT,
ed25519 CT, RNG hygiene) is now closed.

**Now working on:** P1.16 — close the `lib/core/random.c` root cause
you flagged in the P1.15 commit message. See NOW below.
**Queued:** prf.c nullifier-path timing audit, then optionally formal
verification scaffolding for `sapling_check_*`. See NEXT.

---

## NOW — P1.16: close the `lib/core/random.c` GetRandBytes fail-open

File: `lib/core/src/random.c`

This is the root cause your P1.15 wrapper papers over. `GetRandBytes`
can silently return all-zero bytes on certain failure paths (e.g.
`/dev/urandom` open fails, `getrandom()` returns short, etc.) — every
caller that doesn't go through your new `zcl_random_secret_bytes`
wrapper still believes "true means I got entropy" when the actual
result is a deterministic zero buffer.

**Fix.** Audit every error path inside `GetRandBytes` (and any sibling
functions in this file). For each:

- If the syscall fails: don't silently fill with zeros — return
  `false` AND `LOG_FAIL` AND make sure the buffer is left in a state
  the caller can detect (zero-fill is not detectable; consider an
  explicit `0xFF` fill or a separate sentinel pattern that's then
  re-checked at every secret-derivation site).
- The cleanest fix is to make the error code unambiguous: return
  `bool false` only when the buffer was NOT filled with entropy, and
  abort the process if the caller ignores it (assertion in test
  builds, `LOG_FAIL` + caller-side panic in production paths).

**Acceptance.** Add a test that injects a `getrandom()` failure
(LD_PRELOAD shim or `#ifdef ZCL_TEST_FORCE_RNG_FAIL`) and asserts
every secret-derivation path either returns failure or aborts —
never proceeds with zero bytes.

When P1.16 lands, also flip every remaining direct `GetRandBytes`
call site to `zcl_random_secret_bytes` so the wrapper is the only
public path. Update AGENT.md row P1.16 to `done <SHA>`.

**Scope boundary.** Only `lib/core/src/random.c` and the per-call-site
migration. Do not touch any other file under `lib/core/`.

---

## NEXT (pre-authorized) — prf.c nullifier-path timing audit

After P1.16 lands. File: `lib/sapling/src/prf.c`.

**Threat model.** Sapling nullifiers are computed from `(nk, ρ)` where
`nk` is a long-lived secret (one per spending key, reused across all
spends from that key). Any cache-timing leak on `prf_nullifier`
correlates across many spends — much more dangerous than a one-shot
side channel.

**Pattern.** Same plan as P1.12/P1.13:

1. Audit every secret-dependent branch / table lookup in `prf.c`.
2. Replace with masked / branchless equivalents.
3. Diff test (10k random inputs, old-vs-new bit-for-bit match).
4. Timing test (varying nullifier secret entropy, assert no
   correlation in mean/stddev).

Reference: the masked-linear-scan pattern you built for `jub_scalar_mul`
in P1.12 carries over directly.

### Stopping point

After P1.16 + the prf.c audit are both on main, ping Rhett. Likely
next: (a) formal-verification scaffolding for `sapling_check_spend` /
`sapling_check_output` — small-enough spec that a CBMC / cppcheck-style
tool could exhaustively cover the decode/reject paths, or
(b) join Rhett on the P2 network DoS hardening if you want to stretch
out of crypto for a wave.

---

## Preflight — run verbatim when re-bootstrapping a stale clone

```bash
cd ~/zclassic23-3

# 1. Preserve any local-only work
git add -A
git diff --staged --quiet || git commit -m "a3: wip checkpoint before AGENT-3.md workstream"

# 2. If on a feature branch, back it up, then move to main
CURRENT=$(git branch --show-current)
if [ "$CURRENT" != "main" ]; then
    git push origin "$CURRENT" 2>/dev/null || true
    git checkout main
fi

# 3. Sync
git pull origin main --rebase=false

# 4. Confirm clean baseline
git status
git branch --show-current   # must print "main"

# 5. Read the rules and the checklist
cat CLAUDE.md
cat DEFENSIVE_CODING.md
cat AGENT.md

# 6. Confirm green baseline (STOP + report if either fails)
make -j"$(nproc)"
./test_zcl
```

---

## Commit protocol

- One logical fix per commit. Good: `sapling: LOG_FAIL on groth16
  pairing failure`. Bad: `sapling hardening`.
- Every commit: `make test` must pass. Every push: `make ci` must pass.
- Commit message format:

  ```
  <subsystem>: <one-line summary>

  <why — cite file:line from AGENT.md>

  Fixes P1.X (AGENT.md checklist).
  ```

- After each push, update the row in `AGENT.md`:
  `open` → `in-progress` → `done <SHA>`.
- Push frequently — **except for P1.12**, where the rule is invert:
  draft, review, iterate on a branch; merge only once both the diff
  test and the timing test pass.
- Never `--amend` a pushed commit. Never `--force-push`.
- **Never log secret material.** No secret keys, no nonces, no plaintext
  notes. Hashes and public values only.

---

## Coordination rules

- Agent-2 is in wallet / storage / app / tools/mcp. You should not see
  their files in your diff. If you do, stop and check.
- If you need something Rhett owns (validation fix, vendor update), note
  it in your commit message and wait — don't front-run.
- Surprising out-of-scope discoveries → append to the "Notes from
  Agent-3" section at the end of this file. Do not expand scope silently.

---

## Notes from Agent-3

_(append new observations here as work proceeds — keep dated, short,
and flag the owner of anything out of lane.)_

### 2026-04-17 — Wave 2 closeout

- **Step H (curve25519)**: no implementation change required. The
  TweetNaCl Montgomery ladder is constant-time by construction;
  branchless `sel25519` cswap, no precomputed tables, deterministic
  loop counts. Audit conclusion is captured as a header comment in
  `lib/crypto/src/curve25519.c`, and a Hamming-weight regression
  timing test was added to `lib/test/src/test_sapling.c` (lo-vs-hi
  scalar weight ratio must stay within ±15%).
- **Step I (ed25519)**: this file is verify-only. There is no
  `ed25519_sign` in the tree (consensus paths only need verify;
  signing is RedJubjub in `lib/sapling`). Verify operates on public
  inputs only, but the existing impl already uses cswap-driven
  scalarmult and an XOR-OR diff check — CT regardless. Audit comment
  in `lib/crypto/src/ed25519.c` documents this and pre-warns about
  future sign-side requirements.
- **Step J (RNG hygiene)**: literal grep across `lib/crypto/` and
  `lib/sapling/` for `rand`/`random`/`drand48`/`srand*`/`arc4random`
  and weak seeds (`time(NULL)`, `getpid`) found **zero** hits — both
  trees source secret bytes only via `GetRandBytes`. However see the
  out-of-lane finding immediately below.

### 2026-04-17 — Out-of-lane finding (FOR RHETT, lib/core/)

`lib/core/src/random.c:13-27` (`GetRandBytes`) **silently zero-fills**
the output buffer if `open("/dev/urandom")` fails. Every call site in
`lib/sapling/` and `lib/crypto/` ultimately depends on this function —
including ephemeral DH secrets (`secure_channel.c`), Sapling note
randomness (`rcm`/`rcv`/`esk`/`ar`), Groth16 proof blinding factors,
RedJubjub signing nonces, and AEAD nonces in `sha3_crypt`. A failed
open() therefore turns into a same-key-everywhere catastrophe with no
log line and no caller-visible signal.

In-lane mitigation landed as Step J: a defensive
`zcl_random_secret_bytes` wrapper (`lib/crypto/include/crypto/random_secret.h`)
that detects the all-zero output, scrubs it, and `LOG_FAIL`s. All
in-lane secret-generation call sites were migrated. This catches the
failure mode but leaves callers in `lib/net/src/secure_channel.c`
(your lane) still using raw `GetRandBytes`.

Suggested upstream fix in `lib/core/src/random.c`:
1. Try `getrandom(2)` first (works in chroots; no FD needed).
2. Fall back to `/dev/urandom` if `getrandom` is unavailable.
3. On total failure: `LOG_FAIL`-equivalent (the function is `void`
   today; consider returning `bool` and propagating, or `abort()` in
   the absolute worst case rather than handing out predictable bytes).
4. Retry on `EINTR` rather than bailing on the first short read.

`lib/core/` is shared infrastructure, so I am leaving the source
unchanged and flagging here.
