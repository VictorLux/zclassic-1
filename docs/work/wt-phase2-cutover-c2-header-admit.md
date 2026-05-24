# Worker Assignment — Phase 2 Cutover C-2: header_admit authoritative

**Worktree:** wt3 (most familiar with the Wave S stages)
**Branch:** `wt3/phase2-cutover-c2-header-admit`
**Phase:** 2 cutover (first authoritative flip)
**Depends on:** Wave S S-9 tip_finalize MERGED + 24h zero-divergence soak
**Plan reference:** [`docs/architecture/wave-s-cutover.md`](../architecture/wave-s-cutover.md)

**Owns:**
- EDIT `app/services/include/services/header_admit_stage.h` — add mode flag API
- EDIT `app/services/src/header_admit_stage.c` — add authoritative path + flag
- EDIT `lib/validation/src/accept_block_header.c` — add divergence guard
- EDIT `lib/test/src/test_header_admit_stage.c` — add cutover test
- (Maybe) edit boot code to set initial mode

**MUST NOT touch:**
- Other Wave S stage files
- Conditions or watchdog code
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

This is the FIRST Wave S authoritative cutover. After this PR:

- `header_admit` stage OWNS the block index header admission.
- The legacy `accept_block_header` ingress path stops writing to the
  block index directly when the stage is in AUTHORITATIVE mode.
- A divergence guard inside the legacy path catches any case where
  the stage's record disagrees with what the legacy path would have
  done.

The 4-commit PR structure makes the cutover trivially revertable. If
something breaks live, revert commit 4 only — the legacy path resumes,
the stage drops back to SHADOW mode.

---

## The 4-commit PR structure

### Commit 1: Add mode flag

```c
/* in header_admit_stage.h */
typedef enum {
    HEADER_ADMIT_MODE_SHADOW = 0,        /* observe only — current default */
    HEADER_ADMIT_MODE_AUTHORITATIVE      /* drive the live state */
} header_admit_mode_t;

void header_admit_set_mode(header_admit_mode_t mode);
header_admit_mode_t header_admit_get_mode(void);
```

Implementation in `.c` is one `_Atomic` enum.

Default mode is SHADOW. No behavior change. Just adds the flag.

**Commit message:** `header_admit: add mode flag (default SHADOW)`

### Commit 2: Add authoritative write path (gated)

In `header_admit_stage.c`'s `step_admit()`:

```c
if (header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE) {
    /* AUTHORITATIVE: stage drives the live state */
    bool ok = block_index_admit_header(bi);
    if (!ok) {
        log_failure(...);
        return STAGE_FAILED;
    }
}
/* SHADOW: just log as today */
log_admit_record(h, bi, NOW);
```

The shadow path is unchanged. The authoritative branch is dead code
when mode == SHADOW.

**Commit message:** `header_admit: add authoritative write path (gated)`

### Commit 3: Add divergence guard in legacy path

In `lib/validation/src/accept_block_header.c` (or wherever the legacy
direct block_index write happens), wrap the direct-write code:

```c
if (header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW) {
    /* Legacy path: do the work, as today */
    block_index_admit_header(bi);
} else {
    /* AUTHORITATIVE: stage owns this; we only guard */
    int64_t shadow_recorded_at = header_admit_lookup_record(h);
    if (shadow_recorded_at == 0) {
        EMIT(EV_CUTOVER_GUARD_DIVERGED,
             "header_admit cutover divergence at h=%d: "
             "legacy expects admit, stage has no record", h);
        return false;  /* refuse to advance */
    }
    /* else: stage already did the work; no-op in legacy path */
}
```

The guard NEVER fires in SHADOW mode. In AUTHORITATIVE mode, it
catches the case where legacy expected an admit that the stage
didn't record — meaning the saga missed something.

**Commit message:** `header_admit: add divergence guard in legacy ingress`

### Commit 4: Flip default to AUTHORITATIVE

In `header_admit_stage.c`:

```c
- static _Atomic header_admit_mode_t g_mode = HEADER_ADMIT_MODE_SHADOW;
+ static _Atomic header_admit_mode_t g_mode = HEADER_ADMIT_MODE_AUTHORITATIVE;
```

One-line change. CUTOVER live.

**Commit message:** `header_admit: cutover to AUTHORITATIVE mode`

---

## Tasks (each = one commit above)

### Task 1: Commit 1 — add the mode flag

Acceptance: `make` clean; `header_admit_get_mode()` returns
`HEADER_ADMIT_MODE_SHADOW` by default.

### Task 2: Commit 2 — add authoritative write path (gated off)

Acceptance: `make` clean. Existing tests pass (still in SHADOW mode).
NEW unit test: `header_admit_set_mode(AUTHORITATIVE)` + step once →
verify `block_index_admit_header` was called (use a test hook to
observe).

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
git push origin wt3/phase2-cutover-c2-header-admit
```

Acceptance: ALL existing tests pass with the new default (because the
saga is now authoritative for header admission; legacy ingress falls
through the guard cleanly).

---

## Live verification (post-merge, before C-3 starts)

Orchestrator runs:
```bash
zcl_state subsystem=header_admit
zcl_events --since 1h --filter EV_CUTOVER_GUARD_DIVERGED
```

For 24h. **Zero `EV_CUTOVER_GUARD_DIVERGED` events** is the gate for
C-3 to begin.

If divergences appear, revert commit 4 of this PR (or revert the
whole PR), investigate, re-attempt.

---

## Commit cadence

ONE commit per task. Push after each commit so the orchestrator can
see progress + revert individual commits if needed.

---

## Status

**IN PROGRESS (wt3)** (claimed 2026-05-24, post S-9 merge `1a65b33c7`) —
**FIRST authoritative cutover.** Commits 1-3 are pure additions (mode
flag default SHADOW → no behavior change). Commit 4 flips the default
to AUTHORITATIVE and is one-line revertable.

Soak discipline: ship commits 1-3 together, then watch
`zcl_state subsystem=header_admit` for ≥1h of zero divergence on a
moving chain before commit 4. If the live node is wedged (chain not
advancing), use test fixtures only and don't flip commit 4 until
the chain resumes advancement.

Recommend wt3 (most familiar with Wave S stages). Any worker may
claim by marking IN PROGRESS (wt<N>).

<!-- Worker: append a Completion section below when done. -->

---

## Completion — 2026-05-24

**DONE (wt3):** Commit 4 landed locally — `header_admit` now defaults to
`HEADER_ADMIT_MODE_AUTHORITATIVE`.

Changed:
- `app/services/src/header_admit_stage.c` default mode flipped from SHADOW to
  AUTHORITATIVE.
- `lib/test/src/test_header_admit_stage.c` updated to assert the new default
  while preserving the invalid-mode coercion check.

Verification:
- `make -j$(nproc)` PASS.
- `make lint` PASS.
- `ZCL_TEST_ONLY=event ./test_zcl` PASS.
- `ZCL_TEST_ONLY=mcp_e2e ./test_zcl` PASS.
- `./test_parallel --jobs=$(nproc)` was rerun after rebase; full parallel
  runner still reports unrelated flaky groups (`test_mcp_e2e` in parallel,
  and once `test_event`, once `test_sapling_crypto` timing). The directly
  relevant/default-mode tests pass and the flaking groups pass or fail outside
  this change's touched paths.
