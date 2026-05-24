# Worker Assignment — C-3 final-delete: remove legacy validate_headers fallback

**Worktree:** wt2 OR wt3 (either) — or isolated sub-agent worktree
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 2 (Wave S → S-12 cutover)
**Depends on:** C-3 cutover SHIPPED ✅ (`ad34efb65`); 24-hour live soak with
`validate_headers_get_mode() == AUTHORITATIVE` and no
`validate-headers-cutover-diverged` rejects on the chronic-regression node.
**Status: GATED on 24h soak** — claim only after the soak completes (check
`zcl_consensus_report` for any `validate-headers-cutover-diverged` entries
during the window).

**Owns:**
- EDIT `lib/validation/src/accept_block_header.c` — DELETE the
  `validate_headers_mode_t` enum (lines ~38-40), the `extern` declaration
  (~42), and the gated guard at line ~52
- EDIT `lib/validation/src/accept_block_header.c` — DELETE the legacy
  in-place header validation that was the SHADOW path (the code paths
  guarded by `validate_headers_get_mode() == AUTHORITATIVE` are now the
  ONLY path, so DELETE the wrapping `if`)
- EDIT `app/services/include/services/validate_headers_stage.h` — DELETE
  the `set_mode`/`get_mode` declarations
- EDIT `app/services/src/validate_headers_stage.c` — DELETE the `g_mode`
  atomic + `validate_headers_set_mode` + `validate_headers_get_mode`
  functions (now always authoritative)
- EDIT any caller of `validate_headers_set_mode` or `_get_mode` — remove
  the call (find via `grep -rn`)
- EDIT `tools/lint/check_no_raw_sqlite_in_controllers.sh` (or wherever the
  C-3-related WARN lint allowlist lives) if applicable
- EDIT `lib/test/src/test_validate_headers_stage.c` — DELETE tests that
  exercise SHADOW mode (mode-default test, mode-toggle test) — they're now
  testing a removed enum

