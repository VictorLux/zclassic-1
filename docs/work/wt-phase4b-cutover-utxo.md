# Worker Assignment — Phase 4b CUTOVER: retire legacy update_coins SQLite write

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 4 (Storage unification — cutover)
**Depends on:** Phase 4b (utxo_projection) merged ✅ + `zcl_utxo_projection_diff`
returning `match: true` for **24 hours continuous** on a live node.
**Status: QUEUED** until soak completes.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4b cutover

**Owns:**
- EDIT `app/services/src/update_coins.c` — gate the SQLite write behind a
  flag that defaults OFF; projection becomes the authoritative source
- EDIT `config/src/boot_services.c` — drop the legacy coins.db open call
  from non-replay boot paths
- EDIT (if applicable) `app/controllers/src/wallet_controller.c` or
  whichever read path queries `utxo` directly — route through the
  projection instead

**MUST NOT touch:**
- `lib/storage/src/utxo_projection.{c,h}` (4b owns it)
- `event_log` primitive (4a owns it)
- Wave S, Phase 3, Phase 5, Phase 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 4b shipped the projection in **shadow mode**: every UTXO add/spend
is double-written (legacy SQLite + event log + projection). This PR
flips the switch: the projection becomes authoritative, and the legacy
write is gated behind `-utxo-legacy-write=1` (default OFF).

After 24h of soak post-cutover with no incidents, a follow-up PR
deletes the legacy write path entirely and reclaims the legacy `utxo`
table from `node.db`. That's ~500MB of disk + simpler invariants.

**Why not delete in one shot?** The flag-gated cutover lets us
instantly revert if the projection has a bug we missed during shadow
mode. Restart with `-utxo-legacy-write=1` and you're back to the
pre-cutover behavior, no data loss. After the soak proves the
projection is rock-solid, the flag goes away.

---

## Soak gate (must pass before this PR is READY)

```bash
# Run on a live, fully-synced node for 24h. Repeat every hour:
zcl_utxo_projection_diff
# All 24 responses must return: {"match": true, "legacy_sha3": "...",
#                                "projection_sha3": "...same..."}
# Any mismatch within the window → investigate before cutover.
```

If `match: false` ever fires:
1. Capture the response (legacy + projection hashes + heights).
2. Run `zcl_state subsystem=utxo_projection` to inspect counters.
3. Run `zcl_state subsystem=event_log` to confirm no torn writes.
4. **Do NOT proceed with cutover.** Open an issue, investigate, ship
   a fix to 4b first, restart the soak.

---

## Tasks (in order)

### Task 1: Add the `-utxo-legacy-write` flag

EDIT `app/services/src/update_coins.c`. Wrap the SQLite write block
in a runtime check:

```c
if (!g_utxo_legacy_write_enabled) {
    /* projection is now authoritative; skip the legacy write */
    goto skip_legacy_write;
}
/* ... existing AR_CACHED_SAVE for utxo table ... */
skip_legacy_write:
```

EDIT `config/src/cli_args.c` (or wherever flags are parsed): add
`-utxo-legacy-write=0|1` with default `0`.

EDIT `config/src/boot_services.c`: read the flag, set
`g_utxo_legacy_write_enabled` before the chain advances.

**Acceptance:** `make` clean. Existing test_utxo + test_validation
PASS with the default (legacy write off + projection authoritative).
With `-utxo-legacy-write=1`, double-writes occur and
`zcl_utxo_projection_diff` continues to return `match: true`.

### Task 2: Route reads through projection

Identify direct readers of the `utxo` table (probably
`wallet_controller.c`, `getutxocommitment` RPC handler,
`utxo_audit` service). For each, swap the SQLite query for the
projection's read API (added in 4b Task 4).

The shadow-mode reads were unchanged; this is the first PR that
actually swaps them.

**Acceptance:** existing tests PASS. `getutxocommitment` returns
the same SHA3 as before. `walletaudit` produces the same diff.

### Task 3: Drop legacy `coins.db` open path (still keep file)

EDIT `config/src/boot_services.c`. The `coins_db_open()` call can
be skipped on normal boot — the projection is the authoritative
source. Keep the file untouched on disk (rollback path).

Replay / re-index paths still need to open it; gate on `g_replay_mode`.

**Acceptance:** normal boot doesn't open `coins.db`. Cold-import
+ replay paths still do. Boot RSS should decrease slightly.

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

## Post-cutover: live verification

After this PR ships, run the live node normally for 24 hours. Check:
- `zcl_status` → height advancing as before
- `zcl_kpi` → no regression in any metric
- `zcl_utxo_projection_diff` → `match: true` continuously (projection
  vs legacy where legacy is now NOT being written but is still
  queryable from disk for diff purposes — actually, after cutover
  the diff tool needs to compare against the on-disk legacy snapshot,
  which is frozen at cutover time. If it diverges, that means new
  blocks are causing the projection to drift, which would be a
  P0 bug.)

Actually for simplicity: after cutover, change the diff tool to a
"projection self-consistency" check (compare projection's commitment
to a known fixture, or compare against zclassicd's reference). The
real "are we right" test is `zcl_dataintegrity` + bg validation.

---

## After 24h post-cutover soak: 4b-final-delete PR

A separate one-line PR:
- Delete the `-utxo-legacy-write` flag entirely.
- Delete the legacy SQLite write block in `update_coins.c` (no longer
  reachable).
- Delete `coins_db_open()` + the `utxo` SQLite schema.
- Drop the on-disk `coins.db` from data directory layout docs.

This is the actual "remove ~500MB" step.

---

## What this does NOT do

- Does NOT delete any code yet (Task 1 gates with a flag). The
  follow-up PR deletes after soak.
- Does NOT touch the event log primitive.
- Does NOT change consensus rules.
- Does NOT affect cold-import or replay paths (gated separately).

---

## Risk + rollback

If the cutover surfaces a projection bug: stop the node, restart with
`-utxo-legacy-write=1`, and the legacy path takes over until a fix
ships. The on-disk `coins.db` from before cutover is still readable
(we didn't touch it). Worst-case data loss window: the blocks between
cutover and rollback restart (typically minutes; on rollback the
node re-validates and re-applies them via the legacy path).

The follow-up final-delete PR is irreversible without restoring from
backup — that's why we wait 24h.

---

## Commit cadence

One commit per task. Push after task 3.

---

## Status

**QUEUED** — gated on 4b Task 10 ship + 24h `match: true` soak.
Any worker may claim when soak completes by marking IN PROGRESS.

<!-- Worker: append a Completion section below when done. -->
