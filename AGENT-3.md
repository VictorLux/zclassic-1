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

## Current status — 2026-04-19 (night)

**Done and on main (16 rows):** P1.3, P1.4, P1.6 (`f6aa0b080`),
P1.7 (`5ce252bb6`), P1.8, P1.9, P1.10, P1.11, P1.11b, P1.12, P1.13,
P1.14, P1.15, P1.16 (`94d607b85`), P1.16b (`c841defd2`), P5.5
(`75576d7a0`). AGENT.md shows SHAs.

All crypto + consensus + vendor rows shipped. New assignment: the
last HIGH-severity row in `lib/net/` (P7.4 backpressure watchdog),
parallelized with Agent-2's P4.1+P4.2 script-interpreter refactor.

**Open queue (2 logical tasks):**

| Order | Row | Size | Severity |
|---|---|---|---|
| NOW | **P7.4** backpressure watchdog under tip-stuck | medium | HIGH |
| NEXT | **Fresh code-review pass** → open the P8 wave | audit | — |

---

## NOW — P7.4: backpressure watchdog under tip-stuck

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
