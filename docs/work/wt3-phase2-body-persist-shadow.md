# wt3 Assignment — Phase 2 starter: S-5 body_persist shadow stage

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase2-body-persist-shadow`
**Phase:** 2 (Wave S → S-12 cutover)
**Depends on:** Phase 1c (platform rewire — merged into main as `be9e05022`).
S-4 body_fetch shadow stage shipped pre-refactor as `95abed36d`.

**Owns:**
- NEW `app/services/include/services/body_persist_stage.h`
- NEW `app/services/src/body_persist_stage.c`
- NEW `lib/test/src/test_body_persist_stage.c`
- Edits to `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` to register the new test
- Edits to the boot path (search for `header_admit_stage_init` /
  `body_fetch_stage_init` call sites) to add `body_persist_stage_init`
- Schema migration if needed: add `body_persist_log` table — extend the
  existing migrations file under `lib/storage/src/migrations*`

**MUST NOT touch:**
- `app/controllers/`, `tools/mcp/` (wt2 owns the Phase 1b projection work)
- `app/services/src/header_admit_stage.c`, `app/services/src/body_fetch_stage.c`
  (existing stages — read their pattern, don't modify them)
- `app/services/src/validate_headers_stage.c` (existing — pattern reference)
- `lib/framework/`, `lib/util/`, `lib/platform/` (settled primitives)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

The Wave S saga (8 stages: header_admit → validate_headers → body_fetch →
body_persist → script_validate → proof_validate → utxo_apply →
tip_finalize) is the cure for the mega-modules. Each stage is a Job
in the framework sense: typed step, cursor on disk, idempotent, no
shared mutable state with peers.

S-1 through S-4b shipped pre-refactor. S-5 is next. Phase 2 finishes
S-5..S-12 over the next ~5 sessions; only THEN can we delete the
1,447-LOC `sync_watchdog_service.c` and the 1,715-LOC
`chain_advance_coordinator.c` (they exist to paper over the
non-saga ad-hoc chain advance path).

This assignment ships S-5 as a **shadow stage** — it observes the
live block-body pipeline and logs verifiability per height, but does
not mutate consensus state. Same playbook as S-4 (body_fetch).

---

## Architecture reference

- Pattern to mirror: `app/services/src/body_fetch_stage.c` (367 LOC,
  S-4). Read it cover-to-cover before starting. Same module shape,
  same cursor + log + idle semantics.
- Stage primitive: `lib/util/include/util/stage.h` —
  `stage_create`, `stage_register`, `stage_step_fn`,
  `STAGE_OK / STAGE_IDLE / STAGE_FAILED`.
- Stage log table convention: see `validate_headers_log`,
  `body_fetch_log` schemas in `lib/storage/src/migrations*` — they sit
  inside `progress.kv` (SQLite) alongside `stage_cursor`.
- progress_store: `lib/storage/include/storage/progress_store.h` —
  use the same API as body_fetch uses for cursor reads/writes.

---

## Behaviour spec

For each height H starting at `body_persist_cursor + 1`:

1. **Floor check:** if `H > body_fetch_cursor` → return STAGE_IDLE
   (cannot get ahead of body_fetch).
2. **Read upstream log:** look up `body_fetch_log[H]` →
   `(source, ok)`. If `ok = 0` (skipped_invalid or fetch failed):
   log `body_persist_log[H] = (source='upstream_failed', ok=0)`,
   advance cursor, continue.
3. **Look up block:** `active_chain_at(ms, H)` → `bi`. If bi == NULL or
   `(bi->nStatus & BLOCK_HAVE_DATA) == 0` → STAGE_IDLE (body should be
   on disk if upstream said ok; race — try again next tick).
4. **Read the body:** use the same path msg_blocks uses internally
   (`ReadBlockFromDisk` equivalent — grep the codebase). If read fails:
   log `body_persist_log[H] = (source='read_failed', ok=0)`, advance
   cursor (the failure is persistent — the on-disk body is corrupt;
   utxo_apply will fail later anyway and a higher-level recovery
   handles it). Emit a structured warn event.
5. **Verify header consistency:** the read block's header must match
   the admitted header (`bi->GetBlockHash() == read_block.GetHash()`).
   If mismatch: log `(source='header_mismatch', ok=0)`, advance cursor,
   emit ERROR event — this is a real corruption.
6. **Verify Merkle root:** recompute the tx Merkle root from the
   block body, assert == header's hashMerkleRoot. Mismatch: log
   `(source='merkle_mismatch', ok=0)`, advance, ERROR event.
7. **Success:** log `(source='verified', ok=1)`, advance cursor.

Shadow mode: nothing else. No write to the canonical blocks table, no
peer ban, no fetch trigger. The point is to prove the stage can run
correctly across the whole chain without affecting live operations.
The shadow-vs-live diff is what lets us flip the cutover safely in a
later sub-stage.

---

## Tasks (in order)

### Task 1: Schema migration for `body_persist_log`

Add a migration that creates:
```sql
CREATE TABLE IF NOT EXISTS body_persist_log (
    height      INTEGER PRIMARY KEY,
    source      TEXT    NOT NULL,
    ok          INTEGER NOT NULL,
    persisted_at INTEGER NOT NULL
);
```
Schema lives in `progress.kv` (the SQLite DB the other stage logs live
in — grep `validate_headers_log` to find the migrations file).

**Acceptance:** migration ladder includes the new table; `make test_parallel`
storage tests PASS.

### Task 2: Header `body_persist_stage.h`

```c
/* body_persist_stage — Wave S, S-5 shadow stage.
 *
 * Consumes body_fetch_log; for each height where the body is on disk,
 * reads the body, verifies header+merkle, and logs the result.
 * Shadow mode: no mutation of consensus state. */
