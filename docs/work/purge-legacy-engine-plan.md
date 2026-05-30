# Purge the legacy engine — execution plan

One engine survives: `event_log` → 8 pure projections → the Wave-S Job reducer
(`app/jobs/src/*_stage.c`) as the sole writer; tip = `tip_finalize`'s
`progress.kv` cursor. Everything that exists to run a second (legacy `coins.db`)
engine or to compare the two is deleted. Full machine-readable manifest with
per-file references: [`purge-legacy-engine-manifest.json`](./purge-legacy-engine-manifest.json).

Branch: `purge/legacy-engine`. Owner authorized a non-building intermediate; the
END state must be coherent single-engine code.

## Stages (dependency-ordered)

| # | Stage | Kind | Status |
|---|---|---|---|
| 1 | Delete apparatus/legacy **tests** + registrations (19 files) | delete | partial (10 done, `2a11563b7`) |
| 2 | Rewire `-shadow` ingest hook out of `msg_blocks.c` hot path | strip | |
| 3 | Strip `-shadow`/conservation/cutover boot + supervisor + condition wiring | strip | |
| 4 | Delete MCP/RPC apparatus surfaces (chain/ops/diagnostics, metrics gauge) | strip | |
| 5 | Delete cutover controllers/conditions/services, diff+replay use-cases, mutator/, block_log legacy adapters, apparatus jobs, `coins_view_sqlite.c` | delete | |
| 6 | Collapse SHADOW/AUTHORITATIVE stage duality — reducer writes unconditionally | strip | |
| 7 | Strip the `utxo_author_t` flag — projection read + stage emit unconditional | strip | |
| 8 | **NEW CODE:** reducer owns `block_index` + `nStatus` population (cold-start + advance) | write | |
| 9 | **NEW CODE + strip:** retire legacy `coins.db` durable writes; repoint consensus reads to projection; delete `coins_view_sqlite.c` | write | |
| 10 | **NEW CODE:** read-serving (`gettxoutsetinfo`, balance/listunspent, getblock) off the projection | write | |
| 11 | Delete legacy importers (`sync_controller_import.c`, `legacy_import.c`, auto-import boot blocks); cold-start via `boot_rebuild_from_log()` / fast_sync | delete+write | |
| 12 | Shrink lint baselines (E1/E2/E6) + final coherence pass | strip | |

## The three new-code blockers (stages 8–10)

1. **block_index + nStatus** — after the legacy connect path is gone, nothing
   populates `ms->map_block_index` or `BLOCK_HAVE_DATA`/`BLOCK_VALID_SCRIPTS`
   that every stage reads. Either `header_admit_stage` inserts the index +
   `body_persist_stage` sets `HAVE_DATA`, or rewire stage reads to
   `block_index_projection_get_by_height` (exists, zero production readers).
2. **`boot_rebuild_from_log()`** in `config/src/boot.c` — open event_log → replay
   `block_index_projection` → `utxo_projection.catch_up` → seed `tip_finalize`
   cursor; replaces `load_block_index_sqlite` + the legacy importer. Cold UTXO
   comes from fast_sync (SHA3 snapshot → log seed → fold), not external LevelDB.
3. **read-serving off the projection** — `gettxoutsetinfo` + wallet UTXO scans
   read `utxo_projection` (via `coins_view_projection.c`, byte-compatible) once
   `coins.db` is gone.

## Keepers (do NOT delete)

`legacy_mirror_sync_service.c` (monitor-only post-flip, default-off), the 8
projections + `block_log_port.h`/`utxo_snapshot_port.h` (only the legacy
adapters go), `test_stage_reorg_unwind_parity.c` (the surviving-engine reorg
proof), `active_chain_set_tip` (single-engine RAM tip).
