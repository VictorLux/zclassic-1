# Worker Assignment — Phase 4d-1: mempool_projection

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 4 (Storage unification)
**Depends on:** Phase 4a (event_log primitive) merged.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4d
**Batch spec:** [`docs/work/wt-phase4d-projections-batch.md`](./wt-phase4d-projections-batch.md) § 4d-1

**Owns:**
- NEW `lib/storage/include/storage/mempool_projection.h`
- NEW `lib/storage/src/mempool_projection.c`
- NEW `lib/test/src/test_mempool_projection.c`
- EDIT `lib/storage/include/storage/event_log_payloads.h` — add `ev_tx_admit_mempool` + `ev_tx_remove_mempool` payload structs and serialize/parse helpers
- EDIT `app/models/src/mempool_entry.c` — shadow-emit `EV_TX_ADMIT_MEMPOOL` on `db_mempool_save` and `EV_TX_REMOVE_MEMPOOL` on `db_mempool_remove*` (legacy SQLite write stays authoritative)
- EDIT `config/src/boot_services.c` — open projection alongside `peers_projection` + `znam_projection`, run `catch_up()` at boot, close in shutdown ordering
- EDIT `app/controllers/src/diagnostics_controller.c` — register `mempool_projection` in `g_dumpers`
- EDIT `tools/mcp/controllers/ops_controller.c` — add `mempool_projection` to the `zcl_state.subsystem` enum_csv
- EDIT `tools/mcp/controllers/chain_controller.c` (or wherever projection-diff tools live) — add `zcl_mempool_projection_diff`
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h` — register the new test
- EDIT `lib/test/src/test_mcp_controllers.c` — bump `EXPECTED_TOTAL` + `EXPECTED_*` counters for the new MCP tool and the new `zcl_state.subsystem` enum entry

**MUST NOT touch:**
- `lib/storage/src/event_log.c` — Phase 4a primitive; pure consumer here
- Existing projections: `utxo_projection`, `block_index_projection`, `peers_projection`, `znam_projection`
- Wave S stage files
- Other storage layers (`coins_db`, `progress_store`, `schema_migration`)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`
- Wallet, network, or chain modules outside the listed edits
- The mempool eviction policy itself — admission/removal call sites must be left intact aside from a single shadow-emit line at the end

---

## Why this matters

Mempool admission and removal are the highest-frequency write rate in
the system after IBD finishes. Today `app/models/src/mempool_entry.c`
writes directly to the `mempool` + `mempool_spends` SQLite tables via
`db_mempool_save` / `db_mempool_remove*`. Adding an event-log shadow
proves the event log can survive sustained mempool churn (50-200
tx/min) without falling behind. The diff tool against the live mempool
becomes the soak gate before the in-memory cutover lands.

After 4d-1 ships + the (separate) cutover PR ratifies authority:
- `mempool` + `mempool_spends` SQLite tables can be deleted from
  `~/.zclassic-c23/node.db` — projection rebuilds from event log on
  boot, with a configurable trailing window (last 10K events) for
  RAM-only operation in the dream end-state
- `db_mempool_save` / `db_mempool_remove_*` direct writes can be
  removed from `mempool_entry.c`

This PR is **shadow only** — both SQLite and projection get written.
The cutover is a separate one-line PR after a 24h soak with zero
divergence on `zcl_mempool_projection_diff`.

---

## API

```c
/* lib/storage/include/storage/mempool_projection.h */
#ifndef ZCL_STORAGE_MEMPOOL_PROJECTION_H
#define ZCL_STORAGE_MEMPOOL_PROJECTION_H

#include "storage/event_log.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct mempool_projection mempool_projection_t;

mempool_projection_t *mempool_projection_open(const char *path,
                                              event_log_t *log);
void mempool_projection_close(mempool_projection_t *p);

/* Consume new events. Idempotent. Returns new last_consumed_offset or
 * (uint64_t)-1 on error. */
uint64_t mempool_projection_catch_up(mempool_projection_t *p);

/* Lookup by txid. Returns true if present in the projection's mempool. */
bool mempool_projection_get(mempool_projection_t *p,
                            const uint8_t txid[32],
                            int64_t *fee_out, uint32_t *size_out,
                            uint32_t *weight_out,
                            uint32_t *admitted_unix_out);

/* Aggregate counters used by the diff tool. */
uint64_t mempool_projection_count(mempool_projection_t *p);
int64_t  mempool_projection_total_fee(mempool_projection_t *p);
uint64_t mempool_projection_total_weight(mempool_projection_t *p);

/* Shadow-emit globals: write sites in mempool_entry.c call these. */
void mempool_projection_set_event_log(event_log_t *log);
bool mempool_projection_emit_admit(const uint8_t txid[32],
                                   int64_t fee, uint32_t size_bytes,
                                   uint32_t weight,
                                   uint32_t admitted_unix,
                                   uint8_t priority_class);
bool mempool_projection_emit_remove(const uint8_t txid[32], uint8_t reason);

/* Diagnostics — see CLAUDE.md "Adding state introspection". */
struct json_value;
bool mempool_projection_dump_state_json(struct json_value *out,
                                        const char *key);

mempool_projection_t *mempool_projection_current(void);

#endif
```

