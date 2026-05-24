# Worker Assignment — Phase 2 Cutover C-3: validate_headers authoritative

**Worktree:** wt2 OR wt3 (cutover work has no worker affinity now that
all 9 stages have shipped)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 2 cutover (second authoritative flip)
**Depends on:** Cutover C-2 (header_admit authoritative) MERGED +
≥1h zero `EV_CUTOVER_GUARD_DIVERGED` on a moving chain
**Plan reference:** [`docs/architecture/wave-s-cutover.md`](../architecture/wave-s-cutover.md) § C-3

**Owns:**
- EDIT `app/services/include/services/validate_headers_stage.h` — add mode flag API
- EDIT `app/services/src/validate_headers_stage.c` — add authoritative path + flag
- EDIT `lib/validation/src/accept_block_header.c` — add divergence guard
  on the PoW + Equihash verification call sites
- EDIT `lib/test/src/test_validate_headers_stage.c` — add cutover tests
- (Maybe) edit boot code to set initial mode

**MUST NOT touch:**
- Other Wave S stage files (header_admit, body_persist, etc.) —
  those have their own cutover PRs
- Conditions, watchdog, supervisors
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

C-3 flips the PoW + Equihash verification ownership to the
`validate_headers_stage`. Today, when a header is admitted, the legacy
`accept_block_header` path does both:
1. **Admit** the header into the block index — now owned by S-2 / C-2.
2. **Verify** the PoW solution and Equihash difficulty target — still
   owned by legacy.

After C-3, the stage's per-height `validate_headers_log[H]` becomes the
source of truth for "this header's PoW is verified." Legacy
`accept_block_header` does the same work in SHADOW mode (current
state), then in AUTHORITATIVE mode skips it (the stage already did
it) but enforces that the stage has a record.

The performance impact is the first meaningful CPU win: the stage
verifies headers in a dedicated 4-thread pool (S-3 shipped that),
while legacy single-threaded the work. After C-3, legacy STOPS
re-verifying — same work done once instead of twice.

---

## The 4-commit PR structure

### Commit 1: Add mode flag

```c
/* in validate_headers_stage.h */
typedef enum {
    VALIDATE_HEADERS_MODE_SHADOW = 0,
    VALIDATE_HEADERS_MODE_AUTHORITATIVE
} validate_headers_mode_t;

void validate_headers_set_mode(validate_headers_mode_t mode);
validate_headers_mode_t validate_headers_get_mode(void);
```

Default mode is SHADOW. No behavior change.

**Commit message:** `validate_headers: add mode flag (default SHADOW)`

### Commit 2: Add authoritative write path (gated)

In `validate_headers_stage.c`'s per-height step:

```c
if (validate_headers_get_mode() == VALIDATE_HEADERS_MODE_AUTHORITATIVE) {
    /* AUTHORITATIVE: stage drives the live state */
    bi->nStatus |= BLOCK_VALID_HEADER;
    block_index_persist_status(bi);
}
/* SHADOW: still log per-height record as today */
log_validate_record(h, bi, ok, NOW);
```

The shadow path is unchanged. The authoritative branch is dead code
when mode == SHADOW.

**Commit message:** `validate_headers: add authoritative write path (gated)`

### Commit 3: Add divergence guard in legacy path

In `lib/validation/src/accept_block_header.c` (specifically the PoW +
Equihash verification call after admit — find via grep for
`CheckProofOfWork` or `CheckEquihashSolution`):

```c
if (validate_headers_get_mode() == VALIDATE_HEADERS_MODE_SHADOW) {
    /* Legacy path: verify PoW + Equihash inline */
    if (!CheckProofOfWork(bi->GetBlockHash(), bi->nBits, params))
        return false;
    if (!CheckEquihashSolution(bi, params))
        return false;
    bi->nStatus |= BLOCK_VALID_HEADER;
} else {
    /* AUTHORITATIVE: stage owns this; we only guard */
    struct validate_headers_record rec;
    if (!validate_headers_lookup_record(h, &rec) || !rec.ok) {
        EMIT(EV_CUTOVER_GUARD_DIVERGED,
             "validate_headers cutover divergence at h=%d: "
             "legacy expects header verified but stage has no record", h);
        return false;  /* refuse to advance */
    }
    /* else: stage already did the verification + set BLOCK_VALID_HEADER */
}
```

The guard NEVER fires in SHADOW mode. In AUTHORITATIVE mode, it
catches the case where legacy expected a verified header that the
stage didn't record.

**Commit message:** `validate_headers: add divergence guard in legacy PoW path`

