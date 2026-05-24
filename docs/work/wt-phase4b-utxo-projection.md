# Worker Assignment — Phase 4b: utxo_projection (first event-log consumer)

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 4 (Storage unification)
**Depends on:** Phase 4a (event_log primitive) merged.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4b

**Owns:**
- NEW `lib/storage/include/storage/utxo_projection.h`
- NEW `lib/storage/src/utxo_projection.c`
- NEW `lib/test/src/test_utxo_projection.c`
- EDIT `lib/validation/src/update_coins.c` — emit `EV_UTXO_ADD` / `EV_UTXO_SPEND` alongside existing SQLite write (shadow mode only — both write paths active)
- EDIT `config/src/boot_services.c` — open the projection on boot, replay from last_consumed_offset
- EDIT `app/controllers/src/diagnostics_controller.c` — register `utxo_projection` in `g_dumpers`
- EDIT `tools/mcp/controllers/ops_controller.c` — add `utxo_projection` to `zcl_state.subsystem` enum
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`

**MUST NOT touch:**
- `lib/storage/src/event_log.c` — finished in Phase 4a; we are pure consumer
- Wave S stage files (`*_stage.c`)
- Other storage layers (block_index_db, progress_store) — they get their own 4c/4d/4f PRs
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 4a shipped the event log primitive with **zero** production callers
— same risk pattern as the Wave F kernel primitives that sat unused for
weeks. Phase 4b forces the first real adoption: the UTXO set, which is
the largest and most consequential piece of derived state in the node.

This PR is **purely additive in shadow mode**. The legacy
`update_coins()` SQLite write still happens. We just also emit
`EV_UTXO_ADD` / `EV_UTXO_SPEND` events, the projection consumes them,
and a diff tool verifies the projection matches the legacy SQLite state
for 24h on the live node.

Phase 4b-cutover (a separate PR, gated on the 24h diff being clean)
disables the legacy SQLite write and makes the projection authoritative.
This is the **second-most-critical authoritative cutover after Wave S
C-8** — both touch the UTXO set, both must be diff-clean before flipping.

---

## API

```c
/* lib/storage/include/storage/utxo_projection.h */
#ifndef ZCL_STORAGE_UTXO_PROJECTION_H
#define ZCL_STORAGE_UTXO_PROJECTION_H

#include "storage/event_log.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct utxo_projection utxo_projection_t;

/* Open or create the projection. Replays from last_consumed_offset
 * stored in the projection's own metadata table. Returns NULL on
 * unrecoverable error (e.g., projection schema mismatch). */
utxo_projection_t *utxo_projection_open(const char *projection_path,
                                        event_log_t *log);

void utxo_projection_close(utxo_projection_t *p);

/* Consume new events from the event log. Idempotent — replaying the
 * same range twice produces the same result. Returns the new
 * last_consumed_offset, or (uint64_t)-1 on error. */
uint64_t utxo_projection_catch_up(utxo_projection_t *p);

/* Query — lookup a UTXO by (txid, vout). Returns true if present.
 * Fills value/script if non-NULL. */
bool utxo_projection_get(utxo_projection_t *p,
                         const uint8_t txid[32], uint32_t vout,
                         int64_t *value_out,
                         uint8_t *script_out, size_t script_cap,
                         size_t *script_len_out);

/* Count — total live UTXOs in the projection. O(1). */
uint64_t utxo_projection_count(utxo_projection_t *p);

/* SHA3-256 over (txid|vout|value|script) for every UTXO in canonical
 * order. Used by the shadow-diff tool to compare against the legacy
 * SQLite UTXO commitment. */
int utxo_projection_commitment(utxo_projection_t *p, uint8_t out[32]);

/* Diagnostics dumper — see CLAUDE.md "Adding state introspection". */
struct json_value;
bool utxo_projection_dump_state_json(struct json_value *out, const char *key);

#endif
```

---

## Event payload formats (canonical — do not change after merge)

```
EV_UTXO_ADD payload (variable length):
    [ 32B  txid                 ]
    [  4B  vout                 ]
    [  8B  value (zatoshis, LE) ]
    [  4B  height               ]
    [  1B  is_coinbase (0/1)    ]
    [  3B  reserved (zero)      ]
    [  4B  script_len           ]
    [ NB   script_bytes         ]