bool body_persist_stage_init(struct main_state *ms);
void body_persist_stage_shutdown(void);
bool body_persist_dump_state_json(struct json_value *out, const char *key);
```

Mirror `body_fetch_stage.h` exactly for the dump_state_json signature
(see CLAUDE.md "Adding state introspection").

### Task 3: Implementation `body_persist_stage.c`

Follow `body_fetch_stage.c` structure 1:1:
- file header comment summarizing behaviour
- `STAGE_NAME` macro
- `pthread_mutex_t g_lock`, `struct main_state *g_ms`, `stage_t *g_stage`
- `_Atomic uint64_t` counters: `g_verified_total`,
  `g_upstream_failed_total`, `g_read_failed_total`,
  `g_header_mismatch_total`, `g_merkle_mismatch_total`,
  `g_last_step_unix`
- `ensure_log_schema()` idempotent table create (mirror header_admit)
- `step_persist()` — implements the 7-step behaviour spec above,
  returns STAGE_OK / STAGE_IDLE / STAGE_FAILED
- `body_persist_stage_init()` — wires the stage with
  `stage_create(STAGE_NAME, step_persist, NULL)` and registers it
- `body_persist_stage_shutdown()` — symmetric
- `body_persist_dump_state_json()` — emits cursor, all counters, log row
  count, last_step_age_seconds

Wire `body_persist_dump_state_json` into
`app/controllers/src/diagnostics_controller.c:g_dumpers` so it's
visible via `zcl_state subsystem=body_persist`.

**No raw clock calls** — use `platform_time_wall_unix()` from
`platform/time_compat.h` (Phase 1c discipline).
**No raw sqlite_step in app code** — use the AR_* macros from
`app/models/include/models/activerecord.h` (DEFENSIVE_CODING.md).

### Task 4: Wire init in the boot path

Find where `body_fetch_stage_init(ms)` is called (grep) and add
`body_persist_stage_init(ms)` right after it. The stage starts paused
if no `body_fetch_log` rows exist; that's fine.

### Task 5: Test `test_body_persist_stage.c`

Spin up a temp `main_state` with:
- A chain of 5 blocks with known headers and bodies.
- Seed `body_fetch_log` with `(1..5, source='disk', ok=1)`.
- Mark blocks 1..5 BLOCK_HAVE_DATA.

Test cases:
- Happy path: 5 stage steps advance the cursor 1..5, all
  `body_persist_log` rows have `source='verified' ok=1`.
- Upstream failure: seed `body_fetch_log[3] = (skipped_invalid, ok=0)`
  — assert h=3 logged as `source='upstream_failed' ok=0` and cursor
  advances past it.
- Header mismatch: tamper with `bi->GetBlockHash()` for h=2 — assert
  logged as `header_mismatch ok=0`.
- Merkle mismatch: tamper with a tx — assert `merkle_mismatch ok=0`.
- Idle: h=4 has no BLOCK_HAVE_DATA → returns STAGE_IDLE, cursor stays
  at 3.

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt3/phase2-body-persist-shadow
```

Append a Completion section per `docs/work/agent-protocol.md`.

---

## Live observability hook

After the stage is wired and running, the orchestrator will run the
live node and use `zcl_state subsystem=body_persist` to confirm the
cursor advances and the verified counter ticks up. The "shadow vs
live diff" report (counts of header_mismatch / merkle_mismatch in
production) is what gates the cutover in the next sub-stage.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.
Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Status

**IN PROGRESS (wt3)** — started 2026-05-23; body_persist shadow stage branch active.

<!-- Worker: append a Completion section below when done. -->
