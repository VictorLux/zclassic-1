# wt2 Assignment — Phase 1b: Projection Adoption (MVCC reader pattern)

**Worktree:** `~/github/zclassic23-2`
**Branch:** `wt2/phase1-projection-adoption`
**Phase:** 1 (adopt unused primitives)
**Depends on:** Phase 1a (mailbox adoption — merged into main)

**Owns (no other worker may touch):**
- `lib/util/include/util/projection.h` — extend with new typed queries (string, double)
- `lib/util/src/projection.c` — implementations
- `lib/framework/include/framework/projection.h` — NEW, typed sugar layer (mirrors `mailbox.h` pattern)
- `app/controllers/include/controllers/chain_projection.h` — NEW, domain-typed views over node.db
- `app/controllers/src/chain_projection.c` — NEW, implementation
- Edits to `tools/mcp/controllers/chain_controller.c` — rewire `h_zcl_getblockcount` only
- `lib/test/src/test_projection_adoption.c` — NEW, includes the MVCC-under-load test
- Edits to `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h` to register the new test

**MUST NOT touch:**
- `app/services/`, `app/jobs/`, `app/conditions/`, `app/supervisors/` — out of scope
- `lib/framework/` other than the new `projection.h` header (wt3 owns)
- `tools/mcp/controllers/ops_controller.c` (zcl_status / zcl_health / zcl_kpi are
  for a follow-up — too much surface for one assignment)
- Any handler other than `h_zcl_getblockcount`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

`lib/util/include/util/projection.h` (F-3, shipped Wave F) defines a
read-only handle over `node.db` that captures a frozen MVCC snapshot.
It has **zero production callers**. Every MCP tool that needs to read
chain state today goes through `mcp_node_rpc(...)` which serializes
JSON, re-parses it on return, and races against writers via the live
sqlite connection.

The destination (FRAMEWORK.md §5 "Hexagonal cut") is single-writer +
N readers, with readers using projections so they NEVER block the
writer and NEVER see torn writes. Phase 1b proves the pattern works
under live load on ONE handler. The remaining ~30 RPC-forwarding
handlers convert in Phase 3 alongside the mega-module dissolution.

---

## Architecture reference

- Primitive: `lib/util/include/util/projection.h` — `projection_open`,
  `projection_close`, `projection_query_int64`.
- Pattern to mirror: `lib/framework/include/framework/mailbox.h`
  (wt2's Phase 1a) — thin typed sugar over a util primitive.
- Existing RPC handler to convert: `tools/mcp/controllers/chain_controller.c`
  `h_zcl_getblockcount` at line 30 (`DEFINE_PT(h_zcl_getblockcount, ...)`).
- node.db path: live node's `~/.zclassic-c23/node.db` for live testing;
  use a temp DB for the stress test.

---

## Tasks (in order)

### Task 1: Extend `projection.h` typed queries

Add to `lib/util/include/util/projection.h`:

```c
/* SELECT one TEXT column; copies up to (out_cap-1) bytes + NUL. Returns 0/-1
 * same way as projection_query_int64. */
int projection_query_text(projection_t *p, const char *sql,
                          char *out, size_t out_cap);

/* SELECT one REAL column. */
int projection_query_double(projection_t *p, const char *sql, double *out);
```

Implement in `lib/util/src/projection.c`. Match the existing
parameter-free, one-row contract — same error semantics, same logging
shape. No callers yet — wire them in Task 3 / 4.

**Acceptance:** builds, `make lint` PASS.

### Task 2: Add `lib/framework/include/framework/projection.h` sugar layer

Mirror `framework/mailbox.h`. Expose a `FRAMEWORK_PROJECTION_OPEN(label, path)`
macro that opens with a label string (for log/metrics correlation) and
asserts open succeeded. Macro for a scoped `PROJECTION_QUERY_INT64_OR(p,
sql, dflt)` helper that returns the value or `dflt` on error and emits
a single structured log line on miss (no spamming on every poll).

Keep it small — maybe 40 LOC. Same header-only style as `mailbox.h`.

**Acceptance:** header compiles when included from a test file.

### Task 3: Add `chain_projection` — domain-typed views over node.db

Create `app/controllers/include/controllers/chain_projection.h`:

```c
/* Domain projections over node.db. One typed function per fact the MCP
 * tools need. Each call opens a fresh MVCC snapshot, reads, and closes
 * — cheap because SQLite WAL readers don't lock. */
int64_t chain_projection_best_block_height(void);     /* -1 on error */
int64_t chain_projection_best_header_height(void);    /* -1 on error */
/* Add others as call sites land — keep this thin until Phase 3. */
```

Implement in `app/controllers/src/chain_projection.c`. Each function:
1. Resolves the node.db path via the existing `node_db_path()` helper
   (find it; it lives in the storage layer).
2. Opens a projection with a label like `"chain.height"`.
3. Runs the SELECT (start with `SELECT MAX(height) FROM blocks` if that
   table exists — verify the actual schema by inspecting the existing
   `getblockcount` RPC handler).
4. Closes the projection.
5. Returns the int64.

If the projection open fails or returns -1, return -1; the caller is
responsible for falling back to the RPC chain (Task 4 implements the
fallback). Do NOT add try/catch retry loops — projections are cheap to
reopen on the next call.

**Acceptance:** builds, `chain_projection_best_block_height()` returns
the same value as the existing RPC handler when called against a live
node.db.

### Task 4: Rewire `h_zcl_getblockcount` in chain_controller.c

Replace the `DEFINE_PT(h_zcl_getblockcount, "getblockcount", "mcp.chain")`
line with a hand-written handler that:

1. Calls `chain_projection_best_block_height()`.
2. If ≥ 0 → emits `{"height":N}` JSON in the response body directly.
   Note: the existing RPC contract returned the integer at the top level
   (just `N`). To preserve API compatibility, emit `N` directly (not
   wrapped). Verify against the existing wire format with
   `./tools/zcl-rpc getblockcount` before and after.
3. If < 0 → fall back to `mcp_node_rpc("getblockcount", NULL)` (drop-in,
   same path as today). Log a single structured event
   `[mcp.chain] projection miss → rpc fallback` (rate-limited if needed).

The fallback path exists because the projection may legitimately fail
when the node is mid-restart or boot. The condition engine will fire
if fallback usage spikes — that's a separate concern.

**Acceptance:** `./tools/zcl-rpc getblockcount` returns the same value
before and after the change. `./zclassic23 -mcp` initialize → tools/call
zcl_getblockcount returns the same N.

### Task 5: Add MVCC-under-load stress test

`lib/test/src/test_projection_adoption.c` should:

1. Create a temp DB with a `blocks(height INTEGER)` table seeded with
   1 row (height=100).
2. Open a writer connection in WAL mode.
3. Spawn 1 writer thread that does `UPDATE blocks SET height = height +
   1` in a loop with `usleep(100)` for 1000 iterations.
4. Spawn 4 reader threads that each open a projection, read height,
   close, in a tight loop until the writer is done.
5. Assert: zero reader observed a height < 100 or > 1100 (no torn
   writes, no impossible values).
6. Assert: at least 100 readers ran (sanity check the test did real work).
7. Assert: the writer's total update count == 1000 (readers didn't
   block the writer).

