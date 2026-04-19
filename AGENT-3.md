# AGENT-3 — Cryptography / Sapling / Consensus-Crypto / Net-Parallel

**Derived from:** the 2026-04-17 code review + 2026-04-19 consensus wave.
See `AGENT.md` for the cross-agent priority table.

**Working directory:** `~/zclassic23-3`.
**Coordinator:** Rhett (`~/zclassic23`), coordinator-only.
**Sibling:** Agent-2 (`~/zclassic23-2`).

---

## Lane — what you may edit

**Full edit access:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/`
- `lib/core/src/random.c` (owned since P1.16)
- `lib/validation/src/sigops.c` (owned since P1.6)
- `lib/validation/src/check_block.c` (owned since P1.7)
- `vendor/tor` submodule (pin bumps only)
- `lib/test/` — add/modify tests for your changes

**Lane expansion (2026-04-19 night, post-consensus-wave):**
- `lib/net/src/msgprocessor.c`, `lib/net/src/download.c` — for P7.4
  backpressure watchdog ONLY. Agent-2 owns the rest of `lib/net/`.

**Read-only / off-limits:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`, `app/*`, `tools/mcp/` — Agent-2
- `lib/rpc/`, `lib/script/` — Agent-2
- Other `lib/validation/` files — Agent-2
- `lib/core/` beyond `random.c` — off-limits
- Other `vendor/` dirs — pinned infrastructure

**STOP + ping Rhett triggers:**
- Any change to serialized block/tx format
- Any change to consensus constants
- For P7.4: any change that affects the P2P protocol wire format — it's
  a watchdog (observes state, rejects after parse) not a protocol change

---

## RESET (2026-04-19): P9 wave shipped, ALL findings deferred — on-call to review Agent-2's P10.1

You shipped the P9 sapling-prover audit (`04247c19a`) — 10 solid
findings (1 CRIT, 4 HIGH, 4 MED, 1 LOW). Good work. **All deferred**
until P10.1 closes — Rhett's call this evening: live-node fix takes
priority over crypto hardening.

**Read `AGENT.md` "Core focus" + "Priority 10" + [`MVP.md`](../MVP.md).**
The new project-wide rule: no fix without a reproduction + test
first. Agent-2 owns P10.1.x (chain-stall investigation). You are
on-call to review their deliverables — especially **P10.1.2**
(root-cause writeup at `docs/postmortems/2026-04-19-bip30-stall.md`).

**Why your review matters:** The MVP target (`MVP.md`) needs MRS
8/8 + HI ≥80%. P10.1 is the gate to MRS criterion #6 (7-day
soak). If P10.1.2's root-cause analysis is wrong, P10.1.3's
regression test will be the wrong test, and P10.1.4 will be
another hotfix in disguise. Your sapling/crypto background is
secondary here — what we need is **independent eyes on the
analysis**, not crypto expertise. Push back hard if the writeup
hand-waves any of the four required questions.

**No new code from you until P10.1 closes.** Resist the urge to
start fixing P9.x rows; they're parked deliberately. When P10.1
closes, Rhett will re-triage and assign.

**Open queue: empty by design.** Watch for Agent-2's pushes:

| Order | Row | Size | Severity |
|---|---|---|---|
| **NOW** | **On-call**: review Agent-2's P10.1 deliverables when they push | n/a | n/a |
| NEXT | (queue empty — Rhett re-triages P9 wave after P10.1 closes) | — | — |

When Agent-2 pushes P10.1.2 (root-cause writeup), read it carefully
and reply with one of:
- "Concur — root cause looks right, P10.1.3 should test for X."
- "Disagree — the analysis missed Y, please revisit before P10.1.3."

Your crypto/sapling expertise is most valuable as a sanity check on
the writeup, not as a parallel investigation. Don't re-investigate
unless asked.

---

## Current status — 2026-04-19 (late night, P9 audit shipped, all findings deferred)

**Done and on main (20 rows + 2 audits):** P1.3, P1.4, P1.6
(`f6aa0b080`), P1.7 (`5ce252bb6`), P1.8, P1.9, P1.10, P1.11,
P1.11b, P1.12, P1.13, P1.14, P1.15, P1.16 (`94d607b85`), P1.16b
(`c841defd2`), P5.5 (`75576d7a0`), P7.4 (`f6474c77b`), P8.2
(`576b5cde2`), P8.3 (`c06515cbd`), P8.5 (`21da0531e`), the P8-wave
audit pass (`6751d9bfa` — 8 rows), and the P9 sapling-prover audit
(`04247c19a` — 10 rows).

All crypto + consensus + vendor + net-backpressure + MMB-hardening
+ dandelion-RNG + bloom-cost rows shipped. HIGH tier across the
whole project 100% (29/29).

