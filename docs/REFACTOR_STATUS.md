# Refactor Status — Purpose-Per-File Finish Board

> Updated 2026-06-01. This file is the current debt board for finishing the
> framework refactor. `docs/FRAMEWORK.md` remains the architecture.

## Objective

Finish the ZClassic23 refactor by deleting stale shadow/cutover and scaffold
code, making every remaining file match one clear framework shape and purpose,
shrinking all lint baselines to zero, updating docs to reflect the
authoritative reducer architecture, and proving it with clean tests plus a live
node soak.

## Current Truth

- The reducer/staged pipeline is the authoritative chain-advance architecture.
- The public cutover/projection-diff MCP/RPC apparatus has been removed.
- The old legacy block-connect engine files are gone.
- The production `app/`, `lib/storage/`, `lib/validation/`, and `tools/mcp/`
  C/H surfaces no longer describe active reducer read-model paths as
  shadow/cutover/projection-diff infrastructure; remaining historical wording
  is test/doc context only.
- E1, E2, supervisor, E7, typed-blocker, and controller raw-SQL adoption are at
  zero grandfathered entries; E6 is down to 24 grandfathered write surfaces,
  lib-layering is down to 79 grandfathered includes, raw allocation debt is at
  zero active allowlist entries, and other ratchet baselines still grandfather
  real debt.

## Completed Architecture Moves

- `app/` is organized by framework shape: controllers, services, models, jobs,
  supervisors, conditions, events, views.
- Model lifecycle and validation gates are enforced.
- Wave-S reducer stages live under `app/jobs/` and use the Job advance/block
  contract.
- Conditions are real `{detect, remedy, witness}` files.
- Domain extraction is real: 21 pure domain modules with matching domain tests.
- Outbound persistence ports/adapters exist for several services.
- MCP route counts reflect the post-apparatus surface: 98 total tools.
- `header_admit_stage_diff.c` and the public diff report API were deleted;
  the survivor header-admit test now verifies reorg self-heal directly through
  the durable log.
- `app/services/src/cutover_modes.c` and
  `app/services/include/services/cutover_modes.h` were deleted; the
  header-admit, validate-headers, and tip-finalize stages no longer expose
  SHADOW/AUTHORITATIVE runtime switches.
- The active-chain window move no longer publishes the public reducer
  authority. `active_chain_move_window_tip()` is a local cache/window
  primitive; tip authority is updated only through `tip_finalize_stage` or
  explicit trusted bootstrap/repair APIs.
- Trusted restored tips now stamp a durable `tip_finalize` anchor/cursor when
  the persisted stage cursor lags the restored public tip. Stale low
  `tip_finalize` cursors cannot replay old rows and republish a low public tip.
- `tip_finalize` failure rows such as `upstream_failed` advance the stage
  cursor only; they do not move public tip authority.
- `staged_sync_supervisor` no longer describes the active Wave-S jobs as a
  shadow pipeline, and its unused `datadir`/conservation-diff API parameter was
  removed.
- The S-5..S-8 Job headers/sources/tests no longer use comparison-era wording
  for the active reducer stages; the health controller comment now frames
  `log_head` as the reducer log head.
- Boot-time projection/event-log fan-out is now named
  `boot_start_projection_storage` / `boot_stop_projection_storage`; the
  `config/src/boot*.c` production boot surface no longer describes this active
  read-model wiring as shadow/cutover infrastructure.
- `utxo_recovery_restore.c` now carries rich `struct zcl_result` status inside
  its import/restore result structs, logs non-OK statuses at the boot caller,
  and is removed from the E2 service-result baseline.
- `utxo_recovery_service.c` now carries rich `struct zcl_result` status inside
  its recovery execution result, logs non-OK execution failures at the boot
  caller, and is removed from the E2 service-result baseline.
- `utxo_recovery_backfill.c` is split out as the shielded-value backfill
  service helper, with its own `zcl_result` argument validation and an explicit
  `-1` failure return. The split keeps `utxo_recovery_service.c` under the
  framework file-size ceiling.
- `chain_state_repository.c` now exposes `csr_commit_tip_result()`, a
  `struct zcl_result` wrapper over the legacy `enum csr_result` commit API, and
  is removed from the E2 service-result baseline while call sites migrate.
- `legacy_mirror_sync_service.c` now exposes
  `legacy_mirror_sync_request_catchup_result()`, routes the
  `legacy_mirror_stuck` condition through the rich result surface, and is
  removed from the E2 service-result baseline. `check_one_result_type` now
  supports the intended empty-baseline state.
