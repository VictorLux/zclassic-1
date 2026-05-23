# wt3 Assignment — Phase 2: S-6 script_validate shadow stage

**Worktree:** `~/github/zclassic23-3`
**Branch:** `wt3/phase2-script-validate-shadow`
**Phase:** 2 (Wave S → S-12 cutover)
**Depends on:** Phase 2 S-5 (body_persist shadow — merged as `218b79bb4`)

**Owns:**
- NEW `app/services/include/services/script_validate_stage.h`
- NEW `app/services/src/script_validate_stage.c`
- NEW `lib/test/src/test_script_validate_stage.c`
- Edits to `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` to register the test
- Edits to the boot path (right after `body_persist_stage_init`)
- Schema migration for `script_validate_log` table

**MUST NOT touch:**
- `app/services/src/body_persist_stage.c` (just shipped — read it, don't edit it)
- `app/services/src/body_fetch_stage.c` (existing — read pattern only)
- `app/services/src/validate_headers_stage.c` (existing — read pattern only)
- `app/controllers/`, `tools/mcp/` (wt2 owns watchdog dissolution work)
- `app/conditions/` (wt2 owns)
- `lib/framework/`, `lib/util/`, `lib/platform/`
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Wave S has 8 stages. After this assignment, 6 of 8 are shipped (S-1
through S-6). Only S-7 (proof_validate) and S-8 (utxo_apply) +
S-9 (tip_finalize) remain before we can dissolve
`chain_advance_coordinator.c` (1,715 LOC).

This assignment ships S-6 as a **shadow stage** — verifies every input
script in every block body, logs success/failure per height, does not
mutate consensus state.

---

## Behaviour spec

For each height H starting at `script_validate_cursor + 1`:

1. **Floor:** if `H > body_persist_cursor` → STAGE_IDLE.
2. **Upstream log:** read `body_persist_log[H]`. If `ok=0`:
   log `script_validate_log[H] = (status='upstream_failed', ok=0,
   tx_count=0, input_count=0)`, advance cursor, continue.
3. **Block read:** look up the block by height (mirror body_persist
   step 3-4). If body unavailable: STAGE_IDLE.
4. **For each transaction in the block (skip coinbase):**
   - For each input: resolve the prevout, run script_verify with the
     appropriate sighash flags for the block's consensus rules at H.
   - Count inputs verified, inputs failed.
5. **Aggregate result:**
   - All inputs passed → log
     `(status='verified', ok=1, tx_count=N, input_count=M)`.
   - Any input failed → log
     `(status='script_invalid', ok=0, tx_count=N, input_count=M,
       first_failure_txid=..., first_failure_vin=...)`,
     emit ERROR event with the txid + vin.
   - Internal error (prevout missing, deserialize failed) → log
     `(status='internal_error', ok=0, ...)`, emit WARN event.
6. **Advance cursor** regardless of ok/fail — shadow stage just records.

Use the existing `script_verify_input` (or equivalent) entry point —
grep `lib/script/src/interpreter.c` or `lib/validation/src/connect_block.c`
to find the canonical call site and re-use it.

---

## Tasks (in order)

### Task 1: Schema migration for `script_validate_log`

```sql
CREATE TABLE IF NOT EXISTS script_validate_log (
    height               INTEGER PRIMARY KEY,
    status               TEXT    NOT NULL,
    ok                   INTEGER NOT NULL,
    tx_count             INTEGER NOT NULL,
    input_count          INTEGER NOT NULL,
    first_failure_txid   BLOB,
    first_failure_vin    INTEGER,
    validated_at         INTEGER NOT NULL
);
```

Add to `lib/storage/src/schema_migration.c` (or wherever `body_persist_log`
was added by S-5 — grep for it). Bump the migration version by 1.

**Acceptance:** `make test_parallel` storage tests PASS.

### Task 2: `script_validate_stage.h`

```c
/* script_validate_stage — Wave S, S-6 shadow stage.
 *
 * Consumes body_persist_log; for each height where the body was
 * verified-on-disk, runs script_verify on every input and logs the
 * result. Shadow mode: no mutation of consensus state. */
bool script_validate_stage_init(struct main_state *ms);
void script_validate_stage_shutdown(void);
bool script_validate_dump_state_json(struct json_value *out, const char *key);
```

### Task 3: `script_validate_stage.c`

Follow `body_persist_stage.c` structure 1:1 (read it cover-to-cover first).
Counters:
- `g_verified_total`, `g_script_invalid_total`, `g_internal_error_total`,
  `g_upstream_failed_total`
- `g_inputs_verified_total`, `g_inputs_failed_total`

Use the **workpool** primitive from `lib/util/include/util/workpool.h`
if available — script verification is the CPU-bound step that
parallelizes naturally. If workpool isn't available, single-threaded
is fine for shadow mode; we can parallelize later when cutover happens.

**Wire conventions:**
- `platform_time_wall_unix()` for timestamps (no raw clock)
- AR_* macros for any sqlite_step in app code (no raw `sqlite3_step`)
- Wire `script_validate_dump_state_json` into
  `diagnostics_controller.c:g_dumpers` so `zcl_state subsystem=script_validate`
  works
- Add `script_validate` to the enum_csv in `tools/mcp/controllers/ops_controller.c`
  (p_state[].enum_csv) and to `lib/test/src/test_mcp_controllers.c`'s
  enum_csv assertion

### Task 4: Wire init in the boot path

Find `body_persist_stage_init(ms)` (grep), add `script_validate_stage_init(ms)`
right after it.

### Task 5: Test `test_script_validate_stage.c`

Mirror `test_body_persist_stage.c`. Test cases:
- Happy path: 3 blocks with valid scripts → all `ok=1, status='verified'`.
- Script invalid: tamper with an input → `ok=0, status='script_invalid',
  first_failure_txid` populated.
- Upstream failure: body_persist_log row with `ok=0` → propagates as
  `status='upstream_failed', ok=0`.
- Internal error: missing prevout → `status='internal_error', ok=0`.
- Idle: H > body_persist_cursor → STAGE_IDLE.

Register in `test.c`, `test_parallel.c`, `test_helpers.h`.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt3/phase2-script-validate-shadow
```

Append Completion section per `docs/work/agent-protocol.md`.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.
Each commit ends with:
```
Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

---

## Status

**IN PROGRESS (wt3)** — started 2026-05-23; script_validate shadow stage branch active.

<!-- Worker: append a Completion section below when done. -->
