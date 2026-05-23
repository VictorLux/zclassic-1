# wt3 Assignment — Phase 0b: Framework Shape Lint Gate (WARN mode)

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase0-framework-shape-lint`
**Phase:** 0 (foundation)
**Depends on:** scaffold commit (already in `main` when you start)
**Owns (no other worker may touch):**
- `tools/lint/framework_shape_check.sh` (new)
- `tools/lint/framework_shape_allowlist.txt` (new)
- `tools/lint/check_no_raw_clock_outside_platform.sh` (new — Phase 1 prep, WARN mode)
- `tools/lint/check_no_raw_sqlite_in_controllers.sh` (new — Phase 1 prep, WARN mode)
- `lib/framework/include/framework/condition.h` (STUB ONLY — wt2 fleshes it out, but you create the placeholder so the include path exists)
- Edits to `Makefile` — add new gates to `make lint`
- Edits to `DEFENSIVE_CODING.md` — document new gates #18, #19, #20

**MUST NOT touch:**
- Any file under wt2's assignment scope (see `wt2-condition-engine.md`)
- Any existing `app/services/src/*.c` mega-module
- `docs/REFACTOR_STATUS.md` (orchestrator only)
- `docs/FRAMEWORK.md`
- `CLAUDE.md`
- `lib/framework/src/condition.c` (wt2 owns this)
- Files in `app/conditions/src/` (wt2 owns)
- Files in `app/supervisors/src/` (wt2 owns)

---

## Why this matters

The framework refactor only stays on-shape if the build refuses
violations. This assignment lands the **lint ratchet** — three new gates
that count violations today (WARN mode) and tighten to FAIL mode in
Phase 1-2 as violations are fixed.

Without this, the framework is just a doc. With this, it's enforced
in CI.

---

## Architecture reference

Read [`docs/FRAMEWORK.md`](../FRAMEWORK.md) § 6 (composition rule, lint
gates) and `DEFENSIVE_CODING.md` for the existing 17 gates and their
pattern. Look at `tools/lint/` for examples (e.g., `check_no_raw_sqlite_step.sh`).

The pattern every gate follows:

```bash
#!/usr/bin/env bash
# tools/lint/<gate_name>.sh
# Gate #N: <one-sentence description>
# Mode: WARN | FAIL  (controlled by env var ZCL_LINT_MODE; default WARN initially)
set -euo pipefail

MODE="${ZCL_LINT_MODE:-WARN}"
violations=0

# ... scan rules ...
# Each violation: increment $violations, print to stderr with file:line

if [ "$violations" -gt 0 ]; then
    echo "[gate #N] $violations violation(s) found"
    if [ "$MODE" = "FAIL" ]; then exit 1; fi
fi
exit 0
```

---

## Tasks (in order)

### Task 1: Stub `lib/framework/include/framework/condition.h`

Just enough for wt2 to find the header. One forward declaration + a
comment that wt2 owns the implementation. This unblocks the include
path so lint runs cleanly even before wt2 ships.

```c
/* lib/framework/include/framework/condition.h
 *
 * Condition shape — auto-healing primitive.
 * This is a stub created by wt3 to establish the include path.
 * Real implementation shipped by wt2 in branch wt2/phase0-condition-engine.
 * See docs/FRAMEWORK.md § 3.6.
 */
#ifndef ZCL_FRAMEWORK_CONDITION_H
#define ZCL_FRAMEWORK_CONDITION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — full struct in wt2's implementation. */
struct condition;
void condition_register(const struct condition *cond);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_FRAMEWORK_CONDITION_H */
```

**Acceptance:** file exists, compiles standalone via gcc -c with no source.

### Task 2: Write `tools/lint/framework_shape_check.sh`

**Gate #18: framework shape check.** Verifies every `.c` file under
`app/` lives in one of the eight known shape folders:

```
app/controllers/src/
app/services/src/
app/models/src/
app/jobs/src/
app/supervisors/src/
app/conditions/src/
app/events/src/
app/views/src/
```

For each `.c` file under `app/`:
- Extract the immediate parent's parent (the shape folder).
- If it's not one of the eight → record as violation.
- Print: `<file>: not in a known shape folder (expected one of: controllers, services, models, jobs, supervisors, conditions, events, views)`.

Output format:
```
[framework_shape_check] scanned N .c files in app/
[framework_shape_check] V violation(s) found (mode: WARN)
[framework_shape_check] write to tools/lint/framework_shape_allowlist.txt to allowlist existing violations
```

**WARN mode is the default for Phase 0.** Phase 2 ratchets to FAIL.

**Acceptance:** script runs; reports current count (likely 0 or close —
existing files already match this convention).

### Task 3: Write `tools/lint/framework_shape_allowlist.txt`

Pre-populate with any current `.c` files in `app/` that DON'T match the
shape convention (run Task 2's script in WARN mode to find them). Each
line: relative path. Lines starting with `#` are comments.