- `sync_controller_import.c` is below the E1 file-size ceiling after extracting
  the LevelDB UTXO decode/bind helpers into `utxo_import_pipeline.c`, a Service
  helper with `zcl_result` writer-bind status. The E1 baseline no longer
  grandfathers `sync_controller_import.c`.
- `legacy_import.c` is below the E1 file-size ceiling after extracting raw
  block scanning, BIP34 discovery, Sapling prefilter, and decrypt workers into
  private controller helpers `legacy_import_scan.c` / `legacy_import_scan.h`.
- `sync_controller_catchup.c` is below the E1 file-size ceiling after extracting
  Sapling tree rebuild logic into `sync_controller_sapling_tree.c` and wallet /
  mempool persistence into `sync_controller_persistence.c`.
- `legacy_mirror_sync_service.c` is below the E1 file-size ceiling after moving
  lifecycle, stats, dump-state, and test-surface code into
  `legacy_mirror_sync_state.c` behind `legacy_mirror_sync_internal.h`.
- `tools/scripts/check_file_size_ceiling.sh` now supports the intended empty
  baseline state, and `tools/scripts/file_size_ceiling_baseline.txt` contains
  no file entries.
- Typed-blocker adoption is no longer grandfathered. The legacy mirror and
  mirror-consensus public stats now expose typed blocker classes plus
  reason/id fields, `block_source_policy` no longer carries a raw
  `selection_blocker` C field, the legacy JSON keys are preserved for clients,
  and `tools/scripts/typed_blocker_baseline.txt` is empty.
- Active-chain cache/window moves have been separated from public tip authority:
  production cache updates now call `active_chain_move_window_tip()`, while the
  compatibility `active_chain_set_tip()` wrapper remains marked as the
  grandfathered low-level surface. The E6 one-write-path baseline is down from
  34 to 26 write surfaces.
- `docs/work/` now contains only the parallel-worktree protocol. Obsolete
  cutover/B8 runbooks, stale reducer-ingest design snapshots, and a paused
  worker assignment that referenced deleted files were removed; source/test
  comments that pointed at those deleted docs are now self-contained.
- Production comments/log labels under `app/`, `lib/storage/`,
  `lib/validation/`, and `tools/mcp/` no longer call active projection paths
  shadow/cutover/projection-diff machinery. UTXO projection emit helpers are
  now named `*_projection`, not `*_shadow`.
- Production UTXO projection authorship is fixed on the stage/reducer path; the
  old author switch setter is now a `ZCL_TESTING`-only API, removing it from
  the E6 production write-surface baseline.
- Stale lib-layering baseline entries for removed validation files
  (`activate_best_chain.c`, `connect_tip.c`, `disconnect_tip.c`) were deleted,
  dropping the lib-to-app include baseline from 101 to 82.
- Stale lib-layering baseline entries for removed
  `msg_version.c` / `msgprocessor_snapshot.c` includes were deleted, and file
  manifest protocol declarations moved into
  `lib/net/include/net/file_manifest.h`, dropping the lib-to-app include
  baseline from 82 to 79.
- `wallet_scan.c` and `legacy_import.c` no longer call `sqlite3_exec()`
  directly; their checked exec helpers route through `node_db_exec()`, dropping
  the controller raw-SQL baseline from 14 to 12 controller files.
- `snapshot_controller.c`, `wallet_shielded_controller.c`, and
  `repair_controller.c` were removed from the controller raw-SQL baseline.
  Snapshot exec helpers now take `struct node_db *` and route through
  `node_db_exec()`, `z_listunspent` uses the existing block model height query,
  and UTXO height-repair count/update knowledge lives on `models/utxo`. The
  controller raw-SQL baseline is down from 12 to 9 controller files.
- `repair_controller_utxo.c` and `sync_controller_blocks.c` were removed from
  the controller raw-SQL baseline. `repairutxos` transaction control now uses
  `node_db_begin()` / `node_db_rollback()` / `node_db_commit()`, and
  per-block Sapling tree persistence is owned by
  `db_block_update_sapling_tree_data()`. The controller raw-SQL baseline is
  down from 9 to 7 controller files.
- `sync_controller_import.c` was removed from the controller raw-SQL baseline.
  Its post-import UTXO row/distinct-txid validation now calls
  `db_utxo_count_rows_and_distinct_txids()`, keeping table cardinality SQL on
  the UTXO model. The controller raw-SQL baseline is down from 7 to 6
  controller files.