---

## Tasks (in order)

### Task 1: Add `EV_TX_ADMIT_MEMPOOL` + `EV_TX_REMOVE_MEMPOOL` payloads

Edit `lib/storage/include/storage/event_log_payloads.h`. The enum
slots for `EV_TX_ADMIT_MEMPOOL` and `EV_TX_REMOVE_MEMPOOL` (ids 3 + 4)
are already declared in `event_log.h` per the batch spec.

```c
struct ev_tx_admit_mempool {
    uint8_t  txid[32];
    int64_t  fee;
    uint32_t size_bytes;
    uint32_t weight;             /* virtual size, post-witness */
    uint32_t admitted_unix;
    uint8_t  priority_class;     /* 0..3 */
    uint8_t  reserved[3];
};

struct ev_tx_remove_mempool {
    uint8_t  txid[32];
    uint8_t  reason;             /* 1=mined, 2=replaced, 3=expired, 4=conflict */
    uint8_t  reserved[7];
};
```

Add `ev_tx_admit_mempool_serialize/parse` and
`ev_tx_remove_mempool_serialize/parse` matching the shape used by
`ev_peer_observed_*` and `ev_znam_*`. Pure addition — no existing
consumer changes.

**Acceptance:** round-trip test (serialize → parse → compare) for both
events from known fixtures.

### Task 2: Projection skeleton (.h + .c) + SQLite schema

Stub all functions. `open()` creates a SQLite db with WAL + 5s
busy_timeout. Schema:

```sql
CREATE TABLE IF NOT EXISTS mempool (
    txid          BLOB PRIMARY KEY,
    fee           INTEGER NOT NULL,
    size_bytes    INTEGER NOT NULL,
    weight        INTEGER NOT NULL,
    admitted_unix INTEGER NOT NULL,
    priority      INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS projection_meta (
    k TEXT PRIMARY KEY,
    v TEXT NOT NULL
);
```

Use `INSERT OR REPLACE` for idempotent replay. Mirror the shape of
`znam_projection.c` exactly for `apply_pragmas`, `ensure_schema`,
`meta_get_u64`, `meta_set_u64`, `open`, `close`. Bump
`MEMPOOL_PROJECTION_SCHEMA_VERSION = 1`.

**Acceptance:** opens cleanly; `count()` == 0 on fresh; close/reopen
preserves the `last_consumed_offset` meta.

### Task 3: catch_up implementation

Iterate via `event_log_stream(start=last_consumed_offset, ...)`. On
`EV_TX_ADMIT_MEMPOOL`, `INSERT OR REPLACE` into `mempool`. On
`EV_TX_REMOVE_MEMPOOL`, `DELETE FROM mempool WHERE txid=?`. Commit
every 100 events; update `last_consumed_offset` in the same
transaction. Wrap the catch_up scan in `BEGIN IMMEDIATE` /
`COMMIT`/`ROLLBACK` (mirror `znam_projection_catch_up`).

**Acceptance:** synthetic event log with 1000 admits + 200 removes →
projection has 800 entries; `count()` matches; second `catch_up()` is
a no-op.

### Task 4: Reader API

Implement `mempool_projection_get`, `mempool_projection_count`,
`mempool_projection_total_fee`, `mempool_projection_total_weight`
using prepared `SELECT` statements with the `raw-sql-ok:projection-primitive`
marker, same as `znam_projection_name_count`.

