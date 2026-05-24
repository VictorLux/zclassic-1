# Worker Assignment — Phase 4d-5: small-batch projections (contacts + onion_announcements + hodl_history)

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 4 (Storage unification)
**Depends on:** Phase 4a (event_log primitive) merged.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4d
**Batch spec:** [`docs/work/wt-phase4d-projections-batch.md`](./wt-phase4d-projections-batch.md) § 4d-5

> ## Candidate selection rationale
>
> The batch spec calls for several small, low-LOC subsystems collapsed
> into one PR. After grepping `app/models/src/database.c` for all
> `CREATE TABLE` statements, the three smallest schemas with clear
> single-owner write paths and no consensus involvement are:
>
> 1. **contacts** — `(address TEXT PRIMARY KEY, name TEXT,
>    last_used INTEGER)` — wallet address book, user-edited
> 2. **onion_announcements** — `(onion_address TEXT PRIMARY KEY,
>    announced_at INTEGER, script_hex TEXT)` — onion directory
>    advertisements observed on-chain
> 3. **hodl_history** — `(height INTEGER PRIMARY KEY, time INTEGER,
>    total_zat INTEGER, older_1y_zat INTEGER, older_1y_pct REAL)` —
>    HODL wave snapshots for the explorer chart
>
> The other small candidates from the batch spec (`zmsg`, `zslp`,
> `zswp`, `store`) all have multi-table schemas or cross-cutting
> service code; they are intentionally deferred to a follow-up
> small-batch PR. Each of the three picked here is ~50-80 LOC of
> projection code.

**Owns:**
- NEW `lib/storage/include/storage/small_projections.h` (single header
  exporting the three projection APIs side-by-side)
- NEW `lib/storage/src/contacts_projection.c`
- NEW `lib/storage/src/onion_announcements_projection.c`
- NEW `lib/storage/src/hodl_history_projection.c`
- NEW `lib/test/src/test_small_projections.c` (combined test file
  covering all three)
- EDIT `lib/storage/include/storage/event_log_payloads.h` — add 5
  payload structs (`ev_contact_set`, `ev_contact_delete`,
  `ev_onion_announcement`, `ev_hodl_snapshot`, plus
  `ev_contact_touched` for `last_used` bumps) + serialize/parse helpers
- EDIT `lib/storage/include/storage/event_log.h` — allocate 5 new
  ids in `enum event_log_type` (use the next 5 free slots after the
  ZNAM event ids)
- EDIT the model files that own each direct write today:
  - `app/models/src/contact.c` — shadow-emit on save/touch/delete
  - `app/models/src/onion_announcement.c` — shadow-emit on insert
  - `app/models/src/hodl_wave.c` — shadow-emit on snapshot persist
- EDIT `config/src/boot_services.c` — open all three projections in
  the same boot block as the other 4d projections; close in the same
  shutdown ordering
- EDIT `app/controllers/src/diagnostics_controller.c` — register
  three dump functions in `g_dumpers`
- EDIT `tools/mcp/controllers/ops_controller.c` — add
  `contacts_projection`, `onion_announcements_projection`,
  `hodl_history_projection` to the `zcl_state.subsystem` enum_csv
- EDIT projection-diff MCP controller — add three diff tools
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
  `lib/test/include/test/test_helpers.h`
- EDIT `lib/test/src/test_mcp_controllers.c` — bump `EXPECTED_TOTAL`
  by 3 and the relevant per-domain counters; add 3 enum entries

**MUST NOT touch:**
- `lib/storage/src/event_log.c` — Phase 4a primitive; pure consumer
- Existing projections: `utxo_projection`, `block_index_projection`,
  `peers_projection`, `znam_projection`, `mempool_projection`,
  `wallet_projection`
- The `zmsg`, `zslp`, `zswp`, `store`, `products`, `orders`,
  `file_offers` tables — those are deferred to a follow-up
  small-batch PR
- Wave S stage files
- Other storage layers (`coins_db`, `progress_store`,
  `schema_migration`)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`
- Wallet keys, HD seed, or any private material

---

## Why this matters

These three subsystems are the long tail of derived state — small
enough that a separate PR per table is overkill, but each is still on
the path to the dream end-state where the event log is the single
source of truth and every derived table is rebuildable. Shipping all
three behind one shadow PR keeps the orchestrator's tracking simple
(one diff tool per subsystem; one 24h soak window covers all three).

After this PR ships + the (separate) cutover PR ratifies authority:
- `contacts` table direct writes → removable
- `onion_announcements` table direct writes → removable
- `hodl_history` table direct writes → removable
- All three become projection-driven, rebuildable from the event log

This PR is **shadow only** — both SQLite tables and projections get
written. The cutover is a separate small PR after 24h of zero
divergence on all three diff tools.

---

## API

```c
/* lib/storage/include/storage/small_projections.h */
#ifndef ZCL_STORAGE_SMALL_PROJECTIONS_H
#define ZCL_STORAGE_SMALL_PROJECTIONS_H

