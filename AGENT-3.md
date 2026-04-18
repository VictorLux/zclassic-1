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

## Current status — 2026-04-19 (late night, P8.3 landed — P8.2 reassigned to you)

**Done and on main (18 rows + audit):** P1.3, P1.4, P1.6 (`f6aa0b080`),
P1.7 (`5ce252bb6`), P1.8, P1.9, P1.10, P1.11, P1.11b, P1.12, P1.13,
P1.14, P1.15, P1.16 (`94d607b85`), P1.16b (`c841defd2`), P5.5
(`75576d7a0`), P7.4 (`f6474c77b`), P8.3 (`c06515cbd`), and the
P8-wave audit pass (`6751d9bfa` — opened 8 new rows).

All crypto + consensus + vendor + net-backpressure + MMB-hardening
rows shipped. P8.3 closed.

**P8.2 reassigned to you** — natural fit with your RNG lane: you
own `lib/core/src/random.c` (P1.16) and `lib/crypto/src/random_secret.c`
(P1.15). The dandelion seed quality issue is exactly the kind of
RNG-fail-open you've been hardening. Lane expansion into `lib/net/`
already granted via P7.4.

**Open queue (1 row, then ping Rhett):**

| Order | Row | Size | Severity |
|---|---|---|---|
| **NOW** | **P8.2** — dandelion stem-peer PRNG seeded with ~31 bits of entropy | small | HIGH |
| NEXT | (queue empty — ping Rhett) | — | — |

---

## NOW — P8.2: replace dandelion PRNG with cryptographic RNG

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

### 2026-04-19 (night) — consensus + vendor + P1.16b closed

- **P1.6 (`f6aa0b080`):** P2SH sigop accounting mirroring zclassicd.
  Per-input 15-cap deferred to mempool policy (Agent-2 lane) to
  avoid consensus divergence from legacy Zcash.
- **P1.7 (`5ce252bb6`):** removed `skip_diffbits` escape hatch;
  contextual_check_block_header now always calls GetNextWorkRequired.
- **P5.5 (`75576d7a0`):** vendor/tor pin d14113e → 73bd405. Onion
  bootstrap smoke test pending Rhett's `make deploy`.
- **P1.16b (`c841defd2`):** prf.c nullifier CT audit — masked
  jubjub_to_scalar reduction; 10k diff test + Hamming-weight timing
  test.

### 2026-04-19 (night) — lane expansion into lib/net

For P7.4, you now have read+write access to msgprocessor.c and
download.c. Do NOT touch anything else in lib/net/ — Agent-2 owns it.