If the count is 0 (likely), commit an empty file with a comment header:

```
# framework_shape_allowlist.txt
#
# Allowlist for tools/lint/framework_shape_check.sh
# Existing violations get listed here so the ratchet doesn't regress.
# Goal: empty by Phase 2.
#
# Format: one relative path per line; lines starting with # are comments.
```

**Acceptance:** allowlist file committed; lint passes in WARN mode.

### Task 4: Write `tools/lint/check_no_raw_clock_outside_platform.sh`

**Gate #19: clock discipline (Phase 1 prep).** Greps for direct
`clock_gettime(` and `time(NULL)` calls anywhere outside
`lib/platform/`. Reports as violations.

```bash
violations=$(grep -rn --include='*.c' --include='*.h' \
    -E '\bclock_gettime\s*\(|\btime\s*\(\s*NULL\s*\)' \
    app/ lib/ config/ tools/ \
    | grep -v '^lib/platform/' \
    | grep -v '// platform-ok' \
    | wc -l)
```

WARN mode initially. Phase 1 (after `platform.clock` adoption) flips to FAIL.

**Acceptance:** script runs; reports current count.

### Task 5: Write `tools/lint/check_no_raw_sqlite_in_controllers.sh`

**Gate #20: read-discipline (Phase 1 prep).** Greps for direct
`sqlite3_prepare_v2` or `sqlite3_exec` in `app/controllers/` and
`tools/mcp/controllers/`. Should go through `projection_*` or models
instead.

WARN mode initially. Phase 1 flips to FAIL.

**Acceptance:** script runs; reports current count.

### Task 6: Wire all three gates into `Makefile` `make lint` target

Find the existing `lint:` target. Add lines:

```make
lint:
    @# ... existing 17 gates ...
    @echo "→ Gate #18: framework_shape_check"
    @ZCL_LINT_MODE=WARN ./tools/lint/framework_shape_check.sh
    @echo "→ Gate #19: no_raw_clock_outside_platform"
    @ZCL_LINT_MODE=WARN ./tools/lint/check_no_raw_clock_outside_platform.sh
    @echo "→ Gate #20: no_raw_sqlite_in_controllers"
    @ZCL_LINT_MODE=WARN ./tools/lint/check_no_raw_sqlite_in_controllers.sh
```

Verify `make lint` runs end-to-end. Total wall time < 30s.

**Acceptance:** `make lint` succeeds (WARN doesn't fail); shows new gate output.

### Task 7: Document the three new gates in `DEFENSIVE_CODING.md`

Add a section "Framework refactor gates (#18-#20, ratcheting)" near the
existing lint-gate list. For each gate:
- Number, name, file path
- What it checks
- Current mode (WARN)
- When it ratchets to FAIL (which Phase)
- How to fix violations
- How to allowlist (if needed)

Keep the style of existing gate documentation.

**Acceptance:** `DEFENSIVE_CODING.md` updated; gates discoverable.

### Task 8: Run + record baseline counts in your assignment doc

Run `make lint` and capture the WARN counts. Add to the Completion section
of THIS assignment doc:

```
Baseline violation counts (2026-05-23, WARN mode):
- Gate #18 framework_shape_check: <N> violations
- Gate #19 no_raw_clock_outside_platform: <N> violations
- Gate #20 no_raw_sqlite_in_controllers: <N> violations
```

These numbers feed into `REFACTOR_STATUS.md`'s conformance metrics
(orchestrator updates).

**Acceptance:** baseline numbers recorded.

---

## Commit cadence

One commit per task. After tasks 3, 6: `git push origin wt3/phase0-framework-shape-lint`.

Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Push final

```bash
make lint            # WARN mode passes (or reports counts without failing)
make test_parallel   # all existing tests green (you haven't broken anything)
git push origin wt3/phase0-framework-shape-lint
```

---

## Status

**IN PROGRESS (wt3)** — framework shape lint gate implementation started 2026-05-23.

<!-- Worker: append a Completion section below when done, per agent-protocol.md -->