**Acceptance:** insert known fixtures (3 admits with known fees +
weights); aggregate getters return the expected totals.

### Task 5: Shadow-emit at write sites

In `app/models/src/mempool_entry.c`:
- After `db_mempool_save` successfully runs `AR_FINISH_SAVE`,
  call `mempool_projection_emit_admit(txid, fee, size_bytes, weight,
  admitted_unix, priority_class)`. Synthesize `weight` from `size_bytes`
  if there is no separate post-witness size (this is ZCL; pre-segwit
  semantics — use `size_bytes` for both).
- After `db_mempool_remove_spends` / any other mempool removal, call
  `mempool_projection_emit_remove(txid, reason_code)`. Default reason
  if not known is `4=conflict`.

If emit fails, log via `obs-ok:` stderr marker and continue
(shadow mode never blocks the authoritative path). Bump
`g_emit_admit_total` / `g_emit_remove_total` / `g_emit_fail_total`
atomics in the projection module.

**Acceptance:** existing `mempool_entry` tests still pass.
`g_emit_admit_total` increments on a successful admit.

### Task 6: Boot wiring

In `config/src/boot_services.c`, after the event log is opened (Phase
4a) and alongside `peers_projection` / `znam_projection`:

1. `mempool_projection_open("<datadir>/mempool_projection.db", log)`
2. `mempool_projection_catch_up(p)` once
3. `mempool_projection_set_event_log(log)` so shadow-emit can find the
   log without threading it through every call site
4. Shutdown order: close after the legacy mempool model is quiesced

**Acceptance:** node boots clean.
`zcl_state subsystem=mempool_projection` returns `open: true` and a
non-negative `last_consumed_offset`.

### Task 7: `zcl_mempool_projection_diff` MCP tool

Add a new MCP tool that returns:

```json
{
  "projection_count": 7421,
  "live_count": 7421,
  "projection_total_fee": 18230000,
  "live_total_fee": 18230000,
  "projection_total_weight": 9842110,
  "live_total_weight": 9842110,
  "first_diff": null,
  "match": true
}
```

Implementation: read the live mempool via the existing
`db_mempool_each` iterator; build a sorted list of txids; iterate the
projection in the same order; compare bytewise. Return the first
differing txid (as hex) in `first_diff` when `match=false`.

Mirror the wiring used by `zcl_znam_projection_diff`:
- RPC handler in `app/controllers/src/diagnostics_controller.c`
  (or wherever the existing `znamprojectiondiff` handler lives)
- MCP tool registration alongside the other `_projection_diff` tools

**Acceptance:** on a freshly built node post-IBD with empty mempool,
returns `match: true`, all counts 0. After sending a tx via
`zcl_send`, returns `match: true` with `projection_count=1`,
`live_count=1`.

### Task 8: Diagnostics dump

Register `mempool_projection_dump_state_json` in
`app/controllers/src/diagnostics_controller.c:g_dumpers`. The dump
returns:

```json
{
  "open": true,
  "path": "...",
  "last_consumed_offset": 12345,
  "entry_count": 7421,
  "total_fee": 18230000,
  "total_weight": 9842110,
  "events_consumed_total": 9620,
  "emit_admit_total": 7421,
  "emit_remove_total": 2199,
  "emit_fail_total": 0,
  "last_catch_up_ms": 4
}
```

Add `mempool_projection` to the `zcl_state.subsystem` enum_csv in
`tools/mcp/controllers/ops_controller.c` and to the assertion list in
`lib/test/src/test_mcp_controllers.c` if that test enumerates the CSV.

**Acceptance:** `zcl_state subsystem=mempool_projection` returns the
expected JSON.

### Task 9: test_mempool_projection.c + wire expected counts

Test cases:
1. **`open_close_clean`** — open empty, close, reopen, offset=0.
2. **`single_admit_consumed`** — emit 1 admit, catch_up, `get()`
   returns it with correct fee/size/weight.
3. **`remove_then_count`** — emit admit, then remove (same txid);
   `count()` == 0.
4. **`replay_idempotent`** — second catch_up is a no-op.
5. **`replace_on_collision`** — emit 2 admits for the same txid with
   different fees; final fee reflects second admit.
6. **`aggregate_totals`** — emit 3 admits with fees 100/200/300;
   `total_fee()` == 600.
