# wt3 Assignment — Phase 1c: platform.clock + platform.rng Rewire

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase1-platform-rewire`
**Phase:** 1 (adopt unused primitives)
**Depends on:** Phase 0 (merged into main as of `4e0ea3382`)

**Owns (no other worker may touch):**
- Edits to ALL `.c` files that call `clock_gettime(`, `time(NULL)`, or `getrandom(` — except files under `lib/platform/` (which IS the primitive)
- Edits to `tools/lint/check_no_raw_clock_outside_platform.sh` (extend or sharpen)
- Edits to `Makefile` — only `make lint` target additions needed
- Edits to `DEFENSIVE_CODING.md` — flip gate #19 from WARN to FAIL (after rewire)
- New helper file if needed: `lib/platform/include/platform/time_compat.h` (sugar over `platform_clock_*` to make rewires mechanical)

**MUST NOT touch:**
- Files under `app/jobs/`, `app/conditions/`, `app/supervisors/` (wt2 owns Phase 1a)
- `lib/framework/` (wt2 owns)
- `app/services/src/header_admit_stage.c` (wt2 will edit in parallel)
- `app/services/src/header_probe_service.c` (wt2 will edit in parallel)
- `lib/util/src/mailbox.c` (kernel)
- `lib/platform/src/clock.c` (already correct — the primitive)
- `docs/REFACTOR_STATUS.md` (orchestrator only)
- `docs/FRAMEWORK.md`
- `CLAUDE.md`

---

## Why this matters

`platform.clock` (F-5a, `lib/platform/include/platform/clock.h`) shipped
with ZERO production rewires. There are ~443 sites still calling
`clock_gettime` or `time(NULL)` directly (per gate #19's WARN baseline).
This blocks the deterministic simulator (Phase 6) — you can't replay
a bug from a 64-bit seed if half the code reads the host clock directly.

This assignment **mechanically rewires every call site** outside
`lib/platform/`. The rewire is a 1-line change per site. After this
lands, gate #19 ratchets WARN → FAIL — no new code can introduce a
direct clock call.

---

## Architecture reference

- Primitive: `lib/platform/include/platform/clock.h` —
  `platform_clock_monotonic_us(void)` and `platform_clock_wall_unix(void)`
  are the canonical functions.
- Primitive: `lib/platform/include/platform/rng.h` —
  `platform_rng_fill(void *buf, size_t len)` and
  `platform_rng_u64(void)` are canonical.
- Gate: `tools/lint/check_no_raw_clock_outside_platform.sh` (WARN today).
- Existing override-marker convention: `// platform-ok:<tag>` allows
  a specific call site to remain raw (e.g., the platform impl itself,
  or signal handlers that must be async-signal-safe).

---

## Tasks (in order)

### Task 1: Survey + categorize call sites

Run:
```bash
grep -rn --include='*.c' --include='*.h' \
    -E '\b(clock_gettime|time\s*\(\s*NULL\s*\)|getrandom)\s*\(' \
    app/ lib/ config/ tools/ \
  | grep -v '^lib/platform/' \
  | grep -v '// platform-ok' \
  > /tmp/raw_clock_callsites.txt
wc -l /tmp/raw_clock_callsites.txt
```

Should be ~443 + ~N getrandom. Categorize:

- **(A) MECHANICAL** — most. `clock_gettime(CLOCK_MONOTONIC, &ts)` →
  `int64_t us = platform_clock_monotonic_us();`. `time(NULL)` →
  `platform_clock_wall_unix()`. `getrandom(buf, len, 0)` →
  `platform_rng_fill(buf, len);`.
- **(B) ALLOWED-RAW** — signal handlers, `lib/platform/` itself, very
  early boot before platform init. Add `// platform-ok:<reason>` marker.
- **(C) AMBIGUOUS** — anything that uses `clock_gettime(CLOCK_REALTIME, ...)`
  for wall-clock-with-nanoseconds. May need a new
  `platform_clock_wall_realtime_ns()` helper. Defer to Task 4.

Write the breakdown into the Completion section as you go.

**Acceptance:** survey file written + categorized counts.

### Task 2: Add a coccinelle-style sed pass for the MECHANICAL sites

Write `tools/lint/rewire_platform_clock.sh` that applies these
transformations using `sed -i` with patterns:

```
# clock_gettime(CLOCK_MONOTONIC, &ts);
# uint64_t us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#   ↓
# uint64_t us = (uint64_t)platform_clock_monotonic_us();
```

You'll need a few variants because the codebase uses several
conventions. Add `#include "platform/clock.h"` if missing and the file
now uses `platform_clock_*`.

Be conservative — only rewrite EXACT patterns you can verify. Anything
else: leave for Task 3 (manual).

**Acceptance:** script runs against a copy, produces a diff that compiles.

### Task 3: Apply sed pass + manual cleanup

Run the sed script. Build. Fix any compile errors manually (mostly:
missing includes, casts).

Then handle category (C) ambiguous sites manually — either:
- Add a new helper to `lib/platform/include/platform/clock.h` for the
  specific need, OR
- Mark the site `// platform-ok:<reason>` if truly necessary.

Goal: gate #19 count → **0** (or only allowlisted `// platform-ok:` lines).

**Acceptance:** `make` builds clean; `./tools/lint/check_no_raw_clock_outside_platform.sh`
reports 0 violations.

### Task 4: Same for `getrandom` (smaller scope)

Apply the same survey/sed/cleanup to `getrandom(`. Use
`platform_rng_fill()`.

If there are sites that need raw `getrandom` for kernel-entropy reasons,
mark `// platform-ok:rng-direct-entropy-source`.

**Acceptance:** all `getrandom` sites either use platform or are tagged.

### Task 5: Ratchet gate #19 to FAIL mode

Edit `tools/lint/check_no_raw_clock_outside_platform.sh`:
- Change default mode from WARN to FAIL.
- Update the message to say "ratchet now FAIL — no new raw clock calls allowed."

Edit `Makefile` lint target to drop the `ZCL_LINT_MODE=WARN` env var
for gate #19 (or just rely on the new default).

Edit `DEFENSIVE_CODING.md`: update the gate #19 description from
"WARN, ratchets in Phase 1" to "FAIL — added Phase 1 (2026-05-23)."

**Acceptance:** `make lint` passes with gate #19 in FAIL mode (because count is 0).

### Task 6: Final verify + push

```
make -j$(nproc)
make lint                                # gate #19 = FAIL mode, passes
./test_parallel --jobs=$(nproc)          # all green
git push origin wt3/phase1-platform-rewire
```

**Acceptance:** all green.

---

## Commit cadence

One commit per task. After tasks 1, 3, 4: `git push origin wt3/phase1-platform-rewire`.

Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Status

**IN PROGRESS (wt3)** — platform clock/RNG rewire started 2026-05-23.

<!-- Worker: append a Completion section below when done. -->