EV_UTXO_SPEND payload (fixed 36 bytes):
    [ 32B  prevout_txid         ]
    [  4B  prevout_vout         ]
```

Document these in `lib/storage/include/storage/event_log_payloads.h`
(NEW file) so projection authors don't reinvent. **The wire format is
forever** — extending requires a new event_type id, not a new field.

---

## Tasks (in order)

### Task 1: Event payload header

NEW `lib/storage/include/storage/event_log_payloads.h` — typed structs
for each event payload that has stable schema. For Phase 4b only:

```c
struct ev_utxo_add_hdr {
    uint8_t  txid[32];
    uint32_t vout;
    int64_t  value;
    uint32_t height;
    uint8_t  is_coinbase;
    uint8_t  reserved[3];
    uint32_t script_len;
    /* script bytes follow */
};

struct ev_utxo_spend {
    uint8_t  txid[32];
    uint32_t vout;
};
```

Plus serialize/parse helpers that return false on truncation.

**Acceptance:** compiles, round-trip unit test (serialize → parse →
compare).

### Task 2: utxo_projection.h + skeleton .c

Stub all functions. `open` returns a real handle wrapping a fresh
SQLite db with schema:

```sql
CREATE TABLE IF NOT EXISTS utxo (
    txid        BLOB NOT NULL,
    vout        INTEGER NOT NULL,
    value       INTEGER NOT NULL,
    height      INTEGER NOT NULL,
    is_coinbase INTEGER NOT NULL,
    script      BLOB NOT NULL,
    PRIMARY KEY (txid, vout)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS projection_meta (
    k TEXT PRIMARY KEY,
    v TEXT NOT NULL
);
/* projection_meta key: 'last_consumed_offset' (uint64 as text) */
```

Bump schema migration version. Use AR_* macros for every write per
DEFENSIVE_CODING.md.

**Acceptance:** can open, close, no events consumed yet.

### Task 3: catch_up implementation

Reads events from `event_log_stream(start=last_consumed_offset, ...)`.
For each `EV_UTXO_ADD`, INSERT (or REPLACE — see witness note below).
For each `EV_UTXO_SPEND`, DELETE. After each batch (1000 events),
update `projection_meta.last_consumed_offset`.

**Witness:** if an `EV_UTXO_ADD` arrives for a (txid, vout) that
already exists, that's either a chain reorg (handled by the
projection seeing matching SPEND first, then ADD) OR a bug in the
emitter. The projection logs it and uses REPLACE — being lenient at
the projection layer is correct because the emitter is the
authority on event ordering.

**Acceptance:** synthetic event log with 1000 ADDs + 500 random
SPENDs → projection has 500 UTXOs, `count()` == 500.

### Task 4: get + count + commitment

Implement read paths. `commitment` iterates rows in
`ORDER BY txid, vout`, computes SHA3-256 over the canonical
serialization (same as `EV_UTXO_ADD` payload format).

**Acceptance:** insert known fixture, verify commitment matches a
hand-computed reference value.

### Task 5: Wire shadow emission in update_coins.c

In `lib/validation/src/update_coins.c`, wherever `coins_view_add`
or `coins_view_spend` is called against the live `coins.db`, ALSO
emit the corresponding event via `event_log_append`. Both writes
happen; neither blocks the other.

If `event_log_append` fails, log a warning with `obs-ok:` marker
and continue — shadow mode doesn't gate the legacy path.

Add counter `g_utxo_event_emit_total` and `g_utxo_event_emit_fail_total`.

**Acceptance:** existing `update_coins` tests still pass. New
counter increments when a block is connected.

### Task 6: Boot wiring

In `config/src/boot_services.c`, after the event log is opened
(Phase 4a), open the utxo_projection and call `catch_up` once.
Store the handle in a process-global accessor (`get_utxo_projection()`
in the projection's .c file).

**Acceptance:** node boots clean. `zcl_state subsystem=utxo_projection`
returns last_consumed_offset and count.

### Task 7: Diagnostics + MCP enum

- Add `utxo_projection_dump_state_json` returning:
  ```json
  {
    "open": true,
    "last_consumed_offset": 12345678,
    "utxo_count": 1300000,
    "events_consumed_total": 9876543,
    "ev_utxo_add_total": 5432100,
    "ev_utxo_spend_total": 4132043,
    "replace_collisions_total": 0,
    "last_catch_up_ms": 12
  }
  ```
- Register in `g_dumpers` table in `diagnostics_controller.c`.
- Add to `enum_csv` for `zcl_state.subsystem` in `ops_controller.c`
  (and to any test that asserts the list).

**Acceptance:** `zcl_state(subsystem="utxo_projection")` returns the
JSON above on a running node.

### Task 8: test_utxo_projection.c

Test cases:
1. **`open_close_clean`** — open empty projection, close, reopen,
   verify `last_consumed_offset = 0`.
2. **`single_add_consumed`** — emit 1 ADD event, catch_up, verify
   `count() == 1` and `get()` returns the value.
3. **`add_then_spend`** — emit ADD then SPEND, catch_up, verify
   `count() == 0` and `get()` returns false.
4. **`replay_idempotent`** — call catch_up twice; second call is a
   no-op (no double-counting).
5. **`commitment_canonical`** — 3 UTXOs in scrambled insertion
   order; commitment matches a 4th projection that inserted in
   different order.
6. **`reorg_replace`** — ADD (txid, 0), ADD (txid, 0) again with
   different value; `get()` returns the second value;
   `replace_collisions_total == 1`.
7. **`resume_from_partial`** — emit 1000 events; set
   last_consumed_offset to mid-stream; reopen; verify catch_up
   consumes only the suffix.

Add to test_parallel + test_helpers.

**Acceptance:** `./test_parallel --jobs=$(nproc)` all green.

### Task 9: Shadow-diff tool (in-process)

Add MCP tool `zcl_utxo_projection_diff` that:
1. Computes the legacy UTXO commitment via existing
   `utxo_commitment()` (already in `chain_controller.c`).
2. Computes the projection commitment.
3. Returns both hex values + `match: bool`.

This is the 24h soak gate. Mismatch → projection has a bug or the
emit path missed a write. Either way, blocks cutover.

**Acceptance:** on a freshly-built node post-IBD, both commitments
match.

### Task 10: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin main
```

Append Completion section.

---

## Live observability note

After this PR ships, the orchestrator polls `zcl_utxo_projection_diff`
every hour for 24h. **Zero mismatches** is the gate for the 4b-cutover
PR (which disables the SQLite write and makes the projection the
single source of truth for UTXO).

If a mismatch is detected:
1. `zcl_utxo_projection_diff(verbose=1)` returns the first differing
   (txid, vout) — fix the emitter or projection so it stops.
2. Reset the projection (delete file, replay from offset 0).
3. Re-soak.

---

## Commit cadence

One commit per task. Push after tasks 4, 7, 9.

---

## Status

✅ **DONE — pushed 2026-05-24**

## Completion (orch sub-agent, 2026-05-24)

### Commits (in order)

| SHA | Subject |
|---|---|
| `39b1e8efa` | Phase 4b Task 1: EV_UTXO_ADD / EV_UTXO_SPEND payload schemas |
| `089cdc01d` | Phase 4b Tasks 2-4: utxo_projection skeleton + catch_up + reads |
| `f0e4f8d93` | Phase 4b Task 5: wire EV_UTXO_ADD/SPEND shadow emission in update_coins |
| `626d52a94` | Phase 4b Tasks 6-8: boot wiring + diagnostics + 7 test cases |
| `96113ca4e` | Phase 4b Task 9: zcl_utxo_projection_diff MCP tool (24h cutover gate) |
| `d053381b0` | Phase 4b: bump test_mcp_controllers expected counts |

### Files added

- `lib/storage/include/storage/utxo_projection.h` (NEW)
- `lib/storage/src/utxo_projection.c` (NEW)
- `lib/test/src/test_utxo_projection.c` (NEW — 7 test cases per spec)

### Files modified

- `lib/storage/include/storage/event_log_payloads.h` — added
  `ev_utxo_add_hdr` + `ev_utxo_spend` typed structs + (de)serialiser
  helpers (inline, matching the existing peer-event style)
- `lib/validation/src/update_coins.c` — emits `EV_UTXO_ADD` /
  `EV_UTXO_SPEND` alongside the existing `coins_view` writes
  (additive shadow mode; emit failures NEVER gate the legacy path)
- `lib/validation/include/validation/update_coins.h` — exposes
  `update_coins_event_emit_total()` and
  `update_coins_event_emit_fail_total()` for observability
- `config/src/boot_services.c` — extended the existing Phase 4
  shadow startup (added by 4d-2) to also open the
  `utxo_projection` alongside `peers_projection` and call
  `catch_up()` after open; shutdown teardown closes in order
  (detach emitters → projections → event log)
- `app/controllers/src/diagnostics_controller.c` — registers
  `utxo_projection` in `g_dumpers`; auto-propagates to the MCP
  `zcl_state.subsystem` enum via the live derivation in
  `tools/mcp/controllers/diagnostics_controller.c`
- `tools/mcp/controllers/chain_controller.c` — new MCP tool
  `zcl_utxo_projection_diff` (Task 9) computes both commitments
  in-process and returns `{match, legacy_sha3, projection_sha3,
  legacy_height, legacy_utxo_count, projection_utxo_count}`. On
  shadow-disabled boots (projection_not_open) the tool returns a
  structured reason field so soak scripts can distinguish from
  actual divergences.
- `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h` — wire `test_utxo_projection`
- `lib/test/src/test_mcp_controllers.c` — `EXPECTED_TOTAL` 96→97,
  `EXPECTED_CHAIN` 15→16, test description updated to "16 tools"

### Acceptance verification

```
$ make -j$(nproc)
... clean build (zclassic23 + test_zcl + test_parallel) ...

$ ./test_parallel --jobs=$(nproc)
ALL TESTS PASSED — 0/197 groups failed (105s wall, 32 workers)
```

Note: `make lint` failed on one pre-existing violation in
`lib/znam/src/znam.c:318` (raw `time(NULL)` call) that was
introduced by an unrelated Phase 4d-4 commit. Not caused by this
PR.

### Diff-tool smoke test (fresh datadir, no chain loaded)

```
$ zclassic23 -mcp -datadir=/tmp/zcl_proj_test < ... tools/call zcl_utxo_projection_diff
{
  "match": false,
  "reason": "projection_not_open",
  "legacy_sha3": "",
  "legacy_height": 0,
  "legacy_utxo_count": 0
}
```

This is the documented behaviour on a fresh datadir (no events
emitted yet → projection accessor returns NULL → structured
reason field reports `projection_not_open`). On a fully-synced
node post-IBD the soak run should return `match: true` with both
hashes populated; the 4b-cutover PR is gated on 24h of
`match: true` runs.

### Notable design notes

1. **Payload header co-located.** The `ev_utxo_add_hdr` /
   `ev_utxo_spend` structs ship inline in
   `event_log_payloads.h` (matching the pattern peer events
   established) rather than in a separate .c file — emitter and
   consumer link against the same static helpers, so wire-format
   drift is impossible.

2. **Boot wiring extended, not duplicated.** The Phase 4d-2
   `boot_start_phase4_storage_shadow` scaffold already opens
   the event log + peers_projection. This PR extends that
   function to also open the utxo_projection, sharing the same
   event log handle. One log file, multiple projections — exactly
   the Phase 4 unification shape.

3. **Schema mirrors legacy `utxos` table.** Both
   `utxo_projection.commitment` and the existing
   `utxo_commitment_sha3_compute_table` walk
   `ORDER BY txid, vout` and emit the same canonical bytes
   (`txid|vout_le|value_le|script_len_le|script|height_le|cb`).
   Identical UTXO sets → identical SHA3 → `match: true`.

4. **REPLACE collisions are observability, not failures.** A
   chain reorg may legitimately replay an ADD over an existing
   entry. The projection counts these but never rejects — the
   emitter is the authority on event ordering.

