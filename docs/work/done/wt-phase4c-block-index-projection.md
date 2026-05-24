# Worker Assignment — Phase 4c: block_index_projection (kill LevelDB)

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 4 (Storage unification)
**Depends on:** Phase 4a (event_log primitive) merged.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4c

**Owns:**
- NEW `lib/storage/include/storage/block_index_projection.h`
- NEW `lib/storage/src/block_index_projection.c`
- NEW `lib/test/src/test_block_index_projection.c`
- EDIT `lib/storage/include/storage/event_log_payloads.h` — add `EV_BLOCK_HEADER` payload struct (NEW for this PR)
- EDIT `lib/storage/src/block_index_db.c` — emit `EV_BLOCK_HEADER` alongside existing LevelDB write (shadow mode)
- EDIT `config/src/boot_services.c` — open the projection on boot, replay from last_consumed_offset
- EDIT `app/controllers/src/diagnostics_controller.c` — register `block_index_projection`
- EDIT `tools/mcp/controllers/ops_controller.c` — add `block_index_projection` to `zcl_state.subsystem` enum
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`

**MUST NOT touch:**
- `lib/storage/src/event_log.c` — Phase 4a primitive; pure consumer here
- LevelDB wrapper (`vendor/lib/libleveldb*`, `lib/storage/src/dbwrapper.c`) — leave alone
  until the 4c-cutover PR (separate, after 24h soak)
- Wave S stage files
- Other storage layers (coins_db, progress_store)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

This is **the LevelDB-killer**. Today `block_index_db.c` (318 LOC)
wraps a LevelDB instance holding the block index — the file-position
mapping (which `blk*.dat` file + offset holds each block). The
LevelDB on-disk footprint is ~513 MB and adds a heavy dependency
(the vendored libleveldb.a is ~3 MB of binary).

After this PR ships + the cutover PR flips authoritative:
- `vendor/lib/libleveldb.a` is no longer linked → **~3 MB binary
  shrink**
- `~/.zclassic-c23/block_index.bin` → **~513 MB disk freed**
- One less crash-safety invariant (LevelDB compaction races with our
  kill-9 ordering go away — see [[feedback_at_tip_kill9_ordering_invariant]])

The cutover gate: 24h with zero divergence on `zcl_block_index_diff`
(an MCP tool added in this PR that compares the projection to the
live LevelDB state).

This PR is **shadow only** — both LevelDB and projection get written.
The cutover PR (separate) disables LevelDB writes and makes the
projection authoritative; then a follow-up PR DELETES the LevelDB
wrapper + vendor binary entirely.

---

## API

```c
/* lib/storage/include/storage/block_index_projection.h */
#ifndef ZCL_STORAGE_BLOCK_INDEX_PROJECTION_H
#define ZCL_STORAGE_BLOCK_INDEX_PROJECTION_H

#include "storage/event_log.h"
#include "storage/block_index_db.h"   /* for struct disk_block_index */
#include <stdbool.h>
#include <stdint.h>

typedef struct block_index_projection block_index_projection_t;

block_index_projection_t *block_index_projection_open(const char *path,
                                                       event_log_t *log);
void block_index_projection_close(block_index_projection_t *p);

/* Consume new events. Idempotent. Returns new last_consumed_offset or
 * (uint64_t)-1 on error. */
uint64_t block_index_projection_catch_up(block_index_projection_t *p);

/* Lookup by hash. Returns true if present. */
bool block_index_projection_get(block_index_projection_t *p,
                                const uint8_t hash[32],
                                struct disk_block_index *out);

/* Lookup by height (chain tip lookups are common). */
bool block_index_projection_get_by_height(block_index_projection_t *p,
                                          int height,
                                          struct disk_block_index *out);

/* Iterate all entries in canonical (height, hash) order. */
typedef bool (*block_index_projection_cb)(const uint8_t hash[32],
                                          const struct disk_block_index *idx,
                                          void *user);
int block_index_projection_iterate(block_index_projection_t *p,
                                   block_index_projection_cb cb,
                                   void *user);

uint64_t block_index_projection_count(block_index_projection_t *p);

/* SHA3-256 over (hash|height|nStatus|nFile|nDataPos|nUndoPos|nTime|nBits)
 * for every entry in canonical (height, hash) order. The diff tool uses
 * this to compare against the live LevelDB. */
int block_index_projection_commitment(block_index_projection_t *p,
                                      uint8_t out[32]);

/* Diagnostics — see CLAUDE.md "Adding state introspection". */
struct json_value;
bool block_index_projection_dump_state_json(struct json_value *out,
                                            const char *key);