- `wallet_controller_keys.c` was removed from the controller raw-SQL baseline.
  Key readback now uses `wallet_sqlite_read_single_key()`, rollback uses the
  new `wallet_sqlite_delete_key_r()`, and `test_wallet_persistence_cycle`
  covers the delete-key roundtrip. The controller raw-SQL baseline is down
  from 6 to 5 controller files.
- `blockchain_controller.c` was removed from the controller raw-SQL baseline.
  MMR/MMB/commitment-MMR state persistence now uses `node_db_state_get()` /
  `node_db_state_set()`. The controller raw-SQL baseline is down from 5 to 4
  controller files.
- `file_controller_export.c` was deleted from the controller layer. Consensus
  snapshot export now lives in
  `consensus_snapshot_export_service_run()` with a `struct zcl_result` service
  return, and boot/test callers use the service header directly. The
  controller raw-SQL baseline is down from 4 to 3 controller files.
- `blockchain_controller_admin.c` was removed from the controller raw-SQL
  baseline. `importchainstate` now calls
  `db_utxo_rebuild_wallet_and_address_caches()`,
  `db_utxo_total_value()`, and `db_wallet_utxo_balance()` instead of owning
  cache-rebuild and reporting SQL directly. `test_models` covers the derived
  wallet/address cache rebuild. The controller raw-SQL baseline is down from
  3 to 2 controller files.
- `snapshot_controller_txindex.c` and `dbquery_controller.c` were removed from
  the controller raw-SQL baseline. The tx-index job now routes additive-build
  database tuning through `db_tx_configure_additive_build()` and the block-file
  scan query through `db_block_prepare_file_position_scan()`. The `zcl_sql`
  diagnostic primitive now prepares statements through
  `node_db_prepare_readonly_query()`, which rejects writable statements at the
  database boundary. The controller raw-SQL baseline is empty.
- Production raw malloc/calloc/realloc allowlist debt is at zero active
  entries after migrating the boot-services DB path allocation to
  `zcl_malloc()` and the connman deferred-free resize to `zcl_realloc()`.

## Active Debt

### Delete Or Move Out Of Production

- No production `shadow`/`cutover`/`projection-diff` matches remain in
  `app/`, `lib/storage/`, `lib/validation/`, or `tools/mcp/` C/H files outside
  tests/views. Keep this at zero; normalize remaining historical test/doc
  wording only when it obscures current behavior.

### E1 Oversized App Files

`tools/scripts/file_size_ceiling_baseline.txt` is empty. There are no
grandfathered oversized app `.c` files; keep this gate at zero.

### E2 Service Result Debt

`tools/scripts/one_result_type_baseline.txt` is empty. The file-level ratchet is
at zero grandfathered service files. Remaining work is call-site cleanup:
legacy compatibility bool APIs should migrate toward `struct zcl_result` as
their owning files are split or touched for adjacent debt.

### E6 One-Write-Path Debt

From `tools/scripts/one_write_path_baseline.txt`:

- controller/admin `coins_view_cache_flush` call sites
- coins.db batch writer declarations and implementations
- process-block flush-policy write paths
- the compatibility `active_chain_set_tip()` wrapper, while remaining
  production cache/window moves use `active_chain_move_window_tip()`

The final form is one durable writer and one cursor authority.
Current guardrail: low-level active-chain cache/window moves and stale
`tip_finalize` cursors cannot regress the public reducer tip.

### Supervisor Debt

`tools/scripts/supervisor_baseline.txt` is empty. Every long-running service
that gate tracks now registers a supervisor liveness contract.

### Typed Blocker Debt

`tools/scripts/typed_blocker_baseline.txt` is empty. Raw blocker string fields
and legacy blocker setters are not grandfathered; keep this gate at zero.

### Controller And Layering Debt

- Lib-layering debt remains behind `tools/scripts/lib_layering_baseline.txt`
  with 79 grandfathered lib-to-app includes.
- Controller raw-SQL debt is at zero grandfathered files. Keep
  `tools/lint/no_raw_sqlite_in_controllers_baseline.txt` empty.
- `lib/validation/src/process_block_core.c` still mixes chain selection,
  block-index hydration, tip commit, and failed-child propagation.

## Next Work Order

1. Delete zero-purpose scaffolding and stale build references.
2. Keep production terminology clean; normalize remaining historical test/doc
   fixture names only when touched for adjacent work.
