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

**Read-only / off-limits:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/` — Agent-2's lane
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/` — Agent-2's lane
- `tools/mcp/` — Agent-2's lane (MCP JSON-injection fixes are at P3.1/P3.2)
- `lib/rpc/`, `lib/validation/`, `lib/consensus/`, `lib/net/`,
  `lib/script/` — Rhett's lane
- `vendor/` — Rhett (submodule pins, CVE cherry-picks) — you may *read*
  constant-time reference implementations under `vendor/tor/src/ext/ed25519/`

---

## Current status — 2026-04-17

**Done and on main:**

| Row | What | SHA |
|---|---|---|
| P1.3 | Sapling verify fail-open on NULL VK | 3b4b08ba9 |
| P1.4 | Sapling params SHA-512 integrity check | 785db18b1 |
| P1.8 | Ed25519 S<L canonicality | c510c7335 |
| P1.9 | RedJubjub S<Fs canonicality | 8440cd864 |
| P1.10 | find_group_hash abort-on-fail (fixed generators) | e221e0212 |
| P1.11 | LOG_FAIL audit across lib/crypto + lib/sapling | ca139a5ad (+ 8 predecessors) |
| P1.11b | esk nonce-reuse sanity guard (Step 8) | 909636215 |
| — | Regression tests for P1.3/P1.4/P1.8/P1.9 rejection paths | ebc470342 |
| — | FIXME comment flagging jub_scalar_mul as not-constant-time | ecd24894e (to be removed by P1.12) |

P1.11 scope note: `fr.c`, `fr_avx512.c`, `equihash_solver.c`,
`sha3_avx512.c` internal math helpers intentionally left un-logged —
their `return false` is "out-of-range input" by algorithmic design, not
a security event. See `fr.c` header for the rationale.

**Now working on:** Wave 2 — curve25519 + ed25519 CT audits + RNG hygiene (P1.12 done). See NOW below.
**Queued:** Follow-on nullifier / prf timing audit — see NEXT below.

---

## P1.12 — DONE (2026-04-17)

Shipped as `15218ba2f sapling: constant-time jub_scalar_mul (P1.12)`:
masked linear-scan table select + unconditional-add-with-mask; diff
test against the old implementation + scalar-weight timing test, both
now permanent in `lib/test/src/test_sapling_crypto.c`. The FIXME
comment at ecd24894e has been removed as part of the commit.

---

## NOW — Wave 2: curve25519 + ed25519 constant-time + RNG hygiene

Same iteration protocol as P1.12 — for CT audits, diff test +
timing test are mandatory before merge. For RNG hygiene, small
per-call-site commits are fine.

### Step H — `lib/crypto/src/curve25519.c` constant-time audit

Same hazard class as P1.12, one layer down. This file implements
Curve25519 scalar mult on paths that touch secret material (X25519 DH,
ephemeral-key derivation for message encryption). Audit every table
lookup and every `if (bit)` / `if (nibble)` for secret-dependent
branching.

Reference CT implementations: `curve25519-donna`, `ref10/`, `tweetnacl`
— all well-studied and easy to diff against.

Reuse the diff-test + timing-test harness you built for P1.12, scoped
to the curve25519 functions you change.

### Step I — `lib/crypto/src/ed25519.c` full constant-time pass

You already touched this file for P1.8 (S<L canonicality) and P1.11
(LOG_FAIL on verify mismatch). Now the systematic timing audit:

- `ed25519_sign`: secret key `a` must not influence timing
- `ed25519_sign_open` (verify): public keys only, lower priority —
  audit for belt-and-suspenders
- Any internal `ge_scalarmult_base` call: Ed25519 base-point mult has
  a well-known CT pattern; confirm the project's vendored copy is
  one of `donna` / `ref10` (both CT) and not a rolled-own variant

### Step J — RNG hygiene grep across lib/crypto/ + lib/sapling/

Grep both trees for:

- `rand()`, `random()`, `drand48()`, `srand*()` — non-crypto PRNGs
  must not appear on any path touching secrets
- `getrandom(...)` — every call must have a fallback to `/dev/urandom`
  OR assert on failure; never silently continue with zero bytes
- `arc4random` — should be absent on Linux targets
- Weak seed sources: `time()`, pid-only, jitter-alone — not on
  consensus or secret-generation paths

For every hit that isn't clearly justified, commit a fix or a LOG_FAIL
upgrade. Never log the random bytes themselves.

### Stopping point

After H + I + J are all on main, ping Rhett. Likely next:
(a) audit `lib/sapling/src/prf.c` for timing leaks in the nullifier
    path (nullifiers are a correlation hazard — same key across many
    notes, so any side channel on the nullifier derivation is much
    more dangerous than it looks),
(b) formal-verification scaffolding for `sapling_check_spend` /
    `sapling_check_output` — there's a small-enough spec that a CBMC
    / cppcheck style tool could exhaustively cover the decode/reject
    paths,
(c) join Rhett on P2 DoS hardening if the network-lane work is open.

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