#include "storage/event_log.h"
#include <stdbool.h>
#include <stdint.h>

/* ── contacts ─────────────────────────────────────────────────────── */
typedef struct contacts_projection contacts_projection_t;
contacts_projection_t *contacts_projection_open(const char *path,
                                                event_log_t *log);
void contacts_projection_close(contacts_projection_t *p);
uint64_t contacts_projection_catch_up(contacts_projection_t *p);
uint64_t contacts_projection_count(contacts_projection_t *p);
void contacts_projection_set_event_log(event_log_t *log);
bool contacts_projection_emit_set(const char *address, const char *name);
bool contacts_projection_emit_touched(const char *address,
                                      uint32_t last_used);
bool contacts_projection_emit_delete(const char *address);
struct json_value;
bool contacts_projection_dump_state_json(struct json_value *out,
                                         const char *key);
contacts_projection_t *contacts_projection_current(void);

/* ── onion_announcements ──────────────────────────────────────────── */
typedef struct onion_ann_projection onion_ann_projection_t;
onion_ann_projection_t *onion_ann_projection_open(const char *path,
                                                  event_log_t *log);
void onion_ann_projection_close(onion_ann_projection_t *p);
uint64_t onion_ann_projection_catch_up(onion_ann_projection_t *p);
uint64_t onion_ann_projection_count(onion_ann_projection_t *p);
void onion_ann_projection_set_event_log(event_log_t *log);
bool onion_ann_projection_emit(const char *onion_address,
                               uint32_t announced_at,
                               const char *script_hex);
bool onion_ann_projection_dump_state_json(struct json_value *out,
                                          const char *key);
onion_ann_projection_t *onion_ann_projection_current(void);

/* ── hodl_history ─────────────────────────────────────────────────── */
typedef struct hodl_history_projection hodl_history_projection_t;
hodl_history_projection_t *hodl_history_projection_open(
    const char *path, event_log_t *log);
void hodl_history_projection_close(hodl_history_projection_t *p);
uint64_t hodl_history_projection_catch_up(hodl_history_projection_t *p);
uint64_t hodl_history_projection_count(hodl_history_projection_t *p);
void hodl_history_projection_set_event_log(event_log_t *log);
bool hodl_history_projection_emit_snapshot(int32_t height,
                                           uint32_t time_unix,
                                           int64_t total_zat,
                                           int64_t older_1y_zat,
                                           double older_1y_pct);
bool hodl_history_projection_dump_state_json(struct json_value *out,
                                             const char *key);
hodl_history_projection_t *hodl_history_projection_current(void);

#endif
```

---

## Event payloads

Add to `event_log_payloads.h`:

```c
struct ev_contact_set {
    uint8_t address_len;     /* base58, ≤ 64 */
    uint8_t name_len;        /* user label, ≤ 64 */
    uint8_t reserved[6];
    /* address[address_len], name[name_len] follow */
};

struct ev_contact_touched {
    uint8_t  address_len;
    uint8_t  reserved[3];
    uint32_t last_used_unix;
    /* address[address_len] follows */
};

struct ev_contact_delete {
    uint8_t address_len;
    uint8_t reserved[7];
    /* address[address_len] follows */
};

struct ev_onion_announcement {
    uint32_t announced_at_unix;
    uint8_t  onion_addr_len;     /* typically 62 (.onion v3) */
    uint8_t  script_hex_len;     /* hex of OP_RETURN script */
    uint8_t  reserved[2];
    /* onion_address[onion_addr_len], script_hex[script_hex_len] follow */
};