3. Keep import/catchup/legacy-import code below the file-size ceiling while
   moving remaining mixed-purpose code toward the correct framework shape.
4. Split `process_block_core.c` by responsibility.
5. Keep E1/E2/controller-SQL baselines empty and pay down E6/lib-layering
   until they are empty.
6. Run `make lint`, rebuild `test_parallel`, run the suite, then prove live
   node progress with a soak.

## Latest Verification

- `make -j$(nproc)`: pass after emptying the controller raw-SQL baseline.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, and raw-malloc gates remain at zero
  active debt, E6 is 24 grandfathered write surfaces, and lib-layering is 79
  grandfathered includes.
- `make test_parallel`: pass after the controller raw-SQL baseline reached
  zero.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with an empty baseline. WARN mode reports 0 direct raw controller SQL
  calls.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline reached zero.
- `./test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass after
  moving the `zcl_sql` prepare path behind
  `node_db_prepare_readonly_query()`.
- `./test_parallel --only=sqlite --timeout=120 --verbose`: pass, including the
  snapshot tx-index job start/join path.
- `./test_parallel --only=models --timeout=120 --verbose`: pass, including
  tx-index bulk-load lifecycle coverage.
- `./test_parallel --timeout=180`: pass after the controller raw-SQL baseline
  reached zero, `0/279` groups failed in 59.0s.
- Quick live sample at 2026-06-01 03:31:20 UTC after the controller raw-SQL
  baseline reached zero: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a 10-minute journal scan found no low-tip
  regression, integrity failure, OOM, fatal, segfault, assert, or panic signal.
  This is a continuity check, not the final soak.
- `git diff --check`: pass after the `blockchain_controller_admin.c`
  controller raw-SQL shrink.
- `make -j$(nproc)`: pass after moving consensus snapshot export out of the
  controller layer into `consensus_snapshot_export_service_run()`.
- `make test_parallel`: pass after touching
  `lib/test/src/test_file_controller.c`.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  3 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the existing boot-services
  `coins_view_cache_flush` E6 entries line-stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 3 grandfathered controller files, no new ones. WARN mode now
  reports 12 direct raw controller SQL calls across those 3 files.
- `./test_parallel --only=file_controller --timeout=120 --verbose`: pass after
  moving consensus snapshot export to the service layer; `0/1` filtered groups
  failed in 2.0s.
- `./test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 03:08:22 UTC after the controller raw-SQL
  shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:50:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `git diff --check`: pass after the consensus snapshot export service move.
- `make test_parallel`: pass after adding
  `wallet_sqlite_delete_key_r()` and the wallet persistence roundtrip test.
- `make -j$(nproc)`: pass after moving `wallet_controller_keys.c` key
  readback/rollback SQL into `wallet_sqlite`.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  5 grandfathered controller files.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 5 grandfathered controller files, no new ones. WARN mode now
  reports 21 direct raw controller SQL calls across those 5 files.
- `./test_parallel --only=wallet_persistence_cycle --timeout=120 --verbose`:
  pass, including the new `delete_key_r` persisted-key removal case.
- `./test_parallel --only=wallet --timeout=120 --verbose`: pass after moving
  wallet-key readback/rollback SQL into `wallet_sqlite`; `0/32` filtered groups
  failed in 7.0s.
- `./test_parallel --timeout=180`: pass after the wallet/controller raw-SQL
  shrink, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 02:50:33 UTC after the wallet/controller
  raw-SQL shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:50:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `make -j$(nproc)`: pass after moving `sync_controller_import.c` UTXO
  cardinality validation into the UTXO model.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  6 grandfathered controller files.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 6 grandfathered controller files, no new ones. WARN mode now
  reports 23 direct raw controller SQL calls across those 6 files.
- `./test_parallel --only=sync_service --timeout=120 --verbose`: pass after
  moving sync-import UTXO cardinality validation into the UTXO model; filtered
  run covered both `test_sync_service` and `test_snapshot_sync_service`.
