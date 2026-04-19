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

## 2026-04-19 (late): P12.1 SHIPPED 8fb7cb623, NOW P9.5 unblocked

P12.1 landed as a 4-commit wave:
- `bc4d92170` — timing probe on the replay loop (baseline measurement)
- `afcca842e` — RED test (stubs return false; 3 of 4 checks fail)
- `070b1b0a8` — GREEN: real checkpoint (SHA3 trailer + atomic
  .tmp+rename + verified deserialize)
- `8fb7cb623` — integration: boot-time load (before rebuild path
  fires) + every-10K-block flush from connect_tip + sapling_tree_rebuild
  delta-replay from the flat file

Live verification is Rhett's: on the next `make deploy` the log
should show `Sapling tree loaded from checkpoint: … (P12.1)`
instead of `Sapling tree root MISMATCH … rebuilding from block
files …`, and boot-to-ready should be in seconds.

### NOW — P9.5: pthread_once guard on lazy Sapling caches

Next row in the NEXT queue below (after P12.1). Both
`pedersen_hash.c::ensure_generators` and
`incremental_merkle_tree.c::ensure_sapling_empty_roots` are
guarded by a plain `static bool`. Two concurrent first-callers
can race and one gets a zero-generator. Replace with
`pthread_once`. RED test first (two racing threads), then fix.
Commit per the usual template. Mark `done <SHA> [test:1.0]`.

### (ARCHIVED — P12.1 brief, now done)

**Why this is the highest-leverage row.** Today every `make deploy`
(and every crash-recovery restart) runs:

```
Sapling tree root MISMATCH (size=N) — rebuilding from block files...
sapling_tree_rebuild: replaying h=476969..3081408
```

~2.6M blocks replayed, ~5 minutes of node unavailability per
restart. Two independent MVP criteria fail because of this:

- **MVP #1** (someone unfamiliar can run the node) — a 5-minute
  startup is a UX dealbreaker.
- **MVP #6** (7-day zero-intervention soak) — every crash or
  rotation eats 5 min of the soak budget. With P10.1 closing the
  stall, the NEXT restart cause (OOM, disk, power) lands on a
  node that takes 5 min to come back. The soak claim depends on
  restarts being cheap.

A sapling-tree disk checkpoint turns that into a 5-second boot.
Same shape as the existing block-index flat file.

**Files in scope:**
- `lib/sapling/src/incremental_merkle_tree.c` — rebuild path + the
  commit/load surface you'll add.
- `lib/sapling/include/sapling/incremental_merkle_tree.h` — public
  API for the checkpoint dump / restore.
- `app/services/src/sapling_tree_service.c` (or wherever the rebuild
  is kicked off — grep for `sapling_tree_rebuild`) — call site
  that decides "replay from block 0" vs "load from checkpoint N".
- `lib/test/src/test_sapling_tree.c` (may be new) — the RED+GREEN
  tests.

**Deliverable shape:**

1. **On-disk format** under `~/.zclassic-c23/sapling_tree_ckpt.dat`:
   - `uint32_t version = 1`
   - `uint64_t height` (checkpoint height — last block included)
   - `uint8_t root[32]` (root hash at this height — doubles as
     consistency check on load)
   - `uint32_t tree_size` (leaf count)
   - `uint8_t tree_blob[tree_size_bytes]` — serialized IMT peaks,
     matching whatever format the in-memory IMT already uses for
     its internal persistence (or define one with SHA3-256 framing
     of the whole blob for integrity).
   - Trailing SHA3-256 of the file body (excluding the trailing
     hash itself) as tamper check.

2. **Flush every N blocks** — start with `N = 10000` so we have
   at most ~10K blocks of replay on crash recovery. Hook the flush
   into the existing block-commit path (the same place that drives
   `update_tip` — look for `EV_BLOCK_CONNECTED`). Atomic write via
   `.tmp` + `rename(2)`; never half-written on disk.

