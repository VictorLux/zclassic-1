# Worker Assignment — Phase 4c CUTOVER: retire LevelDB block_index

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 4 (Storage unification — cutover)
**Depends on:** Phase 4c (block_index_projection) merged ✅ + 
`zcl_block_index_diff` returning `match: true` for **24 hours continuous**.
**Status: QUEUED** until 4c soak completes.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4c cutover

**Owns:**
- EDIT `lib/storage/src/block_index_db.c` — gate the LevelDB write behind
  `-block-index-legacy-write` flag (default OFF)
- EDIT call sites of `block_index_db_*` read APIs — route through
  projection's reader
- DELETE `lib/storage/src/block_index_db.c` + `.h` (in the followup
  4c-final-delete PR; this PR only flips the switch)

**MUST NOT touch:**
- `lib/storage/src/block_index_projection.{c,h}` (4c owns it)
- `event_log` primitive (4a owns it)
- Wave S, Phase 3, Phase 5, Phase 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 4c shipped the block_index projection in shadow mode (every
header admit double-writes to LevelDB + event log + SQLite projection).

This PR flips the projection to authoritative. After 24h soak, the
follow-up final-delete PR removes:
- `lib/storage/src/block_index_db.c` (~600 LOC)
- LevelDB vendored library (~3 MB binary shrink)
- `~/.zclassic-c23/block_index.bin` (~513 MB on disk)

The result: LevelDB completely gone from the binary. Single dependency
shed. The "what writes to disk" surface shrinks meaningfully.

---

## Soak gate (must pass before this PR is READY)

```bash
zcl_block_index_diff
# Must return: {"match": true, "legacy_count": N, "projection_count": N,
#               "first_diverging_hash": null}
# Continuously for 24h.
```

If a divergence appears:
- Capture diverging hash + height
- `zcl_state subsystem=block_index_projection` for counters
- Investigate; do NOT proceed with cutover.

---

## Tasks (in order)

### Task 1: `-block-index-legacy-write` flag

Mirror the 4b-cutover pattern. EDIT `lib/storage/src/block_index_db.c`:
gate every LevelDB write behind `g_block_index_legacy_write_enabled`.
EDIT CLI parser + boot wiring.

**Acceptance:** `make` clean. With default (flag off), LevelDB stops
being written. With flag on, double-writes continue.

### Task 2: Route reads through projection

Find every `block_index_db_get_*` / `_iter_*` caller. Probably:
- `lib/validation/src/accept_block_header.c`
- `lib/validation/src/connect_tip.c`
- `tools/mcp/controllers/chain_controller.c`
- `app/services/src/chain_advance_coordinator.c` (legacy, will die in C-9)

Swap each for the projection's reader. The projection's API surface
should be 1:1 with the LevelDB one to make this mechanical.

**Acceptance:** existing tests PASS. `getblockhash`, `getblockheader`,
`getbestblockhash` return identical values.

### Task 3: Drop legacy `block_index.bin` open path (still keep file)

EDIT `config/src/boot_services.c` — skip `block_index_db_open()` on
normal boot. Replay/re-index paths still open it.

**Acceptance:** normal boot doesn't open the LevelDB file. Cold-import
+ rescan paths still do.

### Task 4: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## After 24h soak: 4c-final-delete PR

The big-cleanup PR:
- DELETE `lib/storage/src/block_index_db.c` + `.h`
- DELETE the LevelDB vendor dir (no longer linked)
- DELETE the `-block-index-legacy-write` flag
- DROP the LevelDB dep from the makefile linker line
- DOCUMENT: operators with old datadirs can `rm -rf
  ~/.zclassic-c23/block_index.bin` after upgrade

Expected savings: ~3 MB binary + 513 MB disk + 1 fewer C++ dependency
in a C-only codebase (LevelDB is C++).

---

## What this does NOT do

- Does NOT delete LevelDB yet. The flag is the safety net.
- Does NOT touch block bodies (`blocks/blk*.dat`) — that's Phase 4e.
- Does NOT change consensus rules.
- Does NOT touch cold-import — gated separately.

---

## Risk + rollback

If projection has a bug, restart with `-block-index-legacy-write=1`
and LevelDB takes over again. Worst-case data loss window: minutes
between cutover and rollback.

The on-disk `block_index.bin` is untouched by this PR; it's a backup.
The final-delete PR is irreversible — that's why we wait 24h.

---

## Commit cadence

One commit per task. Push after task 3.

---

## Status

**QUEUED** — gated on 4c Task 10 ship + 24h `match: true` soak.

<!-- Worker: append a Completion section below when done. -->