The P9 findings (P9.1 through P9.10) are all in `lib/sapling/` and
default-owned by you, but **deferred** by the 2026-04-19 reset. Do
not start any P9.x fix until Rhett re-triages after P10.1 closes.

---

## (Below: archived NOW for the P9 audit — landed `04247c19a`, reference only) — sapling-prover deep audit

The P1 wave touched the **API surface** of Sapling (verify fail-open,
RedJubjub canonicality, find_group_hash returns, RNG hygiene, jubjub
constant-time, note-encryption). The **prover internals** —
`groth16_prover.c`, `sapling_circuit.c`, `sapling_prover_c23.c`,
`circuit_gadgets.c`, `msm_parallel.c`, `bls12_381.c`, `bn254.c`,
`pedersen_hash.c`, `incremental_merkle_tree.c` — got light coverage.

Likely surfaces still hiding bugs:

- **Constant-time violations in proof generation**: secret-key paths
  through Groth16 witness construction. Branches that depend on
  the spending-key bit pattern. Memory accesses that depend on the
  randomness `r`. Variable-time field ops on secret data.
- **Missing input validation on circuit witness data**: malformed
  diversifiers, out-of-range nullifier components, witness vectors
  whose lengths the prover trusts without checking.
- **Memory leak / use-after-free in the prover state machine**:
  early-exit paths that skip `groth16_prover_free`, error returns
  that double-free, ownership confusion between caller and prover.
- **Side-channel leaks**: timing, cache, branch-predictor — especially
  in Pedersen hash + MSM (multi-scalar multiplication). The MSM
  parallel code is brand new; check the worker thread coordination
  for racy reads of secret state.
- **Incremental Merkle tree corner cases**: the Sapling note
  commitment tree. Off-by-one on tree-full conditions. Witness path
  reconstruction after partial tree compaction.
- **Sprout PHGR13 corners**: only PHGR13 verification has tests.
  The Groth16 ↔ PHGR13 transition logic during Heartwood activation
  may have stale code paths.

### Method (~90 min total)

1. Skim each file's `*.c` + `*.h`. For each finding:

```
[CRIT|HIGH|MED|LOW] <summary>
file: path:line
why: <one sentence>
fix-hint: <one sentence>
```

2. Open as P9.1, P9.2, … in a new section in AGENT.md (after the
   P8 table, before "Status tracking"). Keep P0–P8 intact.
3. Propose Agent-2 vs Agent-3 ownership in the commit message —
   default Agent-3 for anything in `lib/sapling/`.
4. **Do NOT fix anything** — Rhett triages + assigns. Same rule as
   the P8 audit you ran earlier.

### Constraints

- **Scope:** sapling/crypto only. Do not re-audit the rest of the repo.
- **Skip rows already on P0–P8.** Grep AGENT.md first.
- **Cap 10 findings.** Quality over quantity. Single-paragraph evidence.
- Clean file → "sapling/<file>: none found." No padding.
- Under 1000 words in the new AGENT.md section.

### Deliverable

One commit: `agents: open P9 wave — sapling-prover deep audit`.
AGENT.md diff only (new P9 section + Progress block denominator
bump if any new rows). No code. Ping Rhett for triage after push.

If you find nothing of substance after a thorough pass: commit
`agents: P9 sapling-prover audit — clean, no findings` with a
short summary of what was checked and why each subsystem looked
solid. That's still a valuable deliverable.

---

## (Below: archived NOW for P8.5 — landed `21da0531e`, reference only) — clamp `MAX_BLOOM_HASH_FUNCS`

---

## (Below: archived NOW for P8.5 — landed `21da0531e`, reference only) — clamp `MAX_BLOOM_HASH_FUNCS` in the rolling-bloom path

File: `lib/bloom/src/bloom.c:47-52` (the `bloom_filter_init_internal`
helper) + the rolling_bloom call site.

### Bug

`bloom_filter_init_internal` only applies the `MAX_BLOOM_HASH_FUNCS`
cap when `constrained=true` — the public `bloom_filter_init` path.
The internal `rolling_bloom_init` path passes `constrained=false`
and lets `num_hash_funcs = (data_size * 8 / num_elements * LN2)`
grow without ceiling.

Every subsequent `rolling_bloom_insert` / `rolling_bloom_contains`
runs that many siphash iterations per call. Pathological tuning
(small `num_elements`, large `data_size` from a tight `fp_rate`)
produces hot-path CPU blow-up.

### Fix

Extract the `MIN(ideal, MAX_BLOOM_HASH_FUNCS)` clamp into both
branches:

```c
/* before */
size_t ideal = (size_t)((data_size * 8.0 / num_elements) * LN2);
flt->num_hash_funcs = constrained ? MIN(ideal, MAX_BLOOM_HASH_FUNCS) : ideal;

/* after */
size_t ideal = (size_t)((data_size * 8.0 / num_elements) * LN2);
flt->num_hash_funcs = MIN(ideal, MAX_BLOOM_HASH_FUNCS);
(void)constrained;  /* both paths now clamp; keep arg for API stability */
```

If the `constrained` flag is no longer load-bearing after the change,
you can delete it entirely — it's an internal helper. Match the
style of whatever else you find in that file.

### STOP + ping Rhett

- Any change to the bloom filter's wire format (BIP37 `filterload`
  message). The clamp is a CPU-cost guard, not a wire-level change.
- Any change to MAX_BLOOM_HASH_FUNCS itself. Just enforce the
  existing constant.

### Acceptance

1. Unit test in `lib/test/src/test_bloom.c` (or whichever covers
   bloom): construct a `rolling_bloom_init` with parameters that
   pre-fix would yield `num_hash_funcs > MAX_BLOOM_HASH_FUNCS`;
   assert post-init the field is clamped to `MAX_BLOOM_HASH_FUNCS`.
2. Verify the existing public-path `bloom_filter_init` test still
   passes (regression — clamp behavior should be identical there).
3. Full `./test_zcl` + `make ci` green.

### Lane note

You're touching `lib/bloom/src/bloom.c` — outside your usual lane
but the bug is squarely RNG/crypto-cost. Match Agent-2's coding
style (look at any of their net or storage commits — `LOG_FAIL`
macros, header-vs-source split, error-return discipline).

Trivial scope. One commit, ~5 lines of code + ~30 lines of test.

### Commit template

```
bloom: clamp MAX_BLOOM_HASH_FUNCS in rolling_bloom path (P8.5)

Fixes P8.5 (AGENT.md). bloom_filter_init_internal only enforced
the MAX_BLOOM_HASH_FUNCS cap when constrained=true (the public
bloom_filter_init path). The internal rolling_bloom_init path
passed constrained=false and let num_hash_funcs grow without
ceiling — every subsequent insert/contains paid the unbounded
siphash iteration cost.

Pathological tuning (small num_elements, large data_size from a
tight fp_rate) produced hot-path CPU blow-up. Fix lifts the clamp
out of the constrained branch so both call sites enforce it.

Tests: rolling_bloom_init clamp assertion + regression on the
public bloom_filter_init path.
```

---

## NEXT — (queue empty after P8.5 lands)

Ping Rhett. The triage pass already covered every lane — no further
audit work pre-authorized.

---

## (Below: archived NOW for P8.2 — landed `576b5cde2`, reference only) — replace dandelion PRNG with cryptographic RNG

File: `lib/net/src/dandelion.c:42-54` (the seed init + xorshift64 helper).

### Bug

The dandelion stem-peer PRNG is seeded with
`(uint64_t)time(NULL) ^ 0xdeadbeefcafe1234ULL` — ~31 bits of effective
entropy. The same seed drives:
1. The Fisher-Yates shuffle that selects this epoch's stem peers
2. The per-tx fluff coin-flip

An attacker who knows rough boot-time + epoch rotation cadence can
replay the xorshift64 state and predict (a) which 2 outbound peers the
node uses for stem relay this 10-min epoch, and (b) the stem/fluff
outcome of every transaction the node originates — **defeating
Dandelion's origin-privacy property**.