3. **Load on boot** — before the rebuild path is hit, try to load
   the checkpoint. If it loads, verify `root` matches what's
   computed from the deserialized tree; if load or verify fails,
   fall back to the full replay path (which still works today).

4. **Delta-replay** — after loading the checkpoint at height H,
   replay from `H+1..tip` instead of `476969..tip`. This is the
   whole UX win.

**Discipline (P10.x rule — every row from here on is [test:1.0]):**

1. **Reproduce the 5-min cost.** Add a timing probe to the existing
   rebuild path — log `sapling_tree_rebuild: replayed N blocks in
   M ms` at INFO. Commit that separately (trivial) so we have a
   baseline number in the log.
2. **RED test FIRST** — a unit test that asserts
   "`sapling_tree_load_checkpoint(path)` succeeds after a
   `sapling_tree_flush_checkpoint(path)` round-trip, and the
   loaded root matches the in-memory root." This FAILS today
   (the functions don't exist). Commit RED.
3. **Implement the checkpoint serialize + flush.** Test flips to
   GREEN. Commit.
4. **Integrate the load path + delta-replay.** Add a second test
   that asserts delta-replay produces the same root as full-replay
   for an N-block synthetic chain. Commit with the integration.
5. **Live verification (Rhett's row).** After Rhett deploys, a
   second `make deploy` should show ≤5 s to sapling-tree-ready.

**Acceptance:**
- `lib/test/src/test_sapling_tree.c` covers flush/load round-trip,
  checkpoint + delta-replay root equivalence, corruption-on-load
  fall-back to full replay.
- `./test_zcl` + `make ci` green.
- Live deploy: boot time from "starting" to "sapling tree ready"
  drops from ~5 min to ≤5 s on a node that was previously
  checkpointing.

**STOP + ping Rhett triggers:**
- Any change to the in-memory IMT layout — `tree_blob` format is a
  NEW on-disk serialization but the live IMT struct must stay
  unchanged; the checkpoint file can be a fresh codepath.
- Any change that re-orders or skips block-level root commitments
  — we still want per-block root recomputation in the main apply
  path, just with a fast boot-time starting point.

---

### NEXT queue (pre-authorized; land in order after P12.1)

After P12.1 ships + canary-verifies on Rhett's deploy, drain the
following without pinging:

| Order | Row | Size | Severity | Brief |
|---|---|---|---|---|
| 1 | **P9.5** — pthread_once guard on lazy Sapling caches | small | HIGH | `pedersen_hash.c::ensure_generators` + `incremental_merkle_tree.c::ensure_sapling_empty_roots` — both guarded by a plain `static bool`. Use `pthread_once`. RED: two concurrent first-callers, one gets a zero-generator. Trivial fix. |
| 2 | **P9.3** — `lc_add_term` / `cs_alloc_var` / `cs_enforce` OOM silent-drop | small | HIGH | `groth16_prover.c:38-44, 91-103, 117-145` — return bool + propagate; LOG_FAIL on realloc failure. |
| 3 | **P9.4** — `fr_fft` / `fr_fft_parallel` silent no-op on non-pow-2 | small | HIGH | `groth16_prover.c:222`, `msm_parallel.c:333,340` — promote to bool, LOG_FAIL on bad input. Currently unreachable via `groth16_prove` but the pattern waits for the next caller. |
| 4 | **P9.1** — `g1_scalar_mul` constant-time | medium | HIGH | `lib/sapling/src/bls12_381.c:1708-1722` — mirror P1.12 / P1.16b pattern. Branches on secret blinding scalars (r_blind, s_blind, r·s); masked linear-scan table select + unconditional-add-with-mask. Hamming-weight timing regression test. |
| 5 | **P9.2** — `sapling_circuit.c` placeholder UB paths | small-or-huge | CRITICAL | `:65, :161-162` — decide first whether these paths are shadowed by another prover impl (`grep -r sapling_create_spend_proof`). If shadowed: 3-line `LOG_FAIL`. If live: multi-day rewrite; STOP + ping Rhett before starting that. |
| 6 | **P9.6** — `sapling_prover_c23.c` witness length arg | small | MED | Add `size_t witness_len` + LOG_FAIL on shortfall. |
| 7 | **P9.7** — `sprout_verify_groth16` ic_len=0 underflow | small | MED | Snapshot pointer + LOG_FAIL on `ic_len < 1`. |
| 8 | **P9.8** — `ensure_generators` exhaustion silent | small | MED | Mirror P1.10 pattern; track success + LOG_FAIL + abort on exhaustion. |
| 9 | **P9.9** — printf/stdout leak in prover paths | small | MED | Replace with `LOG_INFO` or delete; stdout leak of wallet-activity timing. |
| 10 | **P9.10** — MSM cache-side-channel on witness | medium | LOW | `msm_parallel.c:60-69, 181-190` + `groth16_prover.c:300-322, 362-381` — CT bucket access. Pairs naturally with P9.1 (shared Hamming-weight timing regression). |
| 11 | **P11.4 / P11.5 / P11.6 / P11.8** MVP CI gates | medium each | — | TBD — need upstream service work (P12.3 parity service for #8, shielded payment path for #4, store flow for #5, soak harness for #6). Pick up after Agent-2 delivers each upstream row. |

When the queue drains, ping Rhett for next triage.

---

## RESET (2026-04-19): P9 wave shipped, ALL findings deferred — on-call to review Agent-2's P10.1 (ARCHIVED below)

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

**P9.x crypto fixes stay deferred.** Resist the urge to start
those rows; they're parked deliberately. When P10.1 closes, Rhett
re-triages.

**Open queue (P10.1.2-REVIEW + P11.1 closed; sync MVP rows next):**

| Order | Row | Size | Owner |
|---|---|---|---|
| DONE | P10.1.2-REVIEW (`879192ee2`) — CONCUR_WITH_NOTES | — | — |
| DONE | P11.1 (`63f98909d`) — Tor bootstrap CI test, MVP #2 ✅ | — | — |
| DONE | P11.3 (`ffd1112e4`) — cold-start sync CI test, MVP #3 ✅ | — | — |
| DONE | P11.7 (`8d3d3b23f`) — kill -9 recovery CI test, MVP #7 ✅ | — | — |
| **NOW** | queue empty — **ping Rhett** for next triage (CI-verified MRS now 3/8; P11.4/5/6/8 remain TBD, P9.x sapling audit findings still deferred behind P10.1.5) | — | — |
| follow-up | P11.4 (#4 shielded payment e2e), P11.5 (#5 store flow e2e), P11.6 (#6 7-day soak harness), P11.8 (#8 parity diff service) — all need upstream service work before CI gates can land | — | TBD |

Rhett's directive 2026-04-19: **"work on getting the product
working and syncing."** P11.3 + P11.7 are the two MVP criteria that
DIRECTLY test "syncing works." Once both are green CI tests, we
can prove the syncing claim instead of just asserting it.

---

## (Below: archived NOW for P11.3 — landed `ffd1112e4`, reference only) — cold-start sync CI

After P11.1 (`63f98909d`). Independent of Agent-2's P10.1.4 progress.

### Goal

`MVP.md` criterion #3: "Cold-start sync to tip in <10 min."
Today: untested in CI. Operator has to manually time a fresh boot
to know whether sync is healthy. Build the assertion that flips
this from ☐ to ✅.

### Deliverable

New CI test in `lib/test/src/test_cold_start_sync.c` that:

1. Spins up a node with a **fresh empty datadir** + a synthetic
   peer (or a fixture peer with a small N-block chain).
2. Polls `zcl_syncstate.phase` (or the underlying state machine)
   at 1Hz.
3. Asserts `phase=ready` within 10 minutes of start.
4. Asserts the chain tip matches the peer's tip.
5. Marked `ZCL_STRESS_TESTS`-guarded — this test takes minutes,
   not milliseconds. Don't slow down `make test` for everyone.

### Lane fit

- New test file in `lib/test/src/` — both lanes have access.
- Synthetic peer fixture: if you need to mock a peer, prefer to
  add a small helper to `lib/test/include/test_helpers.h` rather
  than touching `lib/net/` (Agent-2's lane).
- If you need a small chain fixture, generate it in-test rather
  than committing a binary blob.

### Acceptance

- Test exists at `lib/test/src/test_cold_start_sync.c`.
- Runs cleanly under `make stress-test` (or whatever the
  ZCL_STRESS_TESTS gate is named).
- Test PASSES today (verifies current behavior meets MVP #3) on a
  small fixture chain. Real-network 3M-block sync test is too slow
  for CI; if you can stub the network with a local in-memory peer,
  even a 100-block chain proves the state-machine reaches `ready`.
- Updates `MVP.md` criterion #3 from ☐ to ✅ with the test path.
- Bumps the CI-verified MRS line from 1/8 to 2/8.

### Note on scope

Don't try to test the *real* 3M-block sync — that's a soak test
(criterion #6), not a unit test. Criterion #3's CI version is "the
sync state machine reaches `ready` against a small synthetic peer."
The real network behavior is verified by criterion #6's soak.

### Commit template

```
test/sync: CI assertion for MVP criterion #3 (cold-start sync) (P11.3)

Fixes P11.3 (AGENT.md). Adds lib/test/src/test_cold_start_sync.c
which spins a fresh-datadir node against a synthetic peer with an
N-block fixture chain, polls syncstate.phase at 1Hz, and asserts
phase=ready within 10 min.

Flips MVP.md criterion #3 from ☐ to ✅. CI-verified MRS now 2/8.

Test passes today; will fail loudly on any future regression in
the cold-start path.
```

Mark `done <SHA> [test:0.5]` (forward-looking assertion, bumps
MRS not HI).

---

## (Below: archived NOW for P11.7 — landed `8d3d3b23f`, reference only) — kill -9 chaos recovery

P10.1.4 landed `ac782fef5`. Unblocked. Run this against the
post-P10.1.4 binary.

### Goal

`MVP.md` criterion #7: "Recover from `kill -9` in <2 min."
Today: untested. The whole point of P10.1's invariant assertion +
disconnect_block fix is to make this work. P11.7 is the test that
proves it.

### Deliverable

New CI test in `lib/test/src/test_kill9_recovery.c` that:

1. Spins up a node, syncs against a synthetic peer to ~50 blocks.
2. Sends `SIGKILL` to the node mid-block-application (or after a
   non-deterministic short delay so different runs catch different
   moments).
3. Restarts the node from the same datadir.
4. Asserts the node catches up to the peer's tip within 120s.
5. Repeats step 2-4 ten times to catch race conditions in the
   recovery path.
6. ZCL_STRESS_TESTS-gated.

### Lane fit

- Test file in `lib/test/src/`. The kill-restart mechanics may
  require a small helper in `lib/test/src/test_helpers.c` for
  fork/exec/signal — keep that in lib/test/, don't touch
  lib/util/.

### Acceptance

- Test exists, passes against a node built post-P10.1.4.
- Updates `MVP.md` criterion #7 from ☐ to ✅.
- Bumps CI-verified MRS from 2/8 to 3/8.

### Why this matters

Criterion #7 directly validates P10.1's fix. If the chaos test
fails, P10.1 didn't actually fix the disconnect_block leak — the
test catches the regression that humans wouldn't.

Mark `done <SHA> [test:0.5]` (forward-looking).

---

## (Below: archived NOW for P10.1.2-REVIEW — landed `879192ee2`, reference only)

## (legacy section header below — archived NOW for P10.1.2-REVIEW: written review of root-cause writeup)

Agent-2 pushed `docs/postmortems/2026-04-19-bip30-stall.md` as
`5279752d1`. Their TL;DR: `disconnect_block`'s
`coins_map_erase(&view->cache_coins, &tx->hash)` at
`lib/validation/src/connect_block.c:639` leaves the disconnected
coinbase in the backing store. The flush only writes DIRTY entries,
and an erased entry is non-DIRTY → row survives in SQLite → next
reconnect trips BIP30.

### Deliverable

A new markdown file: `docs/reviews/2026-04-19-p10-1-2-review.md`.
Required structure:

```markdown
# Review of P10.1.2 root-cause writeup (5279752d1)
## Reviewer: Agent-3
## Verdict: CONCUR | CONCUR_WITH_NOTES | DISAGREE

## Question 1 (regress path 3,081,408 → 3,081,407)
[Your assessment of Agent-2's answer. Either:
 - "Concur. The disconnect_tip path at process_block.c:2082 matches
   the log evidence (no EV_REORG_START emitted)."
 - OR specific disagreement: "Path X looks more likely because Y."]

## Question 2 (why BIP30 trips post-P8.9)
[Your assessment. Either concur or specifically point to a code
 path the writeup missed.]

## Question 3 (the invariant)
[Your assessment. Is the invariant correctly phrased? Does it
 cover the cache+backing two-layer state correctly?]

## Question 4 (test gap)
[Your assessment. Which existing test SHOULD have caught this?
 Why didn't it?]

## Suggested test for P10.1.3
[Concrete: which file, which existing fixture, what the new test
 should assert. Be specific enough that Agent-2 can implement
 directly.]

## Other concerns (optional)
[Anything else worth flagging.]
```

### How to do the review

1. Read the writeup carefully — `docs/postmortems/2026-04-19-bip30-stall.md`.
2. Read the cited line numbers in the actual source — at minimum
   `connect_block.c:639`, `process_block.c:2082`, `coins_view.c:255`,
   `coins_view_sqlite.c:664`.
3. Read Agent-2's reproduction test — `lib/test/src/test_chain_stall_repro.c`
   (landed `1243e1766`). Confirm the test exercises the SAME path the
   writeup names (not a different BIP30 trigger).
4. Write the review file. Time-box: 30 min. If you can't reach
   CONCUR or DISAGREE within 30 min, emit CONCUR_WITH_NOTES and list
   the specific things you couldn't verify.

### Why this matters

The MVP target needs HI ≥80%. If P10.1.2's root cause is wrong,
P10.1.3's regression test will be the wrong test, P10.1.4 will be
another hotfix in disguise, and the live node won't recover. Your
job is to be the second pair of eyes that catches that.

### Commit template

```
docs/reviews: P10.1.2 review — concur with disconnect_block leak (P10.1.2-REVIEW)

Reviewed Agent-2's root-cause writeup (5279752d1) and the cited
code paths. Verdict: <CONCUR|CONCUR_WITH_NOTES|DISAGREE>.

<one-paragraph summary of the assessment, naming any concerns>.
```

After this lands, Agent-2 proceeds with P10.1.3 (RED regression
test). You move to P11.1.

---

## NEXT — P11.1: MVP criterion #2 CI test (Tor onion bootstrap <60s)

After the P10.1.2 review lands. Independent of P10.1's progress.

### Goal

`MVP.md` criterion #2: "Tor onion bootstrap in <60s." Today this
is **untested in CI** — it's only verified by manual operator
inspection of `zcl_status` after a fresh boot. Build the harness
that flips this from ☐ to ✅.

### Deliverable

New CI test in `lib/test/src/test_onion_bootstrap.c` that:

1. Spins up an in-process or temp-datadir node with `-tor`.
2. Polls `zcl_onion_status` (or the underlying state machine) at
   1Hz for up to 90 seconds.
3. Asserts `bootstrap_state == ready` within **60 seconds**.
4. Asserts the `.onion` address is non-empty + valid format.
5. Marked `ZCL_STRESS_TESTS`-guarded if it requires the full Tor
   submodule build (don't slow down `make test` for everyone).

### Lane fit

- `vendor/tor` is your lane (P5.5).
- `lib/net/src/onion_service.c` + `tor_integration.c` are
  Agent-2's lane — read-only for you, but you can call into
  their public API from the test.
- New test file in `lib/test/src/` — both lanes have access.

### Acceptance

- New test exists.
- Runs cleanly under `make test` (or `make stress-test` if guarded).
- Test PASSES today (verifies the current behavior meets MVP #2).
- Fails loudly if the bootstrap regresses past 60s in the future.
- Updates `MVP.md` criterion #2 from ☐ to ✅ with the test path.

### Commit template

```
test/onion: CI assertion for MVP criterion #2 (bootstrap <60s) (P11.1)

Fixes P11.1 (AGENT.md). Adds lib/test/src/test_onion_bootstrap.c
which spins a temp-datadir node with -tor, polls onion_status at
1Hz, and asserts bootstrap_state=ready within 60s.

Flips MVP.md criterion #2 from ☐ to ✅. MRS now <fill>/8.

Test passes today; will fail loudly on any future regression.
```

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

### 2026-04-19 (overnight) — P11.7 kill -9 recovery CI landed — MRS 2/8 → 3/8 — queue empty

- **P11.7 (`8d3d3b23f`):** new `lib/test/src/test_kill9_recovery.c`.
  For each of 10 cycles, parent `fork()`s a child running a
  realistic connect-block-style write loop (BEGIN/COMMIT per
  "block", 30 blocks at ~1ms cadence, per-block UTXO inserts +
  tip-pointer update), then `kill(pid, SIGKILL)` after a
  randomised 0.5-40ms delay (covers pre-begin / mid-insert /
  pre-commit / post-commit kill windows), `waitpid()`s, reopens
  the datadir via `coins_view_sqlite_open` (same entry point the
  live node takes on boot), and asserts (a) reopen succeeds and
  (b) zero UTXO rows sit above the tip height after recovery.
  - Dev-box 3-run flake check: 10 cycles in 0-1s each, typical
    distribution 5-8 clean tip-advances + 2-5 mid-apply SIGKILLs.
    Every cycle's reopen succeeded with zero UTXO overshoot.
  - **Design choice B from the research brief** (fork + in-process
    chainstate + SIGKILL + parent reopen): the heaviest Option A
    (full `./zclassic23` subprocess on loopback with a synthetic
    peer) is overkill for a regression gate — MCP E2E already
    covers that shape in `test_mcp_e2e.c`. Option C (same-process
    abort) loses real kill -9 kernel semantics. Option B hits the
    right exercise surface for the on-disk atomicity invariant
    that P10.1.4 protects.
  - **Self-contained** — duplicates the SQLite schema + seed
    helpers from `test_coins_view_atomicity.c` rather than exporting
    them. Keeps both tests independent (different schema variants
    over time) and avoids header-surface churn in the lib/test
    public API.
  - `ZCL_STRESS_TESTS=1`-gated to match the P11.1/P11.3 convention.
    Registered in the default sequence at `lib/test/src/test.c`
    AND via `ZCL_TEST_ONLY=kill9`. Skip path verified.
- **Baseline failures carried forward** (pre-existing on
  `c71746023`, not caused by this commit): `test_no_hardcoded_home`
  ×1 (Agent-2 lane, libtor.a debug-path leak), `test_make_lint_gates`
  ×2 (env leakage). 3 total failures = exact match to the P11.3
  baseline and all prior P8.x baselines. `make lint` green.
- **MVP linkage:** flipped `MVP.md` criterion #7 from ☐ to ✅ with
  the test path. CI-verified MRS bumps from 2/8 to 3/8. `AGENT.md`
  Progress block + P11 table + owner lines all synced.
- **Queue empty:** P11.4/5/6/8 all require upstream service work
  before CI gates can land (shielded payment flow, store e2e,
  soak harness, parity diff service — all Agent-2 lanes per the
  P11.x "follow-up" row in AGENT.md). P9.x sapling-prover audit
  findings remain deferred behind Rhett's P10.1.5 live-node
  canary. Pinging Rhett for next triage.

### 2026-04-19 (overnight) — P11.3 cold-start sync CI landed — MRS 1/8 → 2/8

- **P11.3 (`ffd1112e4`):** new `lib/test/src/test_cold_start_sync.c`
  drives the sync FSM (`lib/event/src/event.c:858-916`) from
  `SYNC_IDLE` through both legal cold-start transition sequences
  via a background pthread driver, polls `sync_get_state()` at 1Hz
  from the main test thread, and asserts `SYNC_AT_TIP` is reached
  inside the 600s MVP budget:
  - Path A (legacy IBD): `FINDING_PEERS → HEADERS_DOWNLOAD →
    BLOCKS_DOWNLOAD → CONNECTING_BLOCKS → AT_TIP` — dev-box run
    reached `SYNC_AT_TIP` in **4s**.
  - Path B (ZCL23 fast-sync): `FINDING_PEERS → SNAPSHOT_RECEIVE →
    CONNECTING_BLOCKS → AT_TIP` — dev-box run reached
    `SYNC_AT_TIP` in **3s**.
  - Both paths are live in production; a regression in either
    one (removed transition, renamed state, or deadlock in
    `sync_set_state`) fails this test loudly.
  - `ZCL_STRESS_TESTS=1`-gated to match the P11.1 onion-bootstrap
    convention. Registered in the default sequence at
    `lib/test/src/test.c` AND via `ZCL_TEST_ONLY=cold_start`.
    Skip path is verified (test prints `SKIP` under the default
    `make test` run and returns 0).
  - **Hermeticity:** resets to `SYNC_IDLE` at entry (any state →
    IDLE is legal) and exit — matches the convention in
    `test_sync_watchdog.c`'s `reset_test_state`. Does NOT register
    an event observer on `EV_SYNC_STATE_CHANGE`, so it can't
    accidentally stomp on the observers that `spec_state_machine`
    or `boot_sync_state_logger` install.
- **Portability fix during dev:** initial draft used `usleep(3)`,
  which isn't declared under `-D_POSIX_C_SOURCE=200809L` (obsolete
  in POSIX.1-2008). Switched the delay helper to `nanosleep(2)`
  (POSIX.1-2001) — same wall-clock behavior, builds clean under
  `-Werror`.
- **Baseline delta:** origin/main on `c71746023` reported 5
  failures for me (`test_no_hardcoded_home` ×1, `test_make_lint_gates`
  ×2, plus flaky `addrman: add and select` + `mcp zcl_admin
  envelope` that didn't reproduce on the re-run). With P11.3 added,
  `make test` run saw 3 failures — the 3 known-baseline ones
  (lint-gate env-leak ×2 + tor libtor.a debug-path leak ×1); the
  flaky two came back green. My change is net-neutral on baseline.
  `make lint` green.
- **MVP linkage:** flipped `MVP.md` criterion #3 from ☐ to ✅ with
  the test path. CI-verified MRS now 2/8 (up from 1/8 after
  P11.1). `AGENT.md` Progress block + P11 table synced.
- **P11.7 unblocked** — Agent-2's P10.1.4 fix landed at
  `ac782fef5`, so the kill -9 chaos test can run against a
  post-fix binary. Promoted to NOW above; archive header for the
  P11.3 brief moved below.

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