- `./test_parallel --only=utxo_recovery_service --timeout=120 --verbose`:
  pass.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `./test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 02:43:32 UTC after the controller raw-SQL
  shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:40:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `make -j$(nproc)`: pass after moving `repair_controller_utxo.c` transaction
  control to `node_db_*()` and `sync_controller_blocks.c` Sapling tree writes
  to the Block model.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  7 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the `repair_controller_utxo.c`
  `coins_view_cache_flush` E6 lines stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 7 grandfathered controller files, no new ones. WARN mode now
  reports 24 direct raw controller SQL calls across those 7 files.
- `./test_parallel --only=sync_service --timeout=120 --verbose`: pass after
  moving Sapling tree block persistence into the Block model; filtered run
  covered both `test_sync_service` and `test_snapshot_sync_service`.
- `./test_parallel --only=sapling_tree --timeout=120 --verbose`: pass.
- `./test_parallel --only=utxo_activation_paused --timeout=120 --verbose`: pass
  after moving `repairutxos` transaction control to `node_db_*()`.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `./test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 02:34:30–02:35:07 UTC after the follow-up
  controller raw-SQL shrink: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a short log tail found no low-tip
  regression, integrity failure, OOM, fatal, segfault, or assert signal. This
  is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after the snapshot/wallet/repair controller raw-SQL
  shrink and UTXO model helper extraction.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL
  was then 9 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the blockchain admin E6 baseline
  line-stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with then-9 grandfathered controller files, no new ones. WARN mode then
  reported 28 direct raw controller SQL calls across those 9 files.
- `./test_parallel --only=wallet --timeout=120 --verbose`: pass after replacing
  the shielded wallet height fallback with `db_block_max_height_any_status()`.
- `./test_parallel --only=snapshot_sync_service --timeout=120 --verbose`: pass
  after routing snapshot checked exec helpers through `node_db_exec()`.
- `./test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after moving UTXO missing-height count/repair SQL into `models/utxo`.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `./test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 02:24:01–02:24:57 UTC after the controller
  raw-SQL shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and `getpeerinfo` showed active peers with
  peer tip around `3131915`. A short log tail found no low-tip regression or
  integrity failure. This is a continuity check, not the final soak.
- `git diff --check`: pass after the controller raw-SQL shrink.
- `make -j$(nproc)`: pass after the file-manifest header extraction, controller
  raw-SQL cleanup, and raw allocation wrapper migration.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL
  was then 12 grandfathered controller files.
- `make test_parallel`: pass after making the `body_fetch_stage` crash-replay
  test deterministic with a child-ready pipe.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones.
- `tools/scripts/check_lib_layering.sh`: pass with 79 grandfathered lib-to-app
  includes, no new ones.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with then-12 grandfathered controller files, no new ones.
- `tools/scripts/check_raw_malloc.sh`: pass with no active production raw
  malloc/calloc/realloc allowlist entries.
- `./test_parallel --only=file_controller --timeout=120 --verbose`: pass after
  moving file manifest protocol declarations into `lib/net`.
- `./test_parallel --only=file_market --timeout=120 --verbose`: pass.
- `./test_parallel --only=wallet --timeout=120 --verbose`: pass after routing
  wallet scan / legacy import table clearing through `node_db_exec()`.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the lib-layering/controller-SQL/raw-malloc baseline shrink.
- First full `./test_parallel --timeout=180` after the edits exposed the
  existing timing-sensitive `test_body_fetch_stage` crash subcase
  (`zero-progress case is consistent`, `rows bounded by chain size`). The crash
  test now waits for a child-ready pipe after deterministic drain; focused
  rerun passed.
- Second full `./test_parallel --timeout=180`: pass with `0/279` groups failed
  in 121.0s.
- Live-node sample at `2026-06-01 02:09:55 UTC`: not proven. `systemctl --user
  is-active zclassic23` could not connect to the user bus, `./tools/zcl-rpc
  getblockcount` exited 7, and no `zclassic23`/`zclassicd` process or
  `18232`/`8232`/`8033` listener was visible.
- `rg -ni "shadow|cutover|projection-diff|shadow-diff" app lib/storage lib/validation tools/mcp --glob '*.c' --glob '*.h' --glob '!lib/test/**' --glob '!app/views/**'`:
  clean after normalizing uppercase production "Shadow" comments too.
- `rg -n "shadow|cutover|projection-diff|shadow-diff" app lib/storage lib/validation tools/mcp --glob '*.c' --glob '*.h' --glob '!lib/test/**' --glob '!app/views/**'`:
  clean after deleting stale production wording and renaming UTXO projection
  emitters.
- `make -j$(nproc)`: pass after the production wording cleanup and UTXO
  projection emitter rename.
- `make lint`: pass after re-anchoring the existing
  `utxo_projection_set_author` grandfathered E6 line; E1, E2, supervisor, E7,
  and typed-blocker baselines remain at zero, E6 remains at 26, and
  lib-layering remains at 101.