**MUST NOT touch:**
- `validate_headers_stage.c`'s core verification logic — only mode-toggling.
- The cursor (`progress.kv`'s `validate_headers_height` key) — stays.
- The pass-row schema in `validate_headers_log` table — stays.
- Any other Wave S stage's mode enum (those have their own cutovers C-5..C-9).
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.

---

## Why this matters

The 4-commit C-3 cutover left both paths in the binary — the new
`validate_headers_stage` is authoritative, but the legacy in-place check in
`accept_block_header.c` is still compiled in as fallback. That's intentional:
we wanted one PR (the flip) to be reversible if production diverged.

After 24h of clean production data showing no divergences, the legacy code
is dead weight. This PR removes it.

Net delete: ~150-250 LOC out (one enum, one atomic global, two getters/
setters, one guarded fallback path). No new code. Binary shrinks.

---

## Pre-conditions (verify before starting)

Run on the canonical production node:

```bash
zcl_consensus_report | jq '.rejects[] | select(.reason == "validate-headers-cutover-diverged")'
```

Must return **no entries** since `ad34efb65` (the cutover commit) shipped.

Run on the same node:

```bash
zcl_state subsystem=validate_headers | jq '.mode'
```

Must return `"AUTHORITATIVE"`.

Also confirm:

```bash
zcl_status | jq '.health.checks'
```

No `chain_evidence` or `validate_headers` regressions in the past 24h
(check via `zcl_events`).

If all three pass: proceed. If any fail: do NOT claim this assignment;
investigate the divergence first.

---

## Tasks (in order)

### Task 1: Delete the mode enum + atomic

EDIT `lib/validation/src/accept_block_header.c`:
- DELETE lines ~38-40 (the `validate_headers_mode_t` enum).
- DELETE line ~42 (the `extern` declaration).
- At line ~52 and line ~246: DELETE the `validate_headers_get_mode() ==
  VALIDATE_HEADERS_MODE_AUTHORITATIVE` guards; the inner block stays.

EDIT `app/services/include/services/validate_headers_stage.h`:
- DELETE the `set_mode`/`get_mode` declarations + the `validate_headers_mode_t`
  enum if it's also forward-declared there.

EDIT `app/services/src/validate_headers_stage.c`:
- DELETE the `g_mode` static atomic (line ~81-82).
- DELETE the `validate_headers_set_mode` function (lines ~470-474).
- DELETE the `validate_headers_get_mode` function (lines ~477+).
- DELETE references in lines ~395, ~465, ~576 — those were SHADOW-mode log
  formatting; replace `"authoritative"` literal directly.

**Acceptance:** clean rebuild PASS; no `validate_headers_mode_t` references
remain (verify via `grep -rn 'validate_headers_mode_t\|validate_headers_get_mode\|validate_headers_set_mode' .`).

### Task 2: Remove the legacy fallback path

In `lib/validation/src/accept_block_header.c`: find the block of code that
was the legacy SHADOW path (the in-place header validation that the
authoritative stage now owns). DELETE it.

Reference for what to keep vs delete:
- KEEP: the call site that consumes `validate_headers_stage`'s pass row
  (which is now the only path).
- DELETE: any per-header signature/PoW check that the stage already
  performs.

The cutover commit (`ad34efb65`) added a guard that REFUSED the legacy
path when authoritative. This PR removes both the guard AND the now-unreachable
code it guarded. Read `git show ad34efb65` for context.

**Acceptance:** `make -j$(nproc)` PASS; existing
`test_validate_headers_stage.c` tests still PASS (minus the deleted
SHADOW-mode cases).

### Task 3: Update tests

EDIT `lib/test/src/test_validate_headers_stage.c`:
- DELETE the test case that asserts mode defaults to AUTHORITATIVE.
- DELETE the test case that toggles mode and asserts behavior.
- KEEP all other tests (pass-row lookup, divergence guard tests are now
  testing the only path).

The cutover commit added these specifically to gate the flip — they served
their purpose.

**Acceptance:** `./test_parallel --jobs=$(nproc)` reports
`validate_headers_stage` test passes with the reduced case count.

### Task 4: Find other callers + lint

```bash
grep -rn 'validate_headers_set_mode\|validate_headers_get_mode\|VALIDATE_HEADERS_MODE_' app/ lib/ config/ tools/
```

Should return 0 hits after Tasks 1-3. If any remain: investigate (they
might be MCP diagnostics, in which case delete them too; if they're tests
of other things, fix).

EDIT any tooling that exposed the mode via MCP — `zcl_state
subsystem=validate_headers` dumper. Drop the `mode` field from the JSON
(it's always authoritative now).

**Acceptance:** `make lint` PASS; no orphan references.

### Task 5: Verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section to this file with: LOC deleted (`git show --stat
HEAD`), binary size delta (`ls -l zclassic23` before/after), commit sha.

---

## Acceptance

- Net delete: ≥ 100 LOC out, 0 LOC added (apart from comment updates).
- Binary size shrinks (verify via `ls -l zclassic23`).
- All existing tests pass (minus the deleted SHADOW-mode cases).
- `grep` for `validate_headers_mode_t` returns 0 hits in production code.
- `make lint` PASS.

---

## What this does NOT do

- Does NOT change the authoritative behavior shipped in `ad34efb65`.
- Does NOT touch the validate_headers stage's verification code itself.
- Does NOT change the cutover for any other stage (C-5..C-9).
- Does NOT delete the cutover spec
  (`docs/work/wt-phase2-cutover-c3-validate-headers.md`) — that's history.

---

## Commit cadence

One commit. The PR is a coherent deletion (~150-250 LOC out, mostly
mechanical).

---

## Status

**GATED on 24h soak.** Verify the three pre-conditions above before
claiming. Once soaked, this is a simple delete-only PR — perfect for any
worker.

After this lands, the C-3 cutover saga is **fully closed**: stage owns
the path, mode toggle is gone, legacy code is deleted, binary shrinks.
