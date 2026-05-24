# Worker Assignment — Phase 4c finalize: zcl_block_index_diff MCP tool + unit tests

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 4 (Storage unification — 4c follow-on)
**Depends on:** Phase 4c Tasks 1-6 SHIPPED ✅ (block_index_projection
in production shadow mode).
**Status: READY** — claim by marking IN PROGRESS.

**Owns:**
- EDIT `tools/mcp/controllers/chain_controller.c` — add `zcl_block_index_diff`
  MCP tool handler + route registration
- EDIT `app/controllers/src/diagnostics_controller.c` — wire
  `block_index_projection` into `g_dumpers` (one line) + include the header
- NEW `lib/test/src/test_block_index_projection.c` — 9 unit test cases
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` — wire the new test

**MUST NOT touch:**
- `lib/storage/src/block_index_projection.{c,h}` (already shipped)
- `event_log` primitive (Phase 4a)
- `lib/storage/src/block_index_db.c` (Phase 4c Task 5 already wires shadow emit)
- Wave S, Phase 3, Phase 5, Phase 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 4c shipped Tasks 1-6 (event payload, projection skeleton, catch_up,
readers, shadow emit, boot wiring). The projection IS running in production
under shadow mode RIGHT NOW. What's missing for the cutover to be safe:

1. **`zcl_block_index_diff` MCP tool** — compares projection's commitment
   to live LevelDB's commitment. Must return `match: true` for 24h before
   the cutover PR is shipped.
2. **Unit tests** — confidence floor for future maintainers; verify
   commitment determinism + replay correctness.

Without these, the cutover is unverifiable. With them, the 4c-cutover PR
(`docs/work/wt-phase4c-cutover-block-index.md`) is unblocked.

**Reference implementation:** the 4c sub-agent already wrote both. It's
preserved in the worktree branch `worktree-agent-a44e0099a96b25d8c`. Copy
the implementations from:

- `tools/mcp/controllers/chain_controller.c` lines ~360-600
  (`h_zcl_block_index_diff` + the supporting `live_index_entry` /
  `absorb_live_entry` helpers + route registration)
- `lib/test/src/test_block_index_projection.c` (9 test cases, ~620 LOC)
- `app/controllers/src/diagnostics_controller.c` — add one line to
  `g_dumpers` registering `block_index_projection_dump_state_json` under
  the key `"block_index_projection"`

The implementations are ALREADY known-good (sub-agent ran lint + tests
PASS in its isolated worktree). The reason it didn't ship as part of
4c-Tasks-1-6 was a cherry-pick race with concurrent znam sub-agent
pushes that wiped the orchestrator's in-flight merge.

---

## Tasks (in order)

### Task 1: Copy the diff MCP tool

READ `.claude/worktrees/agent-a44e0099a96b25d8c/tools/mcp/controllers/chain_controller.c`
lines 360-601.

ADD that content to the current `tools/mcp/controllers/chain_controller.c`
between the existing `h_zcl_diff_staged_header_admit` handler and the
existing `h_zcl_utxo_audit` handler. Include both the helper structs/
functions (`live_index_entry`, `live_entry_cmp`, `absorb_live_entry`) and
the main handler.

EXTEND the `k_routes[]` array with the registration:
```c
{ "zcl_block_index_diff", "chain",
  "Phase 4c: compare the block_index_projection (SQLite-backed) "
  "against the live in-memory block_map (canonical view of LevelDB). "
  "Returns {match, projection_commitment, leveldb_commitment, "
  "projection_count, leveldb_count, first_diff}. Read-only. "
  "Gates the cutover PR — 24h of match=true on every hourly call "
  "is the green light to flip the projection authoritative.",
  NULL, 0, h_zcl_block_index_diff, 0, NULL },
```

ADD the required include at the top:
```c
#include "storage/block_index_projection.h"
#include "core/hash.h"  /* for sha3_256 */
#include "util/safe_alloc.h"  /* for zcl_calloc */
```

The handler uses `zcl_calloc(live_cap, sizeof(*live), "block_index_diff/live")`.
**Do NOT replace this with raw `calloc()` — that would trip the lint gate.**

**Acceptance:** `make -j$(nproc)` clean; `make lint` PASS;
`zcl_block_index_diff` shows in `zcl_tools_list`.

### Task 2: Wire diagnostics dumper

READ `.claude/worktrees/agent-a44e0099a96b25d8c/app/controllers/src/diagnostics_controller.c`
search for `block_index_projection`.

EDIT main's `app/controllers/src/diagnostics_controller.c`:
1. Add `#include "storage/block_index_projection.h"` to the include block.
2. Add `{ "block_index_projection", block_index_projection_dump_state_json },`
   to the `g_dumpers` array.

**Acceptance:** `zcl_state subsystem=block_index_projection` returns a
JSON object (rather than "unknown subsystem").

### Task 3: Copy the unit tests

COPY `.claude/worktrees/agent-a44e0099a96b25d8c/lib/test/src/test_block_index_projection.c`
to `lib/test/src/test_block_index_projection.c` verbatim.

EDIT `lib/test/include/test/test_helpers.h` — declare `test_block_index_projection`.

EDIT `lib/test/src/test.c` — add `failures += test_block_index_projection();`.

EDIT `lib/test/src/test_parallel.c` — add `"block_index_projection"` to
the TEST_LIST array (alphabetical insert).

**Acceptance:** `./test_parallel --jobs=$(nproc)` reports
`block_index_projection: 9 passed, 0 failed`.

### Task 4: Update `test_mcp_controllers.c`

If the test asserts the enum_csv of `zcl_state` subsystems, add
`block_index_projection` to the expected list.

If `test_mcp_controllers.c` asserts the set of registered chain routes,
add `zcl_block_index_diff` to the expected set.

(Look for `EXPECTED_OPS`, `EXPECTED_CHAIN_ROUTES`, or similar markers.)

**Acceptance:** `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` PASS.

### Task 5: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append a Completion section to this file.

---

## Live verification (post-merge)

```bash
zcl_block_index_diff
# Expect: {"match": true, "projection_commitment": "...",
#          "leveldb_commitment": "...same...",
#          "projection_count": N, "leveldb_count": N,
#          "first_diff": null}
```

If `match: false` on the first call: investigate. The projection has been
in shadow mode for several hours / blocks by the time this PR ships, so
a divergence indicates either (a) a bug in the shadow emit (Task 5 in
the earlier batch), (b) a bug in the projection's replay (Task 2-4),
or (c) a bug in this PR's commitment computation. The detailed
`first_diff` field tells which.

---

## What this does NOT do

- Does NOT cut over (that's `wt-phase4c-cutover-block-index.md`).
- Does NOT delete LevelDB (that's the 4c-final-delete PR after cutover).
- Does NOT touch the projection's storage / replay logic — already shipped.
- Does NOT change the boot path — already wired in Task 6.

---

## Commit cadence

One commit per task. Push after task 3.

---

## Status

**READY** — Tasks 1-6 already on main; reference impl preserved in
`worktree-agent-a44e0099a96b25d8c`. Any worker may claim.

<!-- Worker: append a Completion section below when done. -->