struct ev_hodl_snapshot {
    int32_t  height;
    uint32_t time_unix;
    int64_t  total_zat;
    int64_t  older_1y_zat;
    double   older_1y_pct;       /* 0.0 .. 100.0 */
};
```

Allocate 5 new ids in `enum event_log_type` (the next free slots after
the ZNAM events). Add serialize/parse helpers matching the existing
`ev_peer_observed_*` / `ev_znam_*` style.

---

## Tasks (in order)

### Task 1: Add the 5 event payloads + helpers

Edit `event_log.h` (new enum slots) + `event_log_payloads.h` (structs
+ serialize/parse). Pure addition.

**Acceptance:** round-trip test (serialize → parse → compare) for all
5 payload types from known fixtures.

### Task 2: Three projection skeletons + SQLite schemas

One `.c` per subsystem. Each mirrors `znam_projection.c` boilerplate
exactly (`apply_pragmas`, `ensure_schema`, `meta_*`, open/close,
WAL + 5s busy_timeout, schema_version = 1).

Schemas:

```sql
-- contacts_projection.db
CREATE TABLE IF NOT EXISTS contacts (
    address    TEXT PRIMARY KEY,
    name       TEXT NOT NULL,
    last_used  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS projection_meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);

-- onion_ann_projection.db
CREATE TABLE IF NOT EXISTS onion_announcements (
    onion_address TEXT PRIMARY KEY,
    announced_at  INTEGER NOT NULL,
    script_hex    TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_onion_announced_at
    ON onion_announcements(announced_at DESC);
CREATE TABLE IF NOT EXISTS projection_meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);