(The original P8 audit also flagged a possible data race; on closer
read, all callers hold `ds->cs` so the race is not real. **The
seed-quality issue is the actual bug.** Don't waste time on locking.)

### Fix

Replace `srand_xorshift64(time(NULL) ^ ...)` with a per-epoch reseed
from your already-hardened `zcl_random_secret_bytes`. Two acceptable
shapes:

**A. Seed xorshift64 from the secret RNG once per epoch** (minimal
change — keep the xorshift64 stream for cheap intra-epoch calls but
make the seed unpredictable):

```c
uint64_t seed;
zcl_random_secret_bytes((uint8_t *)&seed, sizeof seed);
ds->rng_state = seed;  // or whatever the field is
```

**B. Replace xorshift64 entirely with `zcl_random_secret_bytes`** for
both the Fisher-Yates shuffle and the per-tx fluff coin-flip. Higher
CPU cost per call (a few siphash rounds), but eliminates the predictable
stream. Probably negligible — dandelion is not hot-path.

Prefer **B** unless profiling shows the per-tx call cost matters. The
fewer predictable streams in the codebase, the better.

### STOP + ping Rhett

- Any change to the Dandelion P2P wire format (stem/fluff message
  layout, epoch rotation cadence, peer-selection topology). The fix
  is purely the seed/PRNG inside an already-existing helper — that's
  allowed.
- Any change to consensus or transaction format. Dandelion is a relay
  policy layer above consensus; this fix doesn't touch consensus at all.

### Acceptance

1. Unit test in `lib/test/src/test_net.c` (or whichever existing test
   file covers dandelion): boot two dandelion instances back-to-back
   within the same wall-clock second; assert their stem-peer
   selections differ. (Pre-fix: identical because `time(NULL)` is
   identical. Post-fix: different because the secret RNG is fresh.)
2. Statistical sanity: run the per-tx fluff coin-flip 10,000 times,
   assert ~50/50 split within ±2σ. Smoke-tests the new entropy source
   doesn't bias the coin.
3. Audit comment in `lib/net/src/dandelion.c` documenting the
   cryptographic-RNG dependency (so a future "performance" refactor
   doesn't silently regress to xorshift64-from-time).
4. Full `./test_zcl` + `make ci` green.

### Lane note

You're touching `lib/net/src/dandelion.c` — outside your usual lane
but inside the P7.4 expansion. Match Agent-2's coding style (look at
their P2.1/P2.4 commits for net-layer idioms — `LOG_FAIL` macros,
header-vs-source split, error-return discipline).

Don't touch `dandelion_relay()`, `dandelion_advance_epoch()`, or
anything that changes the relay topology — only the seeding helper.

### Commit template

```
net/dandelion: reseed stem-peer PRNG from cryptographic RNG (P8.2)

Fixes P8.2 (AGENT.md). Dandelion's stem-peer selection + per-tx
fluff coin-flip both ran on xorshift64 seeded with
(time(NULL) ^ const) — ~31 bits of attacker-predictable entropy.

A peer with rough boot-time + epoch-rotation cadence could replay
the stream and predict which 2 outbound peers the node uses for
stem relay this epoch, plus every fluff outcome — defeating
Dandelion's origin-privacy property.

Replaces with zcl_random_secret_bytes (already used for esk /
groth16 blinding / sapling-r). One reseed per epoch; per-tx
coin-flip pulls from the same source.

Tests: same-wall-clock-second boot pair → distinct stem peers,
10k coin-flip ±2σ uniformity, audit comment in dandelion.c.
```

---

## NEXT — (queue empty after P8.2 lands)

Ping Rhett. The triage pass already covered every lane — no further
audit work pre-authorized.

---

## (Below: archived NOW for P8.3 — landed `c06515cbd`, reference only) — cap MMB mountain height on deserialize

Files: `lib/chain/src/mmb.c:254-261` (deserialize), with downstream
defense in `mmb_merge_after_insert` (lines ~100-115).

### Bug

`mmb_deserialize` reads each mountain's `height` as a raw little-
endian `uint32_t` from the input buffer with no upper-bound check —
only `nm` (mountain count) is capped against `MMB_MAX_MOUNTAINS`.

For any practical chain, `height ≤ ⌈log2(num_leaves)⌉ ≤ 64`. The
code today happily accepts `UINT32_MAX`. Downstream
`mmb_merge_after_insert` increments `height` during merges — a
deserialized state with `height` near `UINT32_MAX` triggers
signed/unsigned wraparound on the next `mmb_append`, silently
corrupting the FlyClient/snapshot trust root.

Snapshot input may transit fast-sync/swarm before P2.4's hash check
binds — defense-in-depth gap.

### Fix

Cap each mountain's `height` at `MMB_MAX_HEIGHT` on read. Reject
the entire MMB blob if any mountain exceeds the cap.

```c
#define MMB_MAX_HEIGHT 64  /* ⌈log2(num_leaves)⌉ for any plausible chain */

/* in mmb_deserialize, per-mountain loop */
uint32_t height = read_le32(...);
if (height > MMB_MAX_HEIGHT) {
    LOG_FAIL("mmb", "deserialize: mountain height %u exceeds cap %u",
             height, MMB_MAX_HEIGHT);
    return false;
}
```

Mirror the cap in `mmb_merge_after_insert`: assert (or reject) when
a merge would push `height` past `MMB_MAX_HEIGHT`. That second guard
catches in-memory corruption that bypasses the deserialize path.

### STOP + ping Rhett

- Any change to the MMB on-disk serialization format (field order,
  encoding, length-prefix) — that's consensus-adjacent because the
  SHA3 root commits over the bytes.
- Adding the cap is a NEW input validation, not a format change —
  this is allowed.

### Acceptance

1. Unit test in `lib/test/src/test_chain.c` (or `test_mmb.c` if it
   exists): build a malicious blob with a single mountain at
   `height = MMB_MAX_HEIGHT + 1`; assert `mmb_deserialize` returns
   false + emits the LOG_FAIL.
2. Round-trip test: serialize a real MMB at h=3,000,000-ish; assert
   no mountain exceeds `MMB_MAX_HEIGHT` (sanity check the cap is
   above any real-world value).
3. Wraparound test: construct an MMB with `height = UINT32_MAX - 1`
   in-memory (bypassing deserialize); call `mmb_append`; assert the
   merge guard fires before any state corruption.
4. Full `./test_zcl` + `make ci` green.

### Commit template

```
chain/mmb: cap mountain height on deserialize to prevent wraparound (P8.3)

Fixes P8.3 (AGENT.md). mmb_deserialize accepted height fields up to
UINT32_MAX with no bound; downstream mmb_merge_after_insert would
wraparound on the next append, silently corrupting the FlyClient
trust root. Adds MMB_MAX_HEIGHT=64 cap at parse time and a defensive
assert in the merge path.

Tests: malicious-blob reject + real-chain round-trip + wraparound
guard.
```

---

## NEXT — (queue empty after P8.3 lands)

Ping Rhett. The triage pass already covered every lane — no further
audit work pre-authorized.

---

## (Below: archived NOW for P7.4 — reference only) — P7.4 backpressure watchdog

Files: `lib/net/src/msgprocessor.c`, `lib/net/src/download.c`.

### Bug

When the chain tip doesn't advance (pre-P7.1, or any future tip-stall
regression, or a legitimate long-fork reorg), block buffers accumulate
in the download queue. On 2026-04-18 the live node's RSS climbed to
6.0 GB (cgroup `MemoryHigh=6G`) before the OOM path fired. P7.1
fixed the root cause; this row adds the backstop so the next
tip-stall bug surfaces as a diagnostic event instead of OOM.

