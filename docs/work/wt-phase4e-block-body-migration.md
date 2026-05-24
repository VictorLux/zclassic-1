# Worker Assignment — Phase 4e: Migrate block bodies into event log + DELETE legacy blocks/blk*.dat

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 4 (Storage unification — final PR)
**Depends on:** Phase 4a (event_log) merged ✅, Phase 4c (block_index_projection)
merged + cutover complete (LevelDB block_index gone), 24h soak with zero
divergence on `zcl_block_index_diff`.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4e

**Owns:**
- EDIT `lib/storage/include/storage/event_log_payloads.h` — add `ev_block_body` payload
- NEW `tools/migrate/block_bodies_to_event_log.c` — one-shot migration tool
- EDIT `app/services/src/body_persist_stage.c` — emit `EV_BLOCK_BODY` instead of writing `blk*.dat`
- EDIT `lib/validation/src/accept_block.c` (or wherever the legacy direct write happens) — same
- EDIT `lib/storage/include/storage/disk_block_io.h` — replace `disk_block_io_write_block` with `event_log_append`-backed version OR delete entirely
- EDIT `config/src/boot_services.c` — boot-time check: if `blocks/` exists AND migration not yet recorded, halt + emit `EV_MIGRATION_REQUIRED` event with operator instructions
- DELETE `lib/storage/src/blocks_mmap_reader.c`
- DELETE `lib/storage/src/blocks_index_legacy_reader.c`
- DELETE `lib/storage/src/disk_block_io.c`
- DELETE `lib/storage/include/storage/blocks_index_legacy_reader.h`

**MUST NOT touch:**
- `lib/storage/src/event_log.c` (Phase 4a primitive; pure consumer here)
- `lib/storage/src/block_index_db.c` (deleted by Phase 4c-cutover already)
- Wave S stage files (header_admit, validate_headers, etc.)
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

This is **the last Phase 4 PR**. After it ships:
- `~/.zclassic-c23/blocks/blk*.dat` (6.3 GB on a synced node) → DELETED
- `~/.zclassic-c23/block_index.bin` (LevelDB, 513 MB) → already gone after 4c-cutover
- ALL block bodies live in the event log alongside headers, txs, UTXO deltas, peer observations, etc.
- **One file format, one recovery path, one source of truth.**

The total Phase 4 disk savings: ~7 GB on disk + ~3 MB binary shrink
(LevelDB gone) + N fewer crash-safety invariants. The
`at-tip kill-9 ordering invariant` becomes a footnote — there's only
one writer, the event log, and its sentinel design is the canonical
torn-write story.

This PR is the heaviest Phase 4 PR because of the migration tool —
existing nodes need a one-time data conversion. New nodes
(`-cold-import` or fast-sync) build the event log fresh, so the
migration only runs on pre-Phase-4 nodes.

---

## EV_BLOCK_BODY payload

Add to `lib/storage/include/storage/event_log_payloads.h`:

```c
struct ev_block_body_hdr {
    uint8_t  hash[32];           /* block hash, matches header */
    int32_t  height;             /* canonical (chain) height */
    uint32_t tx_count;
    uint32_t body_len;           /* serialized body bytes following */
    /* serialized block body bytes follow (body_len) */
};
```

The body bytes are the same wire format as `blocks/blk*.dat` (Bitcoin Core
block serialization). No re-encoding — just a copy.

---

## Tasks (in order)

### Task 1: Add `ev_block_body` payload

Edit `event_log_payloads.h`. Add `struct ev_block_body_hdr` per above +
serialize/parse helpers + a `EV_BLOCK_BODY` enum entry (it's already
declared in `event_log.h` as id 2, this just exposes the payload struct).

**Acceptance:** round-trip test for a synthetic 1KB block body.

### Task 2: Build the migration tool

NEW `tools/migrate/block_bodies_to_event_log.c`:

```bash
$ zclassic23 -migrate-block-bodies-to-event-log \
    -datadir=~/.zclassic-c23 \
    [-dry-run]
```

Behavior:
1. Open the existing `blocks/blk*.dat` files via the legacy
   `blocks_mmap_reader` (read-only).
2. For each block in canonical (height, hash) order, emit an
   `EV_BLOCK_BODY` event into a NEW event log file at
   `~/.zclassic-c23/event_log_v2.dat` (NOT overwriting the existing
   event_log.dat).
3. After all blocks emitted, atomically rename `event_log_v2.dat` →
   `event_log.dat` (replacing the existing file). The existing file
   may have non-body events; this is OK as long as the migration tool
   preserves them in proper canonical (offset) order.

Actually — better approach to avoid losing existing events: **append**
the EV_BLOCK_BODY events to the EXISTING event log. They'll be
out-of-chronological-order but the projection layer doesn't care
(it consumes by event type, not by timestamp). The block_index
projection already exists; it'll see the EV_BLOCK_BODY events and
flag bodies in their entries.

Document a marker event `EV_MIGRATION_COMPLETED_BLOCK_BODIES` at the
end so subsequent boots know not to re-migrate.

**Dry-run mode:** count blocks, sum body bytes, print summary,
don't write anything.

**Acceptance:** dry-run on a test datadir reports the expected
count + bytes. Real run on a fixture datadir emits N events; a
follow-up read confirms all N body bytes are recoverable bit-exact.

### Task 3: Rewire `body_persist_stage.c` and `accept_block.c`

Find all call sites that write to `blocks/blk*.dat` (via
`disk_block_io_write_block` or equivalent). Replace with
`event_log_append(log, EV_BLOCK_BODY, serialized_body, body_len)`.

The body_persist stage already records `body_persist_log[H]` — that
stays as the cursor. The event log holds the body bytes.