-- hodl_history_projection.db
CREATE TABLE IF NOT EXISTS hodl_history (
    height        INTEGER PRIMARY KEY,
    time          INTEGER NOT NULL,
    total_zat     INTEGER NOT NULL,
    older_1y_zat  INTEGER NOT NULL,
    older_1y_pct  REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_hodl_history_time ON hodl_history(time);
CREATE TABLE IF NOT EXISTS projection_meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);
```

**Acceptance:** all three open cleanly; `count()` == 0 on fresh;
close/reopen preserves `last_consumed_offset`.

### Task 3: catch_up for all three

Each projection iterates via `event_log_stream`, dispatching only on
its own event types (the other types are skipped). Per type:
- contacts: `EV_CONTACT_SET` / `EV_CONTACT_TOUCHED` / `EV_CONTACT_DELETE`
- onion_ann: `EV_ONION_ANNOUNCEMENT`
- hodl_history: `EV_HODL_SNAPSHOT`

`INSERT OR REPLACE` semantics throughout. Commit every 100 events
inside a `BEGIN IMMEDIATE` block; mirror `znam_projection_catch_up`.

**Acceptance:** synthetic event log with mixed events → each
projection consumes only its own types; counts match.

### Task 4: Reader API + diagnostics dumps

Implement `_count()` for each via `SELECT COUNT(*)`. Each
`_dump_state_json` returns `open` + `path` + `last_consumed_offset` +
`<table>_count` + `events_consumed_total` + per-type emit counters +
`emit_fail_total` + `last_catch_up_ms`. Mirror
`znam_projection_dump_state_json`.

Register all three dump functions in `g_dumpers`.

**Acceptance:** `zcl_state subsystem=contacts_projection`,
`zcl_state subsystem=onion_announcements_projection`,
`zcl_state subsystem=hodl_history_projection` each return JSON with
`open: true`.

### Task 5: Shadow-emit at write sites

Each call site emits AFTER the legacy SQLite write succeeds; emit
failures log via `obs-ok:` and continue.

- `app/models/src/contact.c`:
  - On contact save → `contacts_projection_emit_set(addr, name)`
  - On `last_used` bump → `contacts_projection_emit_touched(addr, now)`
  - On delete → `contacts_projection_emit_delete(addr)`
- `app/models/src/onion_announcement.c`:
  - On insert → `onion_ann_projection_emit(addr, announced_at,
    script_hex)`
- `app/models/src/hodl_wave.c`:
  - On `hodl_history` snapshot persist → `hodl_history_projection_emit_snapshot(...)`

Each projection module owns atomic counters
(`g_emit_<type>_total`, `g_emit_fail_total`).

**Acceptance:** existing tests for `contact`, `onion_announcement`,
`hodl_wave` still pass. Emit counters tick when the respective write
runs.

### Task 6: Boot wiring

In `config/src/boot_services.c`, alongside the other 4d projections,
open all three:

```c
/* small batch — three projections at once */
contacts_projection_open("<datadir>/contacts_projection.db", log);
onion_ann_projection_open("<datadir>/onion_ann_projection.db", log);
hodl_history_projection_open("<datadir>/hodl_history_projection.db", log);
/* catch_up each, then set_event_log each */
```

Close in shutdown ordering after the legacy models are quiesced.

**Acceptance:** node boots clean. All three `zcl_state` subsystems
return `open: true`.

### Task 7: Three `zcl_<name>_projection_diff` MCP tools

Add three tools that compare projection counts against live SQLite
counts:

- `zcl_contacts_projection_diff` →
  `{ projection_count, live_count, first_diff, match }`
- `zcl_onion_announcements_projection_diff` →
  `{ projection_count, live_count, first_diff, match }`
- `zcl_hodl_history_projection_diff` →
  `{ projection_count, live_count, first_diff, match }`

For the `first_diff` field: walk both in canonical order
(PRIMARY KEY ASC), return the first key where the two diverge as a
string. `null` when `match=true`.

Wire RPC handler + MCP tool registration mirroring
`znamprojectiondiff`.

**Acceptance:** on a freshly built node, all three return
`match: true` with whatever the live counts happen to be (usually 0
on a clean start).

### Task 8: test_small_projections.c

One combined test file covering all three projections. Cases (8 per
subsystem = 24 total):

**contacts:**
1. `contacts_open_close_clean`
2. `contacts_set_consumed`
3. `contacts_touched_updates_last_used`
4. `contacts_delete_removes_row`
5. `contacts_replay_idempotent`
6. `contacts_emit_set_global`

**onion_announcements:**
1. `onion_open_close_clean`
2. `onion_announcement_consumed`
3. `onion_replace_on_collision` (same onion_address, newer
   `announced_at`)
4. `onion_replay_idempotent`
5. `onion_emit_set_global`

**hodl_history:**
1. `hodl_open_close_clean`
2. `hodl_snapshot_consumed`
3. `hodl_replace_on_collision` (same height, newer values)
4. `hodl_replay_idempotent`
5. `hodl_emit_set_global`

Plus 8 shared cases for `resume_from_partial` semantics — emit 100
mixed events, rewind, verify suffix-only consumption per projection.

Wire into `lib/test/src/test.c` (with a single `ZCL_TEST_ONLY=small_projections`
entry), `lib/test/src/test_parallel.c`,
`lib/test/include/test/test_helpers.h`.

### Task 9: Bump `test_mcp_controllers` expected counts

Update `lib/test/src/test_mcp_controllers.c`:
- Bump `EXPECTED_TOTAL` by 3.
- Bump the matching per-domain counters where the three new
  `*_projection_diff` tools register.
- If the test asserts the `zcl_state.subsystem` enum_csv list, add
  all three new entries.

**Acceptance:** `ZCL_TEST_ONLY=small_projections ./test_zcl` → 0
failures. `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` → 0 failures.

### Task 10: Final verify + push

```bash
make -j$(nproc)
make lint
ZCL_TEST_ONLY=small_projections ./test_zcl
ZCL_TEST_ONLY=mcp_controllers ./test_zcl
ZCL_TEST_ONLY=mcp_e2e ./test_zcl
./test_parallel --jobs=$(nproc)
git push origin main
```

Append a `Completion` section to **this file** with commit hashes +
verification output (mirror the 4d-2 / 4d-4 completion blocks in
`wt-phase4d-projections-batch.md`). Confirm all three diff tools
return `match: true` on the test instance.

---

## Live verification block

After push + node restart on the test instance, the gate is:

```text
zcl_contacts_projection_diff             → { "match": true, "first_diff": null }
zcl_onion_announcements_projection_diff  → { "match": true, "first_diff": null }
zcl_hodl_history_projection_diff         → { "match": true, "first_diff": null }
```

Orchestrator polls all three hourly for 24h. **Zero mismatches across
all three** is the gate for the 4d-5 cutover PR (separate, deletes
the three direct SQLite writes).

---

## Commit cadence

One commit per task (10 commits). Push after Task 4, Task 7, Task 9.

---

## Status

**IN PROGRESS (wt2)** — claimed 2026-05-24.

Progress, 2026-05-24:
- Task 1 event payload ids plus serialize/parse helpers landed in
  `0f10cd5f4` (`add small projection event payloads`).
- Task 2 projection skeletons and SQLite schemas landed in
  `a177e119d` (`add small projection sqlite skeletons`). Follow-up
  schema tightening adds the required `idx_onion_announced_at` and
  `idx_hodl_history_time` indexes with focused test coverage.
- Task 3 mixed event-log catch-up replay landed in `a4ac396b3`
  (`replay small projection events`). Rebuilt `test_zcl` and
  `ZCL_TEST_ONLY=small_projections ./test_zcl` pass with the index
  checks included.

Next slice: Task 4 reader diagnostics dumps and `zcl_state`
registration for `contacts_projection`, `onion_announcements_projection`,
and `hodl_history_projection`.

<!-- Worker: append a Completion section below when done. -->