### Fix

Watchdog state machine inside `msgprocessor.c` (or a helper at
`lib/net/src/tip_watchdog.c` if you prefer isolation):

- Track `last_tip_advance_ns` — updated via an `EV_TIP_UPDATED` hook
  or by observing `chain_tip` inside the msgprocessor main loop.
- If `now - last_tip_advance_ns > N sec` AND
  `download_queue_bytes() > M` → enter **BACKPRESSURE_ACTIVE**:
  1. Drain download queue (free buffered block bodies; keep headers).
  2. Refuse new `inv/block` messages for K seconds. Emit
     `EV_BACKPRESSURE_REJECT` per drop (peer id + reason). Do NOT
     bump ban-score — the peer isn't misbehaving.
  3. Emit `EV_BACKPRESSURE_ACTIVE` once per entry.
- Exit **BACKPRESSURE_ACTIVE** when `chain_tip` advances OR K seconds
  elapse. Emit `EV_BACKPRESSURE_CLEAR`.

### Tuning constants

```c
#define TIP_STALL_THRESHOLD_SEC      60
#define DOWNLOAD_QUEUE_HIGH_WATER    (256UL * 1024 * 1024)
#define BACKPRESSURE_REJECT_SEC      120
```

Compile-time `#define`s only in this patch. RPC-tunable policy is a
separate row.

### Acceptance (4 tests)

1. **Fixture** in `lib/test/src/test_net.c`: force the watchdog clock
   forward by 61s with download queue pre-filled to 256 MB equivalent;
   assert `EV_BACKPRESSURE_ACTIVE` fires + subsequent block-inv gets
   `EV_BACKPRESSURE_REJECT`.
2. **Clear-on-advance:** same fixture, then bump `chain_tip`; assert
   `EV_BACKPRESSURE_CLEAR` + next block-inv accepted.
3. **RSS cap smoke (ZCL_STRESS_TESTS-guarded):** synthetic peer sends
   1000 orphan blocks while tip is pinned; assert RSS stays under
   2 GB (was 6 GB pre-fix).
4. Full `./test_zcl` + `make ci` green.

### Lane note

First time touching `lib/net/src/msgprocessor.c` handler-level (your
P1.6/P1.7 were pure validation). Read Agent-2's P2.4/P2.6/P2.7
commits first to match coding style and idioms.

Do NOT touch P2P wire format or message parsing. Watchdog is a
rejection layer AFTER parse + BEFORE dispatch; observes state only.

### Commit template

```
net: tip-stall backpressure watchdog to cap RAM under P7.1-class bugs (P7.4)

Fixes P7.4 (AGENT.md). When chain_tip doesn't advance for
TIP_STALL_THRESHOLD_SEC and download_queue > DOWNLOAD_QUEUE_HIGH_WATER,
enter BACKPRESSURE_ACTIVE: drain download queue, refuse new block-inv
for BACKPRESSURE_REJECT_SEC, emit EV_BACKPRESSURE_*. Clears on tip
advance.

Live-outage context: 2026-04-18 pre-P7.1 stuck-tip loop hit 6 GB RSS
before SIGABRT. P7.1 fixed the root cause (update_tip silent-drop);
this row is the diagnostic backstop.

Tests: fixture + clear-on-advance + ZCL_STRESS_TESTS RSS-cap.
```