7. **`resume_from_partial`** — emit 100 events; manually rewind
   `last_consumed_offset` to event 50; reopen; catch_up consumes only
   the suffix; counts match the full state.
8. **`emit_set_global`** — `mempool_projection_set_event_log` +
   `emit_admit` produces an event readable via `event_log_stream`.

Wire into `lib/test/src/test.c` (run + `ZCL_TEST_ONLY` block),
`lib/test/include/test/test_helpers.h`, and `lib/test/src/test_parallel.c`.

Update `lib/test/src/test_mcp_controllers.c`:
- Bump `EXPECTED_TOTAL` by 1 (for `zcl_mempool_projection_diff`).
- Bump the relevant per-domain `EXPECTED_*` counter (likely
  `EXPECTED_CHAIN` or `EXPECTED_OPS` — match the controller domain
  where `znamprojectiondiff` was registered).
- If the test asserts the `zcl_state.subsystem` enum_csv list,
  include `mempool_projection`.

**Acceptance:** `ZCL_TEST_ONLY=mempool_projection ./test_zcl` →
0/8 cases fail. `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` → 0
failures.

### Task 10: Final verify + push

```bash
make -j$(nproc)
make lint
ZCL_TEST_ONLY=mempool_projection ./test_zcl
ZCL_TEST_ONLY=mcp_controllers ./test_zcl
ZCL_TEST_ONLY=mcp_e2e ./test_zcl
./test_parallel --jobs=$(nproc)
git push origin main
```

Append a `Completion` section to **this file** with commit hashes +
verification output (mirror the 4d-2 / 4d-4 completion blocks in
`wt-phase4d-projections-batch.md`).

---

## Live verification block

After push + node restart on the test instance, the gate is:

```text
zcl_mempool_projection_diff →
  { "match": true, "first_diff": null,
    "projection_count": <N>, "live_count": <N>,
    "projection_total_fee": <F>, "live_total_fee": <F>,
    "projection_total_weight": <W>, "live_total_weight": <W> }
```

Orchestrator polls this hourly for 24h. **Zero mismatches** is the
gate for the 4d-1 cutover PR (separate, makes the projection
authoritative and deletes the SQLite mempool tables).

---

## Commit cadence

One commit per task. Push after Task 4, Task 7, Task 9.

---

## Status

**DONE — pushed 2026-05-24** to main as commit `da005eb31`.

## Completion (wt2, 2026-05-24)

### Summary
Phase 4d-1 mempool projection shipped in shadow mode. The node now
emits `EV_TX_ADMIT_MEMPOOL` / `EV_TX_REMOVE_MEMPOOL`, replays them into
`mempool_projection.db`, exposes diagnostics through `zcl_state
subsystem=mempool_projection`, and registers `zcl_mempool_projection_diff`
for live-vs-projection soak checks.

### Commits
- `da005eb31` mempool_projection: add shadow replay

### Files Added/Modified
- `lib/storage/include/storage/mempool_projection.h`
- `lib/storage/src/mempool_projection.c`
- `lib/test/src/test_mempool_projection.c`
- `lib/storage/include/storage/event_log_payloads.h`
- `app/models/src/mempool_entry.c`
- `config/src/boot_services.c`
- `app/controllers/src/diagnostics_controller.c`
- `tools/mcp/controllers/diagnostics_controller.c`
- `lib/test/include/test/test_helpers.h`
- `lib/test/src/test.c`
- `lib/test/src/test_parallel.c`
- `lib/test/src/test_mcp_controllers.c`

### Acceptance Verification
- [x] `ZCL_TEST_ONLY=mempool_projection ./test_zcl` — PASS, 0 failures
- [x] `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` — PASS, 0 failures
- [x] `ZCL_TEST_ONLY=mcp_e2e ./test_zcl` — PASS, 0 failures
- [x] `make -j$(nproc)` — PASS
- [x] `make lint` — PASS; gate #20 remains WARN with grandfathered raw-controller-SQL violations
- [x] `./test_parallel --jobs=$(nproc)` — PASS, 0/201 groups failed

### Surprises / Follow-ups
The implementation landed before this assignment document was closed out,
so this completion update records the shipped commit and verification
without changing production code. Live 24h soak still needs the
orchestrator to poll `zcl_mempool_projection_diff`.