**Subtlety:** the existing `disk_block_io_read_block(file, pos, len)`
is called from a few places (mining, RPC `getblock`, validation
re-checks). Either:
- (a) Migrate ALL callers to read via the new `block_body_lookup(hash)`
  helper that consults block_index_projection for the file_pos → reads
  event log at that offset.
- (b) Keep the legacy disk reader as a fallback during the
  shadow-soak period, then delete in a follow-up PR.

**Recommend (b)** — it shrinks this PR to a manageable size and
the cleanup PR is straightforward after a 24h soak.

**Acceptance:** `make test_parallel` passes. New blocks flowing from
P2P get written to the event log (no new `blk*.dat` writes).

### Task 4: Boot-time migration gate

In `config/src/boot_services.c`, after opening the event log:

```c
if (boot_dir_exists(datadir, "blocks") &&
    !event_log_has_migration_marker(log, EV_MIGRATION_COMPLETED_BLOCK_BODIES)) {
    fprintf(stderr,
        "zclassic23: legacy blocks/ directory present but migration "
        "to event log not recorded.\n"
        "Run: zclassic23 -migrate-block-bodies-to-event-log -datadir=%s\n"
        "Then delete the blocks/ directory.\n", datadir);
    exit(EXIT_FAILURE);
}
```

Fresh nodes (no `blocks/` dir) skip this entirely. Migrated nodes pass
the marker check. Un-migrated existing nodes get a clear instruction.

**Acceptance:** synthetic test creates a `blocks/` dir, runs node, gets
the error message + exit 1. After migration tool runs, node boots clean.

### Task 5: Delete the legacy readers

Remove:
- `lib/storage/src/blocks_mmap_reader.c`
- `lib/storage/src/blocks_index_legacy_reader.c`
- `lib/storage/src/disk_block_io.c`
- `lib/storage/include/storage/blocks_index_legacy_reader.h`
- Any matching headers that become orphans
- `lib/storage/include/storage/disk_block_io.h` if no callers remain

Update the boot path to remove their initializers.

**Subtlety:** the migration tool itself NEEDS `blocks_mmap_reader.c` to
read the legacy files. Solution: keep these files alive for ONE more
release (in `tools/migrate/legacy_block_reader.c` — a copy inside the
migrate tool's directory, NOT in the main library tree). The main
library `lib/storage/` no longer compiles them.

**Acceptance:** `make` builds successfully without the legacy readers
in the main library. Migration tool still works (uses its own copy).
`./test_parallel` PASSES — any tests that referenced the legacy readers
get updated to use the event log instead.

### Task 6: Update tests

Several tests probably reference `blocks/blk*.dat`:
- `test_block_log_file.c`
- `test_block_log_legacy.c`
- `test_disk_block_io.c`
- `test_blocks_index_legacy_reader.c` (if exists)

Each needs to either:
- (a) DELETE if it tested the legacy reader (which is now gone)
- (b) UPDATE to read via the event log

Be liberal with deletes — per `feedback_stop_test_churn`, the new
event-log tests cover the same surface.

**Acceptance:** `./test_parallel --jobs=$(nproc)` PASSES.

### Task 7: Run migration on the local dev node + verify

```bash
# (Stop the node first)
systemctl --user stop zclassic23

# Dry-run
zclassic23 -migrate-block-bodies-to-event-log -datadir=$HOME/.zclassic-c23 -dry-run
# Should report: ~3.1M blocks, ~6.3 GB of bodies

# Real run
zclassic23 -migrate-block-bodies-to-event-log -datadir=$HOME/.zclassic-c23
# Should take 5-15 min depending on disk

# Verify
ls -lh $HOME/.zclassic-c23/event_log.dat
# Should grow by ~6.3 GB

# Test a random block read via the event log
zclassic23-cli getblock 3000000 | jq .hash
# Should return the same hash as before migration

# Start the node
systemctl --user start zclassic23
sleep 10
zclassic23-cli getblockcount
# Should be the current tip

# (Now safe to delete the legacy directory)
rm -rf $HOME/.zclassic-c23/blocks
```

If everything works: this assignment is done. If `getblockcount` is
wrong or `getblock` fails after migration: do NOT delete the legacy
directory; revert this PR via `git revert` and investigate.

**Acceptance:** node operates normally post-migration; the legacy
`blocks/` directory is deletable without losing data.

### Task 8: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## Risk + rollback

The migration is destructive in that it APPENDS to the event log.
Worst case (migration tool has a bug): event log gets garbage events;
recovery is to truncate the log back to its pre-migration offset (the
fsync-sentinel design makes this trivial — `event_log_truncate_to(N)`)
and re-run migration.

Worst case for the legacy `blocks/` dir: we never delete it. Migration
fails → operator keeps `blocks/`, runs old binary, all fine.

**The destructive step is `rm -rf blocks/`** — and that happens
manually, AFTER the migration tool reports success AND the node
verifies via `getblock` calls. No automatic deletion.

---

## What this does NOT do

- Does NOT delete the LevelDB block_index — that was Phase 4c-delete-PR.
- Does NOT add any new events besides `EV_BLOCK_BODY` and
  `EV_MIGRATION_COMPLETED_BLOCK_BODIES`.
- Does NOT change consensus rules. Block bodies are
  byte-identical to what `blk*.dat` held.

---

## Commit cadence

One commit per task. Push after tasks 2, 5, 7.

---

## Status

**QUEUED** — gated on Phase 4c-cutover complete + 24h soak. After
that, this is the last Phase 4 PR. Cleanup PR (delete `blocks/` dir
on a fresh install, drop `-migrate-block-bodies-to-event-log` flag)
can follow in a few weeks once everyone's migrated.

<!-- Worker: append a Completion section below when done. -->