- `./test_parallel --only=utxo_apply_authorship --timeout=120 --verbose`:
  pass after the UTXO projection emitter rename.
- `./test_parallel --only=reorg_projection_parity --timeout=120 --verbose`:
  pass after the projection wording cleanup.
- `./test_parallel --only=reorg_parity --timeout=120 --verbose`: pass.
- `./test_parallel --only=block_index_backfill --timeout=120 --verbose`: pass.
- `./test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass.
- `./test_parallel --timeout=180`: pass after the production wording cleanup,
  `0/279` groups failed in 67.0s.
- `git diff --check`: pass after the production wording cleanup and status
  update.
- `rg` for deleted `docs/work` plan names and source-comment references:
  clean after removing obsolete cutover/B8 runbooks, stale reducer-ingest design
  snapshots, and the paused import-path worker assignment.
- `make lint`: pass after the `docs/work` stale-plan deletion; E1, E2,
  supervisor, E7, and typed-blocker baselines remain at zero, E6 remains at 26,
  and lib-layering remains at 101.
- `git diff --check`: pass after the `docs/work` stale-plan deletion.
- `make -j$(nproc)`: pass after splitting active-chain cache/window moves from
  public tip authority.
- `make lint`: pass; E1, E2, supervisor, E7, and typed-blocker baselines are at
  zero, E6 is down to 26 grandfathered write surfaces, and lib-layering remains
  at 101.
- `./test_parallel --only=chain_state_repo --timeout=120 --verbose`: pass after
  the active-chain cache/window API split.
- `./test_parallel --only=chain_tip --timeout=120 --verbose`: pass.
- `./test_parallel --only=tip_finalize_stage --timeout=120 --verbose`: pass;
  includes the authority guard proving a raw low-level active-chain cache move
  does not lower public reducer height.
- `./test_parallel --only=invalidateblock --timeout=120 --verbose`: pass after
  migrating invalidate-path cache movement to `active_chain_move_window_tip()`.
- `./test_parallel --only=process_block_revalidate --timeout=120 --verbose`:
  pass.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass;
  includes the E6 fixture that proves a new writer still trips the ratchet.
- `./test_parallel --timeout=180`: pass after the active-chain cache/window API
  split, `0/279` groups failed in 69.0s.
- `git diff --check`: pass after the active-chain cache/window API split and
  doc update.
- `make -j$(nproc)`: pass after the projection-storage boot rename.
- `make test_parallel`: pass.
- `./test_parallel --only=tip_finalize_stage --timeout=120 --verbose`: pass;
  includes `authority_guard` and `stale_cursor`, proving raw low-level
  active-chain cache moves do not lower public reducer height and stale low
  `tip_finalize` cursors anchor above a restored high tip instead of replaying.
- `./test_parallel --only=reducer_ingest_e2e --timeout=120 --verbose`: pass.
- `./test_parallel --only=chain_restore_service --timeout=120 --verbose`: pass.
- `./test_parallel --only=supervisor --timeout=120 --verbose`: pass after the
  staged-sync supervisor API/comment cleanup.
- `./test_parallel --only=body_persist_stage --timeout=120 --verbose`: pass
  after the S-5 Job/test wording cleanup.
- `./test_parallel --only=script_validate_stage --timeout=120 --verbose`: pass
  after the S-6 Job/test wording cleanup.
- `./test_parallel --only=proof_validate_stage --timeout=120 --verbose`: pass
  after the S-7 Job/test wording cleanup.
- `./test_parallel --only=utxo_apply_stage --timeout=120 --verbose`: pass after
  the S-8 Job/test wording cleanup.
- `./test_parallel --only=boot_phase --timeout=120 --verbose`: pass after the
  boot projection-storage wording/function rename.
- `./test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after rebuilding `test_parallel`; covers the new import/restore `zcl_result`
  status paths, including invalid-context errors. Re-run after the execution
  status/backfill split: pass.
- `./test_parallel --only=chain_state_repo --timeout=120 --verbose`: pass after
  adding `csr_commit_tip_result()`; covers the `zcl_result` failure wrapper.
- `./test_parallel --timeout=180`: pass, `0/279` groups failed in 72.1s after
  the recovery execution status/backfill split. Re-run after the CSR wrapper:
  pass, `0/279` groups failed in 70.0s.