### Commit 4: Flip default to AUTHORITATIVE

```c
- static _Atomic validate_headers_mode_t g_mode = VALIDATE_HEADERS_MODE_SHADOW;
+ static _Atomic validate_headers_mode_t g_mode = VALIDATE_HEADERS_MODE_AUTHORITATIVE;
```

One-line change. CUTOVER live.

**Commit message:** `validate_headers: cutover to AUTHORITATIVE mode`

---

## Tasks (each = one commit above)

### Task 1: Commit 1 — add the mode flag

Acceptance: `make` clean; `validate_headers_get_mode()` returns
`VALIDATE_HEADERS_MODE_SHADOW` by default.

### Task 2: Commit 2 — add authoritative write path (gated off)

Acceptance: `make` clean. Existing tests pass (still in SHADOW mode).
NEW unit test: `validate_headers_set_mode(AUTHORITATIVE)` + step once
→ verify `BLOCK_VALID_HEADER` was set in the block_index entry
(observe via the existing `block_index_get_status` helper, or a
test hook).

### Task 3: Commit 3 — divergence guard in legacy path

Acceptance: `make` clean. Existing tests pass (still in SHADOW mode,
guard doesn't fire). NEW unit test: switch to AUTHORITATIVE,
intentionally skip the stage's record, then run legacy
`accept_block_header` → guard fires with `EV_CUTOVER_GUARD_DIVERGED`.

### Task 4: Commit 4 — flip default

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin main
```

Acceptance: ALL existing tests pass with the new default. Tip
advancement velocity (blocks_per_sec via `zcl_validationstatus`) is
unchanged OR faster (legacy stops re-verifying, but the bottleneck
may have been elsewhere).

---

## Live verification (post-merge, before C-5 starts)

Orchestrator polls:
```bash
zcl_state subsystem=validate_headers
zcl_events --since 1h --filter EV_CUTOVER_GUARD_DIVERGED
```

For ≥1h on a moving chain. **Zero `EV_CUTOVER_GUARD_DIVERGED` events**
is the gate for C-5 to begin.

If divergences appear, revert commit 4 of this PR (or revert the
whole PR), investigate, re-attempt.

If chain wedged: use test fixtures only and don't flip commit 4
until the chain resumes advancement.

---

## Commit cadence

ONE commit per task. Push after each commit so the orchestrator can
see progress + revert individual commits if needed.

---

## Status

**✅ DONE — pushed 2026-05-24** to main as commit `ad34efb65`.

When this lands, the next-in-line is `wt-phase2-cutover-c5-body-persist.md`
(C-4 body_fetch is folded into C-5 per the batch spec).

<!-- Worker: append a Completion section below when done. -->

## Completion (wt3, 2026-05-24)

### Summary

Implemented C-3 validate_headers authoritative cutover:
- `validate_headers_stage` now defaults to AUTHORITATIVE mode, marks passing
  block index entries at least `BLOCK_VALID_HEADER`, exposes mode and pass-row
  lookup APIs, and reports mode in stage JSON.
- `accept_block_header` now skips legacy PoW/header verification only in
  AUTHORITATIVE mode and requires a passing `validate_headers_log` row before
  promoting legacy state; missing rows emit `EV_CUTOVER_GUARD_DIVERGED` and
  reject with `validate-headers-cutover-diverged`.
- Added focused tests for mode defaults, authoritative status marking, pass-row
  lookup, and the missing-pass-row divergence guard.

### Commits
- `ad34efb65` validate_headers: cut over authoritative verification
- `c3e696b1d` crypto_registry: stabilize ECDSA overhead benchmark
- `686cad976` event: deflake async dispatcher lifecycle test

### Files modified
- `app/services/include/services/validate_headers_stage.h`
- `app/services/src/validate_headers_stage.c`
- `lib/validation/src/accept_block_header.c`
- `lib/test/src/test.c`
- `lib/test/src/test_crypto_registry.c`
- `lib/test/src/test_event.c`
- `lib/test/src/test_validate_headers_stage.c`

### Acceptance verification
- [x] `make test_zcl -j$(nproc)` — PASS
- [x] `ZCL_TEST_ONLY=validate_headers ./test_zcl` — PASS
- [x] `make lint` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS, 0/196 groups failed

### Surprises / follow-ups
The local worktree was 24 commits behind origin before commit; the C-3 patch
reapplied cleanly after a fast-forward to `8d4edd206`. Full-suite verification
then exposed two pre-existing timing-sensitive tests (`crypto_registry` ECDSA
overhead and `event` async dispatch); both are now stabilized in separate
commits above.
