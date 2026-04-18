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

## Current status — 2026-04-19 (evening, P1.7 on main)

**Done and on main (15 rows):** P1.3, P1.4, P1.6 (`f6aa0b080`),
P1.7 (`5ce252bb6`), P1.8, P1.9, P1.10, P1.11, P1.11b, P1.12,
P1.13, P1.14, P1.15, P1.16 (`94d607b85`), P1.16b (`c841defd2`).

P1.7 deleted the window_clean + skip_diffbits goto/label from
`contextual_check_block_header`.  Every call into that function now
runs the GetNextWorkRequired check; incomplete-window nodes
compare against nProofOfWorkLimit (the weakest permitted compact)
rather than blindly trusting the header's nBits.  Fast-sync / MMB
callers that need to accept headers without local-window validation
must bypass the function entirely — process_block.c:732-734's
pre-existing skip_contextual gate already does this for distant-IBD
blocks, so no new call-site changes were needed.  Regression test
in test_chain.c asserts that nBits=0x1d00ffff (Bitcoin limit) is
rejected against the Zcash expected of 0x1f07ffff with
"bad-diffbits".

**Now working on:** P5.5 — vendor/tor submodule pin bump
(promoted from NEXT → NOW).  This is the last Agent-3 row.

**Queued NEXT (pre-authorized):** nothing after P5.5.  Once it
lands, ping Rhett for the next wave (see "Stopping point" below).

---

## NOW — prf.c nullifier-path constant-time audit

File: `lib/sapling/src/prf.c`.

**Threat model.** Sapling nullifiers are computed from `(nk, ρ)`
where `nk` is a long-lived secret (one per spending key, reused
across all spends from that key). Any cache-timing leak on
`prf_nullifier` correlates across many spends — much more dangerous
than a one-shot side channel.

**Pattern.** Same plan as P1.12/P1.13:
1. Audit every secret-dependent branch / table lookup in `prf.c`.
2. Replace with masked / branchless equivalents.
3. Diff test (10k random inputs, old-vs-new bit-for-bit match).
4. Timing test (varying nullifier secret entropy, assert no
   correlation in mean/stddev).

Reference: the masked-linear-scan pattern you built for
`jub_scalar_mul` in P1.12 carries over directly.

**Acceptance.** Diff test passes + timing test's Hamming-weight
stddev ratio stays within ±15% across entropy buckets (matching the
P1.12/P1.13 regression pattern).

---

## NEXT — queue, in order (pre-authorized)

### NEXT[1]: P1.6 — P2SH sigop accounting (CONSENSUS-SENSITIVE)

File: `lib/validation/src/sigops.c:10-18`.

**Bug.** The sigop counter (used by block-size/sigop-limit consensus
rules) does NOT account for sigops inside P2SH redeem scripts. A
miner/attacker can construct a block whose raw script sigops are
within limits but whose P2SH redeem scripts push the real sigop count
far past `MAX_BLOCK_SIGOPS`. zclassicd (the legacy peer) counts these
correctly, so our node will accept blocks that zclassicd rejects →
**consensus split**.

**Reference.** The correct algorithm is in zclassicd's
`src/main.cpp::GetTransactionSigOpCount` (it walks the `vin` and,
for each input whose prev output is a P2SH script, parses the
redeem script out of the scriptSig and counts sigops inside it).
Mirror that logic byte-for-byte; do not invent a new algorithm.

**Fix.**
1. Audit `lib/validation/src/sigops.c` for every existing sigop
   counter function. Identify the one(s) called during block
   acceptance.
2. Add the P2SH path: when counting sigops for an input, if the
   prevout scriptPubKey is P2SH, parse the last push in the
   scriptSig as a redeem script and count sigops inside it.
3. The `MAX_P2SH_SIGOPS=15` per-input cap matches Bitcoin/Zcash
   — cap per-input to avoid unbounded work.

**Acceptance (non-negotiable for consensus rows):**
1. **Parity test against zclassicd.** Construct 10 real blocks from
   the live chain that contain P2SH inputs. For each, run the
   zclassic23 sigop counter AND the zclassicd RPC
   `getblock/getrawtransaction + decodescript` path to count sigops
   independently. Counts MUST match exactly.
2. **Regression test.** A synthetic block with one P2SH input whose
   redeem script contains 16 `OP_CHECKSIG` ops: zclassic23 must
   reject it (>15 P2SH sigops per input).
3. **No chain fork on live data.** After deploy, the node must stay
   in sync with zclassicd-rhett (`zcl_status` height matches legacy
   peer height within 1 block for 10 minutes).

**STOP + ping Rhett triggers:**
- Any change to the serialized block/tx format
- Any change to consensus constants (`MAX_BLOCK_SIGOPS`,
  `MAX_P2SH_SIGOPS`, etc. — these must match Zcash exactly)
- If your parity test finds a block where zclassic23 and zclassicd
  DISAGREE on sigop count — stop, because either our counter is
  wrong OR zclassicd's is, and either way Rhett needs to decide
  before we ship

**Commit message template:**
```
val: count P2SH redeem-script sigops in block acceptance (P1.6)

Fixes P1.6. Consensus-split risk: zclassicd counts P2SH redeem
script sigops, zclassic23 did not. Mirrors zclassicd
src/main.cpp:GetP2SHSigOpCount (MAX_P2SH_SIGOPS=15 per input).
10-block parity test against legacy peer + synthetic 16-sigop
rejection test in test_validation.c.
```