- `git diff --check`: pass after the S-5..S-8, health-comment, and
  projection-storage boot cleanup; re-run after the recovery split: pass.
- `make -j$(nproc)`: pass after the recovery execution status/backfill split
  and again after the CSR wrapper.
- `rg` over `config/src/boot_services.c` and `config/src/boot.c` finds no
  `shadow`/`cutover`/`SHADOW`/`AUTHORITATIVE` boot-surface references.
- `make lint`: pass after the legacy mirror result wrapper; E2 is at zero
  grandfathered service files, E7 has zero grandfathered entries, E1 remains
  at 4, typed-blocker remains at 4, lib-layering remains at 101, and E6 remains
  at 34.
  The E6 baseline change only re-anchored the same three
  `config/src/boot_services.c` `coins_view_cache_flush` entries after comment
  cleanup shifted their line numbers.
- `./test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  adding `legacy_mirror_sync_request_catchup_result()`; covers the non-OK
  `zcl_result` path carrying `hash-disagreement`.
- `./test_parallel --only=legacy_mirror_stuck_condition --timeout=120 --verbose`:
  pass after routing the remedy through the result-returning catchup API.
- `make -j$(nproc)`: pass after the legacy mirror result wrapper.
- `./test_parallel --timeout=180`: pass after the legacy mirror result wrapper,
  `0/279` groups failed in 79.1s.
- `make -j$(nproc)`: pass after extracting `utxo_import_pipeline.c` from
  `sync_controller_import.c`.
- `./test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after the import-pipeline helper split.
- `./test_parallel --only=sync_service --timeout=120 --verbose`: pass after the
  import-pipeline helper split.
- `make lint`: pass after the import-pipeline helper split; E1 is down to 3
  grandfathered oversized app files, E2 remains at zero, E7 remains at zero,
  typed-blocker remains at 4, lib-layering remains at 101, and E6 remains at
  34.
- `./test_parallel --timeout=180`: pass after the import-pipeline helper split,
  `0/279` groups failed in 78.0s.
- `make -j$(nproc)`: pass after the `legacy_import.c`,
  `sync_controller_catchup.c`, and `legacy_mirror_sync_service.c` E1 splits.
- `make lint`: pass after emptying the E1 baseline; E1 is at zero grandfathered
  oversized app files, E2 remains at zero, typed-blocker remains at 4,
  lib-layering remains at 101, and E6 remains at 34.
- `./test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  the legacy mirror state split; covers catchup failure reporting and dump-state
  fields.
- `./test_parallel --only=legacy_mirror_stuck_condition --timeout=120 --verbose`:
  pass after the legacy mirror state split; covers condition remedy routing.
- `./test_parallel --only=lag_slo --timeout=120 --verbose`: pass after the
  legacy mirror state split; covers the monitor contract dump shape.
- `./test_parallel --only=sync_service --timeout=120 --verbose`: pass after the
  sync catchup/persistence split; filtered run covered both `test_sync_service`
  and `test_snapshot_sync_service`.
- `./test_parallel --only=sqlite --timeout=120 --verbose`: pass after the sync
  persistence split; covers sync job wrappers, mempool persistence, and DB
  service writes.
- `./test_parallel --only=sapling_tree --timeout=120 --verbose`: pass after the
  Sapling tree rebuild split.
- `./test_parallel --timeout=180`: pass after the E1 baseline reached zero,
  `0/279` groups failed in 65.0s.
- `git diff --check`: pass after the E1 baseline reached zero and docs were
  updated.
- `make -j$(nproc)`: pass after replacing the remaining typed-blocker baseline
  surfaces with typed class plus reason/id fields.
- `make test_parallel`: pass after the typed-blocker public-struct rename.
- `make lint`: pass after emptying `tools/scripts/typed_blocker_baseline.txt`;
  E1, E2, E7, supervisor, and typed-blocker baselines are at zero. E6 remains
  at 34 and lib-layering remains at 101.
- `./test_parallel --only=chain_advance_coordinator --timeout=120 --verbose`:
  pass after the source-policy `selection_reason` struct-field rename while
  preserving the legacy `selection_blocker` JSON key.
- `./test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  the legacy mirror and mirror-consensus typed blocker stats rename.
- `./test_parallel --only=lag_slo --timeout=120 --verbose`: pass after the
  legacy mirror typed blocker stats rename.
- `./test_parallel --only=syncdiag_rpc --timeout=120 --verbose`: pass after the
  diagnostics JSON compatibility check.