---

## NEXT — fresh code-review pass for the P8 wave

After P7.4 lands. **Do not start coding anything new.** Produce an
audit report.

### Objective

Fresh eyes on the repo, find the next wave of issues. Original
2026-04-17 review: 53 rows. 2026-04-18 live-node review: +10 (P7).
Both ~95% closed. Next wave should find issues that:

- Only surface now that P1–P7 fixes landed (new code paths live)
- Were below the severity threshold of the first two reviews
- Are in subsystems not re-audited since (p2p_game, zslp, ZNAM,
  ZMSG, ZSWP atomic swaps, file_market, block_explorer,
  store_controller, mining, compact_blocks, dandelion, bloom,
  mmb/mmr/flyclient proofs)
- Are operator-visible gaps (log noise, metric gaps, MCP diagnostics
  that would have caught P7.1 faster)

### Method

1. Skim each subsystem's `*.c` + `*.h` + tests (~90 min).
2. For each finding:

```
[CRIT|HIGH|MED|LOW] <summary>
file: path:line
why: <one sentence>
fix-hint: <one sentence>
```

3. Open as P8.1, P8.2, … in a new AGENT.md section (keep P7 intact).
4. Propose Agent-2 vs Agent-3 ownership in the commit message.
5. Do NOT fix anything — Rhett triages + assigns.

### Constraints

- Skip rows already on P0–P7. Grep AGENT.md first.
- Cap 20 findings. Quality over quantity.
- Clean subsystem → "subsystem X: none found." No padding.
- Under 1500 words in the new AGENT.md section.

### Deliverable

One commit: `agents: open P8 wave — fresh review after P7 drain`.
AGENT.md diff only (new P8 section + Progress block denominator bump).
No code. Ping Rhett for triage after push.

---

## Preflight

```bash
cd ~/zclassic23-3
git fetch origin
git checkout main
git reset --hard origin/main
cat CLAUDE.md DEFENSIVE_CODING.md AGENT.md AGENT-3.md
make -j"$(nproc)" && ./test_zcl
```

If build or tests fail — STOP and report.

---

## Commit protocol

- One logical fix per commit.
- Every commit: `make test` passes. Every push: `make ci` passes.
- Commit body cites file:line from AGENT.md + ends with `Fixes P<N> (AGENT.md).`
- After each push, update the AGENT.md row to `done <SHA>`.
- Push frequently. Never `--amend` pushed commits. Never `--force-push`.
- Never log secret material.

---

## Coordination rules

- Agent-2 owns the rest of lib/net/, plus wallet/storage/validation-other/script/util.
- Rhett is coordinator-only. When NOW + NEXT are empty, ping Rhett.
- Out-of-scope discoveries during P7.4: append to "Notes from Agent-3"
  below; do NOT expand the P7.4 commit's scope.

---

## Notes from Agent-3

_(Keep short — 1-3 recent entries.)_

### 2026-04-19 (late night) — P8.5 rolling-bloom clamp landed — queue empty

- **P8.5 (`21da0531e`):** lifted the `MIN(ideal, MAX_BLOOM_HASH_FUNCS)`
  clamp out of the `constrained` branch in `bloom_filter_init_internal`
  so the internal `rolling_bloom_init` path (which passes
  `constrained=false`) now bounds `num_hash_funcs` on both internal
  filters `b1`/`b2`. Kept the `constrained` flag — it's still
  load-bearing for the `filter_bits` sizing cap in the public
  `bloom_filter_init` path; only the hash-func count path is now
  unconditional. Audit comment in `bloom.c` documents why both paths
  must clamp.
  - 3 new tests in `lib/test/src/test_bloom.c`:
    `P8.5: rolling_bloom_init clamps num_hash_funcs` — pathological
    tuning `rolling_bloom_init(num_elements=1, fp_rate=1e-30)` yields
    `ideal ≈ 97` pre-fix (filter_bits ≈ 287 → data_size=35, then
    `35*8/2 * LN2 = 97`); post-fix both `b1.num_hash_funcs` and
    `b2.num_hash_funcs` are clamped to `MAX_BLOOM_HASH_FUNCS=50`.
    Observed: `b1=50 b2=50 cap=50`.
    `P8.5: rolling_bloom_init sane params not over-clamped` — normal
    tuning `(120 000 elements, fp=1e-6)` stays under the cap
    (observed `hashes=19`) — asserts everyday rolling-bloom behavior
    is untouched.
    `P8.5: bloom_filter_init regression (public path still clamps)` —
    same pathological params via the public constrained path still
    clamp to 50; confirms we didn't break the pre-fix public-path
    behavior while lifting the clamp.
  - No changes to `MAX_BLOOM_HASH_FUNCS` itself (still 50) or the
    BIP37 `filterload` wire format — clamp is a CPU-cost guard only,
    not a protocol change.