### NEXT[2]: P1.7 — remove skip_diffbits difficulty-check escape hatch

File: `lib/validation/src/check_block.c:222,233-250`.

**Bug.** There's a `skip_diffbits` flag that, when true, silently
skips the difficulty (`nBits`) check in `CheckBlockHeader`. Any code
path that sets `skip_diffbits=true` can let a block with an invalid
PoW target through consensus. Auditing the call sites shows the flag
is usually set to `true` during "trust-me" paths like fast-sync
headers — but the intent is to trust the MMB proof, not to skip PoW
entirely. Current implementation skips BOTH, which is a real hole.

**Fix.** Remove the `skip_diffbits` parameter entirely. Difficulty
is always checked. If fast-sync / MMB needs to accept headers
without a full validity check, it must do so BEFORE calling
`CheckBlockHeader` (e.g. via a separate "mmb_verify_header" path
that validates MMB-proof equivalence and separately enforces the
difficulty target).

**Acceptance:**
1. The `skip_diffbits` parameter no longer exists.
2. Every previous call-site now either (a) validates difficulty
   inline via a local check, OR (b) documents in a comment why the
   path is safe without the `check_block` call.
3. A synthetic block with correct Merkle / timestamp / sigops but
   `nBits` = 0x1d00ffff (trivially-low difficulty) is REJECTED
   on every path that previously could have accepted it.

### NEXT[3]: P5.5 — vendor/tor submodule pin + .onion smoke test

File: `vendor/tor` submodule.

**Bug.** `git submodule status vendor/tor` shows `+73bd405d1b...`
— the `+` prefix means the submodule HEAD differs from the pinned
commit. Either the pin is stale (work landed on the dynhost fork
that should be recorded) or the working tree is dirty (needs
`git submodule update`).

**Fix.**
1. `cd vendor/tor && git log origin/main..HEAD` — see what's
   actually ahead.
2. If the ahead commits are valid (real work on the fork): bump
   the pin by `cd ~/zclassic23-3 && git add vendor/tor && git
   commit -m "vendor: bump tor submodule pin (P5.5)"`.
3. If the ahead commits are local noise: `git submodule update
   vendor/tor` to reset.
4. Rebuild clean: `make clean && make -j$(nproc)`.
5. **Smoke test (mandatory):** `make deploy`, then via MCP run
   `zcl_onion_status` — verify the .onion address bootstraps
   within 60 seconds AND `zcl_status.health.checks.onion_service_ready
   == true`. If either fails, the pin change broke the embedded
   Tor — revert immediately.

**Acceptance:**
- `git submodule status vendor/tor` shows no `+` prefix (clean).
- `./test_zcl` passes (includes existing Tor tests in
  `test_tor.c`).
- Live deploy with `-tor` flag produces a working .onion (verified
  via `zcl_onion_status`).

**STOP + ping Rhett trigger:** if the pin bump breaks Tor
bootstrap in any way, revert the commit and ping — do not try to
patch the Tor fork yourself.

---

## Stopping point

After prf.c + P1.6 + P1.7 + P5.5 are all on main, you will have
closed every Agent-3-owned row on AGENT.md. At that point ping
Rhett — likely next wave is either formal-verification scaffolding
for `sapling_check_spend` / `sapling_check_output` (deep consensus
review), or a new wave of post-AGENT.md items from the next live-
node review.

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

### 2026-04-19 — P1.6 shipped mirror-of-zclassicd; per-input 15-cap flagged for Rhett

P1.6 (P2SH redeem-script sigop accounting) landed in
`lib/validation/src/sigops.c` + `lib/script/src/script.c` +
`lib/validation/src/connect_block.c`.  The implementation mirrors
zclassicd `src/main.cpp::GetP2SHSigOpCount` + `src/script/script.cpp::
CScript::GetSigOpCount(flags, scriptSig)` byte-for-byte: walk each
non-coinbase input, resolve prevout, if P2SH then count sigops in
the last-pushed redeem-script payload (accurate mode), add to the
aggregate.  Only consensus check is `aggregate > MAX_BLOCK_SIGOPS
(20000)`, identical to zclassicd `ConnectBlock` at main.cpp:2634-2637.

**Brief divergence (flagged to Rhett):** AGENT-3.md NEXT[1] test 2
asks for block-level rejection when any single P2SH input's redeem
script has >15 sigops.  That would be STRICTER than zclassicd —
`MAX_P2SH_SIGOPS=15` in zclassicd is enforced only at policy /
standardness (`AreInputsStandard` in main.cpp:882-884, via
`IsStandardTx`), not at ConnectBlock.  Shipping the per-input
consensus cap would create a NEW consensus divergence where we
reject blocks zclassicd accepts — which is the exact scenario the
brief's STOP+ping trigger warns about ("zclassic23 and zclassicd
DISAGREE on sigop count → stop").  I therefore shipped only the
mirror-of-zclassicd piece (closes the real consensus-split risk
where we under-counted) and did NOT add the per-input 15-cap.  If
Rhett wants the 15-cap as a standardness/mempool rule, the natural
home is mempool acceptance in Agent-2's lane (`lib/net/src/msg_tx.c`
or equivalent), not my lane.  Waiting for Rhett's call before
touching anything further here.

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