#endif
```

---

## EV_BLOCK_HEADER payload (canonical wire format — never change)

Add to `lib/storage/include/storage/event_log_payloads.h`:

```c
struct ev_block_header {
    uint8_t  hash[32];
    uint8_t  hashPrev[32];
    int32_t  height;
    uint32_t nStatus;
    int32_t  nFile;
    uint32_t nDataPos;
    uint32_t nUndoPos;
    uint32_t nTime;
    uint32_t nBits;
    uint8_t  nNonce[32];
    uint8_t  hashMerkleRoot[32];
    uint8_t  hashFinalSaplingRoot[32];
    int32_t  nVersion;
    uint32_t nTx;
    uint16_t nSolutionSize;
    uint8_t  reserved[2];
    /* nSolution bytes follow (nSolutionSize bytes) */
};
```

Plus serialize/parse helpers; emit/replace policy follows the same
"last writer wins per hash" pattern as utxo_projection (a reorg is
just a SPEND/ADD sequence; here it's just a new write for the
new tip's index entry).

---

## Tasks (in order)

### Task 1: Add `EV_BLOCK_HEADER` payload + helpers

Edit `lib/storage/include/storage/event_log_payloads.h`. Add the
`struct ev_block_header` above + `ev_block_header_serialize(...)` /
`ev_block_header_parse(...)` helpers.

**Acceptance:** round-trip test (serialize → parse → compare) for a
known fixture.

### Task 2: Projection skeleton (.h + stub .c)

Stub all functions. `open()` creates a SQLite db:

```sql
CREATE TABLE IF NOT EXISTS block_index (
    hash       BLOB PRIMARY KEY,
    height     INTEGER NOT NULL,
    n_status   INTEGER NOT NULL,
    n_file     INTEGER NOT NULL,
    n_data_pos INTEGER NOT NULL,
    n_undo_pos INTEGER NOT NULL,
    n_time     INTEGER NOT NULL,
    n_bits     INTEGER NOT NULL,
    n_version  INTEGER NOT NULL,
    n_tx       INTEGER NOT NULL,
    blob       BLOB NOT NULL    -- full struct disk_block_index bytes
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS block_index_height_idx ON block_index(height);

CREATE TABLE IF NOT EXISTS projection_meta (
    k TEXT PRIMARY KEY,
    v TEXT NOT NULL
);
```

Bump schema migration version. AR_* macros for every write.

**Acceptance:** opens cleanly; `count()` == 0 on fresh; close/reopen
preserves state.

### Task 3: catch_up implementation

Iterate events via `event_log_stream(start=last_consumed_offset, ...)`.
For each `EV_BLOCK_HEADER`, INSERT OR REPLACE into the table.
Update `last_consumed_offset` after each batch (1000 events).

**Acceptance:** synthetic event log with 1000 headers → projection
has 1000 entries, `count()` matches.

### Task 4: get + get_by_height + iterate + commitment

Implement read paths. `commitment` iterates in `ORDER BY height, hash`,
SHA3-256 over canonical serialization (same as event payload).

**Acceptance:** insert known fixtures, verify commitment matches
hand-computed reference.

### Task 5: Wire shadow emission in block_index_db.c

In `lib/storage/src/block_index_db.c`, find every place that writes
to LevelDB (probably 2-3 functions). After each LevelDB write, ALSO
emit the corresponding `EV_BLOCK_HEADER` via `event_log_append`.

Counter: `g_block_index_event_emit_total`,
`g_block_index_event_emit_fail_total`. If emit fails, log warning
with `obs-ok:` marker, continue (shadow mode).

**Acceptance:** existing `block_index_db` tests still pass. Counter
increments when a header is accepted.

### Task 6: Boot wiring

In `config/src/boot_services.c`, after the event log is opened
(Phase 4a), open `block_index_projection` and call `catch_up()` once.

**Acceptance:** node boots clean. `zcl_state subsystem=block_index_projection`
returns last_consumed_offset + count.

### Task 7: Diagnostics + MCP

- `block_index_projection_dump_state_json` returns:
  ```json
  {
    "open": true,
    "last_consumed_offset": 12345678,
    "entry_count": 3120000,
    "events_consumed_total": 3120000,
    "replace_collisions_total": 0,
    "last_catch_up_ms": 8
  }
  ```
- Register in `g_dumpers`.
- Add `block_index_projection` to `zcl_state.subsystem` enum_csv.

**Acceptance:** `zcl_state(subsystem="block_index_projection")` returns
the JSON.

### Task 8: Diff MCP tool

Add MCP tool `zcl_block_index_diff` returning:
```json
{
  "projection_commitment": "<hex>",
  "leveldb_commitment": "<hex>",
  "match": true,
  "projection_count": 3120000,
  "leveldb_count": 3120000,
  "first_diff": null
}
```

For `first_diff` (only when `match=false`): the first (height, hash)
where the two diverge. Implementation: walk both in (height, hash)
order via iterators, compare bytewise, return first mismatch.

**Acceptance:** on a freshly built node post-IBD, both commitments
match.

### Task 9: test_block_index_projection.c

Test cases:
1. **`open_close_clean`** — open empty, close, reopen, offset=0.
2. **`single_header_consumed`** — emit 1 header event, catch_up,
   `get()` returns it.
3. **`get_by_height`** — insert 3 entries at heights 100/200/300,
   retrieval by height works.
4. **`iterate_canonical`** — entries returned in (height, hash) order.
5. **`replay_idempotent`** — second catch_up is a no-op.
6. **`reorg_replace`** — INSERT then INSERT (same hash, different
   nStatus); final state reflects second write.
7. **`commitment_canonical`** — 3 entries in scrambled insertion
   order; commitment matches a 4th projection that inserted in
   different order.
8. **`resume_from_partial`** — emit 1000 events; manually rewind
   last_consumed_offset to mid-stream; reopen; catch_up consumes
   only the suffix.

**Acceptance:** `./test_parallel --jobs=$(nproc)` all green.

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

After this PR ships, orchestrator polls `zcl_block_index_diff` every
hour for 24h. **Zero mismatches** is the gate for the 4c-cutover PR
(which disables the LevelDB write and makes the projection
authoritative). After cutover + 1 more week of soak, a separate
deletion PR removes `lib/storage/src/block_index_db.c`,
`lib/storage/src/dbwrapper.c`, and the libleveldb vendor binary.

---

## Commit cadence

One commit per task. Push after tasks 4, 7, 9.

---

## Status

**QUEUED** — gated on Phase 4a (event_log primitive) merged. After 4a
ships, this becomes READY.

<!-- Worker: append a Completion section below when done. -->