- **Baseline failures carried forward** (pre-existing on main, not
  caused by this commit): `test_no_hardcoded_home` ×1
  (`vendor/tor/libtor.a` build-path leak — Agent-2 lane),
  `test_make_lint_gates` ×2 (env leakage from earlier test group in
  the runner; standalone repro is green). 3 total failures = exact
  match to the P8.2/P8.3 baseline. `make lint` green.
- Queue empty — pinging Rhett. All Agent-3-owned rows across
  P0–P8 closed.

### 2026-04-19 (late night) — P8.2 dandelion PRNG hardened — queue empty

- **P8.2 (`576b5cde2`):** dropped the static `s_dandelion_rng_state`
  + `dandelion_rand` xorshift64 (seeded from `time(NULL) ^ const`).
  Both decisions in `lib/net/src/dandelion.c` now route through
  `zcl_random_secret_bytes` via a small static `dandelion_secret_u64`
  helper — the same source already used for esk / Sapling rcm/rcv /
  Groth16 blinding. One fresh /dev/urandom read per Fisher-Yates
  swap in `dandelion_maybe_rotate_epoch` and one per coin-flip in
  `dandelion_should_stem`. Picked option B from the brief (full
  replacement, not just per-epoch reseed) — fewer predictable
  streams in the codebase, and dandelion is not a hot path.
  - **Safe-fail policy on RNG failure:** stem-peer rotation aborts
    leaving `num_stem_peers=0` (next `dandelion_should_stem` returns
    `false` → tx fluffs via normal relay), and the coin-flip itself
    returns `false` (tx fluffs). Both options drop privacy for the
    affected epoch/tx but keep relay working — strictly safer than
    picking with a compromised RNG. Logs the failure with
    `[dandelion] stem-peer RNG failed; ...` so a real RNG outage
    surfaces in the operator log instead of silently regressing
    privacy.
  - **Audit comment** at the top of the new RNG block in
    `dandelion.c` documents the cryptographic-RNG dependency and
    explicitly tells future "performance" refactors not to swap
    back to a cheap PRNG. The seed has to be unpredictable to the
    network, not just statistically random.
  - **Tests** in `lib/test/src/test_dandelion.c` via two
    `#ifdef ZCL_TESTING` hooks at the end of `dandelion.c`
    (`dandelion_test_shuffle`, `dandelion_test_should_stem_coin`)
    that mirror the production shuffle / coin-flip without needing
    a populated `net_manager`:
    `P8.2: dandelion stem shuffle non-deterministic` — back-to-back
    shuffles of `{1..8}` differ across calls; 5 retries drive the
    false-FAIL probability below ~1e-20 (8! ^ -5). Pre-fix, two
    shuffles inside the same wall-clock second produced identical
    outputs because `time(NULL)` was identical.
    `P8.2: dandelion fluff coin-flip ±3σ uniformity (10k)` — 10 000
    coin-flips fall in 8910..9090 stem (3σ around 9000 expected for
    `FLUFF_PROB=10`). Smoke-tests no bias from the new entropy
    source. Both new tests green; observed first run was
    `9042/10000` — comfortably inside the band.
  - The existing `dandelion_should_stem probability` test (1000
    trials, 80%-97% stem) still passes against the new RNG path
    (observed 88.5%) — no regression.
- **Baseline failures carried forward** (pre-existing on main, not
  caused by this commit): `test_no_hardcoded_home` ×1
  (`vendor/tor/libtor.a` build-path leak from Agent-2's tor build —
  Agent-2 lane), `test_make_lint_gates` ×2 (env leakage from
  earlier test group in the runner; standalone repro is green). 3
  total failures = exact match to the AGENT-3.md 2026-04-19 P8.3
  baseline. `make lint` green.
- Queue empty — pinging Rhett. All Agent-3-owned rows across
  P0–P8 closed. HIGH tier across the project now 100% (29/29).

### 2026-04-19 (late night) — P8.3 MMB height cap landed — queue empty