- `./test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass after
  the MCP status compatibility check.
- `./test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the typed-blocker baseline became empty and `check_typed_blocker.sh` learned
  the empty-baseline count.
- `./test_parallel --timeout=180`: pass after the typed-blocker baseline reached
  zero, `0/279` groups failed in 70.0s.
- Live node negative proof: the 2026-05-31 22:34 UTC restart initially
  restored RPC/UTXO height to `3130701`, then the old stale cursor path
  regressed the public tip back into the ~44k range after background validation
  had time to run.
- Live node patched proof: after the 2026-05-31 22:54 UTC restart, boot
  restored RPC/UTXO height to `3130701`; `tip_finalize` stamped
  `authority anchor cursor from=45685 to=3130702 reason=init_existing_tip`;
  `progress.kv` contains `tip_finalize_log` row `(3130701, anchor, ok=1)`.
- Live soak 2026-05-31 22:56:05–23:00:40 UTC: every 30s sample reported
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and the latest tip-finalize row remained
  `(3130701, anchor, ok=1)`. Post-soak log scan found no low-tip `commit_tip`,
  `chain_integrity_failed`, or orphan-UTXO regression. Background validation is
  still running from height `44770` toward `3130701`, so this is a live
  regression proof, not final refactor completion.
- After the staged-sync supervisor cleanup and rebuild, the current binary was
  restarted again at 2026-05-31 23:07 UTC. Samples through 23:10 UTC stayed at
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`, and
  `stage_cursor.tip_finalize=3130702`; logs showed all eight staged jobs
  initialised and no low-tip commit or integrity regression. Background
  validation resumed from height `55001` toward `3130701`.
- Quick live sample at 2026-05-31 23:24 UTC after this cleanup still reported
  `getblockcount=3130701` and `gettxoutsetinfo.height=3130701`. This is a
  continuity check, not a replacement for the required final soak.
- Quick live sample at 2026-05-31 23:32 UTC after the E2 shrink still reported
  `getblockcount=3130701` and `gettxoutsetinfo.height=3130701`.
- Quick live sample at 2026-06-01 00:00:45 UTC after the E2 baseline reached
  zero: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and latest `tip_finalize_log` row
  remained `(3130701, anchor, ok=1)`. A tail scan found no low-tip regression
  or integrity failure. This is a continuity check, not the final soak.
- Quick live sample at 2026-06-01 00:14:19 UTC after the E1 shrink:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and latest `tip_finalize_log` row
  remained `(3130701, anchor, ok=1)`. A tail scan found no low-tip regression
  or integrity failure. This is a continuity check, not the final soak.
- Quick live sample at 2026-06-01 00:47 UTC after the E1 baseline reached zero:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, and `gettxoutsetinfo.height=3130701` with
  `txouts=1348252`; a node.log tail scan found no low-tip commit, integrity
  failure, or orphan-UTXO regression. This is a continuity check, not the final
  soak.
- Live sample attempt at 2026-06-01 01:00 UTC after the typed-blocker shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, RPC port `18232` was closed, no `zclassic23` process was
  running, and the journal showed `zclassic23.service` was OOM-killed at
  2026-06-01 00:51:52 UTC after `zclassicd-rhett` was OOM-killed with a 6.0G
  memory peak. This is a failed live sample, not a refactor completion proof.
- Live sample attempt at 2026-06-01 01:10 UTC after the active-chain E6 shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, `./tools/zcl-rpc getblockcount` exited with code 7, no
  `zclassic23` or `zclassicd` process was running, and no listener existed on
  ports `18232`, `8232`, or `8033`. This is a failed live sample, not a refactor
  completion proof.
- Live sample attempt at 2026-06-01 01:32 UTC after the production wording
  cleanup: no continuity proof was available. `systemctl --user` could not
  connect to the user bus, `./tools/zcl-rpc getblockcount` exited with code 7,
  no `zclassic23` or `zclassicd` process was running, and no listener existed
  on ports `18232`, `8232`, or `8033`. This is a failed live sample, not a
  refactor completion proof.
- Live sample attempt at 2026-06-01 01:47 UTC after the E6/lib-layering shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, `./tools/zcl-rpc getblockcount` exited with code 7, no
  `zclassic23` or `zclassicd` process was running, and no listener existed on
  ports `18232`, `8232`, or `8033`. This is a failed live sample, not a refactor
  completion proof.

Do not mark this refactor complete while any ratchet baseline contains a real
entry or the live node proof is missing.