This is the load-bearing proof that the pattern works. If it doesn't
hold, fix `projection.c` before proceeding.

Register the test in `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
and `lib/test/include/test/test_helpers.h` per the existing convention
(grep for `test_mailbox_adoption` from Phase 1a for the pattern).

**Acceptance:** `./test_parallel --jobs=$(nproc)` includes the new test
and it PASSES.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint                                # all gates pass
./test_parallel --jobs=$(nproc)          # new test green, no regressions
./tools/zcl-rpc getblockcount            # parity check against live node
git push origin wt2/phase1-projection-adoption
```

Then append a Completion section per `docs/work/agent-protocol.md`.

**Acceptance:** all green; getblockcount parity holds.

---

## Commit cadence

One commit per task. After tasks 2, 4, 5: `git push origin
wt2/phase1-projection-adoption`.

Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Scope discipline note

If during Task 4 you discover `h_zcl_getblockcount`'s wire contract is
non-trivial (e.g., it actually returns wrapped JSON, not bare integer),
STOP and append a BLOCKED note to this doc. Do NOT silently change the
wire contract — `zcl-rpc` and other clients depend on it. The
orchestrator will decide whether to extend the assignment scope or
defer.

---

## Status

**✅ DONE — merged 2026-05-23** into main as `a96856925`. Next: `wt2-phase3-watchdog-dissolve-pr1.md`.

<!-- Worker: append a Completion section below when done. -->

## Completion (integration replay, 2026-05-23)

### Summary
Replayed the Phase 1 projection adoption work onto current `main` after
the S-5 body_persist merge. The branch adds the generic projection
primitive, the framework typed-query wrapper, chain height projections,
and rewires MCP `zcl_getblockcount` to read `node.db` directly with an
RPC fallback on projection miss.

### Commits
- `cd4ffdd84` wt2: start projection adoption
- `b3c7c9ee9` add projection typed queries
- `35dcc5837` route getblockcount through projection
- `0a7420a9e` test projection adoption under load

### Files added/modified
- `lib/util/include/util/projection.h`
- `lib/util/src/projection.c`
- `lib/framework/include/framework/projection.h`
- `app/controllers/include/controllers/chain_projection.h`
- `app/controllers/src/chain_projection.c`
- `tools/mcp/controllers/chain_controller.c`
- `lib/test/src/test_projection_adoption.c`
- `lib/test/include/test/test_helpers.h`
- `lib/test/src/test.c`
- `lib/test/src/test_parallel.c`

### Acceptance verification
- [x] `make -j$(nproc)` — PASS
- [x] `ZCL_TEST_ONLY=projection_adoption ./test_zcl` — PASS:
      `projection_adoption: 0 failures`
- [x] `make lint` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS:
      `ALL TESTS PASSED — 0/181 groups failed`
- [x] `./tools/zcl-rpc getblockcount` — PASS:
      `{"result":3121684,"error":null,"id":null}`
- [x] MCP `tools/call zcl_getblockcount` — PASS:
      text body `3121684`

### Surprises / follow-ups
The replay only conflicted in `lib/test/src/test_parallel.c` because
current `main` already had the S-5 `body_persist_stage` test entry. The
resolution keeps both `projection_adoption` and `body_persist_stage`.

### Status
DONE — branch `integration/phase1-projection-adoption` is ready for
orchestrator review/merge.