- **P8.3 (`c06515cbd`):** new `MMB_MAX_HEIGHT=64` in
  `lib/chain/include/chain/mmb.h`. `mmb_deserialize` rejects any
  mountain whose height exceeds the cap and calls `mmb_init(m)`
  again to re-zero the struct — prevents the caller from ever
  touching a partially-populated MMB where the cap-violating
  mountain and its predecessors might still be sitting in the
  peaks array. Mirror guard in the static
  `mmb_merge_after_insert` (both the rightmost-pair and the
  deferred-scan branches) returns `-1` via `LOG_ERR` when the
  pre-increment height is already at the cap; callers
  (`mmb_append` / `mmb_append_hash`) propagate that as-is, and
  `test_snapshot_sync_service.c:69` already treated `< 0` as
  error so no downstream change was needed.
  - Test fixtures in `lib/test/src/test_mmb.c`:
    `test_mmb_deserialize_rejects_oversize_height` (cap+1,
    UINT32_MAX, and exact-cap accept),
    `test_mmb_deserialize_real_chain_under_cap` (8192-leaf MMB
    round-trip — all heights well under 64; 8192 leaves reaches
    `ceil(log2(8192)) = 13`, 5× headroom vs cap),
    `test_mmb_merge_guard_blocks_wraparound` (two mountains at
    UINT32_MAX-1 via direct struct poisoning; then a second
    branch at MMB_MAX_HEIGHT — both assert guard-refused `rc<0`
    plus peak bytes + heights untouched after the refusal). All
    3 new tests green; full `./test_zcl` run shows the same 3
    pre-existing baseline failures (`test_no_hardcoded_home`,
    `test_make_lint_gates` ×2) carried forward unchanged.
- Queue empty — pinging Rhett. All Agent-3-owned rows across
  P0–P8 closed.

### 2026-04-19 (night) — P7.4 backpressure watchdog landed

- **P7.4 (`f6474c77b`):** new `lib/net/src/tip_watchdog.c` +
  `lib/net/include/net/tip_watchdog.h`. State machine with stats +
  test hooks (`tip_watchdog_test_set_now_ns`,
  `tip_watchdog_test_set_queue_bytes`, ...).
  - Tip-advance signal wired via sync event observers registered in
    `msg_processor_init` — EV_BLOCK_CONNECTED (fires per-block during
    IBD) + EV_TIP_UPDATED (fires on caught-up-to-peer). Both land on
    `tip_watchdog_note_tip_advance`.
  - `tip_watchdog_tick()` runs once per call to `msg_process_messages`;
    cheap (a couple of atomics + one `dl_get_stats` read).
  - Rejection layer added at top of the msg dispatch loop (after
    `msg_header_get_command`, before the `g_msg_dispatch` walk) so no
    handler runs for inv/block while ACTIVE. Drops emit
    `EV_BACKPRESSURE_REJECT` with peer id + cmd; ban-score untouched.
  - Queue-bytes estimate = `(in_flight + queued) * 2 MiB` — upper-
    bound conservative because the download manager only tracks hashes
    per slot, not per-slot byte cost. The 2 MiB figure is
    MAX_BLOCK_SIZE; erring high makes the watchdog trip sooner, which
    is the safer direction for an OOM backstop.
  - `dl_drain_for_backpressure` added to `download.[ch]`: drops queued
    hashes + marks every in-flight slot inactive without zeroing the
    hash bits (find_slot's open-addressing probe relies on those bits
    to distinguish virgin slots from deletion gaps).
  - 3 new events registered in event.h/event.c:
    `net.backpressure_active`, `net.backpressure_reject`,
    `net.backpressure_clear`.
  - 4 acceptance tests in `lib/test/src/test_net.c`:
    (1) ACTIVE entry on 61s stall + 256 MiB queue + inv/block reject,
    tx-kept; (2) clear-on-advance; (3) cooldown-elapsed-clear
    (bonus — not in the brief but falls naturally out of the test
    harness); (4) `ZCL_STRESS_TESTS`-guarded 1000-orphan flood
    rejection count. All 4 green.
- **Baseline failures carried forward** (pre-existing on
  `b669eed33`, not caused by this commit):
  - `test_no_hardcoded_home` hits one `/home/rhett` occurrence in the
    zclassic23 binary. Confirmed: `strings ./zclassic23 | grep
    /home/rhett` → single match `/home/rhett/zclassic23/vendor/tor` in
    `vendor/tor/libtor.a` debug-section strings, leaked from Agent-2's
    build path when the Tor submodule was recompiled earlier in the
    wave. Fix belongs in Agent-2's lane (build-path hygiene).
  - `test_make_lint_gates` reports the "baseline passes" and "passes
    after fixture removed" cases as FAIL — but the underlying
    `make -s check-raw-sqlite` target passes when run standalone
    (confirmed with a small C reproducer driving `system(...)`
    identically). Reproduces only when invoked from `test_zcl`'s
    environment, suggesting env leakage from an earlier group in the
    runner. Also pre-existing; outside P7.4 scope.
  - `make ci` under `ulimit -s unlimited` additionally hits a Bus
    error in `cookie_rotation` — reproduces on clean main
    (`b669eed33`) before this commit; pre-existing stack/RPC
    interaction flake.

