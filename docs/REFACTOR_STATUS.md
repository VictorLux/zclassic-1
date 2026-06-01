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
- E1, E2, E6, supervisor, E7, typed-blocker, controller raw-SQL adoption,
  lib-layering, and raw allocation debt are at zero grandfathered entries.
  Remaining refactor work is now code-shape cleanup, process-block splitting,
  doc honesty, and live-node proof rather than baseline burn-down.

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
  production cache updates now call `active_chain_move_window_tip()`. The E6
  one-write-path baseline was initially reduced from 34 to 26 write surfaces
  after moving public tip authority to reducer stages and explicit
  repair/bootstrap APIs.
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
- The next lib-layering slice moved `zcl_node_db_path()` from app views to
  `lib/util`, moved UTXO script classification from the ActiveRecord model to
  `lib/script`, replaced the `msg_internal.h` service include with a forward
  declaration, and removed the storage-layer model include from
  `coins_view_sqlite.c`, dropping the lib-to-app include baseline from 79 to
  75.
- Schema migration now lives in the Model shape instead of `lib/storage`, and
  file-offer SQLite persistence moved from `lib/net/src/file_market.c` into
  `app/models/src/file_offer.c` behind the FileOffer model lifecycle. The
  lib-to-app include baseline is down from 75 to 71.
- ZMSG SQLite persistence moved from `lib/net/src/zmsg.c` into
  `app/models/src/zmsg.c` behind the Zmsg model lifecycle. `lib/net/src/zmsg.c`
  now owns only wire serialization, message IDs, and the in-memory delivery
  cache. The lib-to-app include baseline is down from 71 to 68.
- ZNAM at-rest record structs and SQLite persistence moved from
  `lib/znam/include/znam/znam.h` / `lib/znam/src/znam.c` into
  `app/models/include/models/znam.h` / `app/models/src/znam.c`. The lib ZNAM
  files now own only OP_RETURN protocol parsing/building, and the lib-to-app
  include baseline is down from 68 to 65.
- Swap-contract persisted records and SQLite persistence moved from
  `lib/script/include/script/htlc.h` / `lib/script/src/htlc.c` into
  `app/models/include/models/swap_contract.h` /
  `app/models/src/swap_contract.c`. The lib HTLC files now own only script
  building/parsing, address helpers, secrets, and swap IDs, and the lib-to-app
  include baseline is down from 65 to 62.
- Addrman sidecar integrity moved from `app/services` into `lib/net`, and the
  shared SHA3 sidecar helper moved from `app/services` into `lib/storage`.
  `connman.c` now includes `net/addrman_integrity.h` for peers.dat integrity,
  while block-index sidecar service code uses the storage helper. The
  lib-to-app include baseline is down from 62 to 61.
- Mining found-block submission moved out of `lib/mining` and into its callers.
  `gen_context` now exposes a found-block callback, boot/controller code owns
  reducer ingestion for mined blocks, and `lib/mining/src/miner.c` no longer
  includes the app activation service. The lib-to-app include baseline is down
  from 61 to 60.
- Connman onion peer discovery is now a net-layer callback registered by boot.
  The shared `struct onion_peer` contract lives under `lib/net`, boot injects
  `blog_discover_onion_peers()`, and `lib/net/src/connman.c` no longer
  includes the blog controller. The lib-to-app include baseline is down from
  60 to 59.
- Onion service blog serving and peer discovery are now net-layer app-handler
  callbacks registered by boot. `lib/net/src/onion_service.c` owns Tor/onion
  routing only, delegates blog responses through `onion_blog_serve_fn`, and no
  longer includes the blog controller. The lib-to-app include baseline is down
  from 59 to 58.
- Compact-block reducer submission is now a net-layer callback registered by
  boot. `lib/net/src/msg_compact.c` owns only BIP152 reconstruction and peer
  scoring, while boot maps completed compact blocks to
  `REDUCER_SRC_COMPACT`. The lib-to-app include baseline is down from 58 to
  57.
- Handshake peer persistence is now a net-layer callback registered by boot.
  `lib/net/src/msg_version.c` owns version/verack protocol state only, while
  boot owns the async `db_service_enqueue_write()` /
  `db_peer_save_advisory()` model write. The lib-to-app include baseline is
  down from 57 to 55.
- Metrics service/model gauges and connman known-ZCL23 peer selection are now
  boot-owned callback injections. `lib/metrics/src/metrics.c` owns console and
  Prometheus gauge publishing without including app services/models, and
  `lib/net/src/connman.c` owns outbound peer selection without including the
  Peer model or runtime singleton. The lib-to-app include baseline is down
  from 55 to 50.
- Tx wallet persistence and the snapshot-active query are now net-layer
  callbacks registered by boot. `lib/net/src/msg_tx.c` owns transaction relay,
  mempool classification, peer scoring, Dandelion propagation, and inventory
  request policy without including app controller/model/service headers. The
  lib-to-app include baseline is down from 50 to 46.
- P2P block reducer submission is now a net-layer callback registered by boot,
  and block-message snapshot gating uses the already-injected snapshot-active
  callback. `lib/net/src/msg_blocks.c` owns block/getdata/getblocks protocol
  handling without including app controller/model/activation/snapshot headers.
  The lib-to-app include baseline is down from 46 to 41.
- Block-connected tip observers are now a net-layer callback registered by
  boot. `lib/net/src/msg_blocks.c` no longer includes the sync monitor service;
  boot owns the `sync_monitor_on_block_connected()` side effect. The
  lib-to-app include baseline is down from 41 to 40.
- Block-sync planning for invalid-block retries and valid-block acceptance now
  runs through net-internal message-processor helpers. `lib/net/src/msg_blocks.c`
  owns block/getdata/getblocks wire handling without including the block sync
  service. The lib-to-app include baseline is down from 40 to 39.
- Stale unused app-layer includes were removed from `msg_headers.c`,
  `msgprocessor.c`, and `msgprocessor_snapshot.c`. FlyClient proof building is
  now a boot-owned callback injection, so `msgprocessor_snapshot.c` no longer
  reaches into the blockchain controller or the MMB leaf-store model. The
  lib-to-app include baseline is down from 39 to 32.
- Snapshot block-piece serving now callback-injects block-hash range loading
  and local UTXO SHA3 computation from boot. `msgprocessor_snapshot.c` no
  longer includes the Block model or dereferences `struct node_db`; the
  lib-to-app include baseline is down from 32 to 31.
- ZMSG, file-offer, and file-service P2P persistence is now callback-injected
  from boot. `lib/net/src/msgprocessor.c` owns protocol handling without
  including the node DB model header or the FileService model; the lib-to-app
  include baseline is down from 31 to 29.
- Snapshot-sync service accessors moved from `lib/net/src/msgprocessor.c` into
  `lib/net/src/msgprocessor_snapshot.c`, where the snapshot service dependency
  already belongs. Generic message-processor orchestration no longer includes
  the snapshot sync service, and the lib-to-app include baseline is down from
  29 to 28.
- Header/block sync planner contracts now live in
  `lib/sync/include/sync/sync_planner.h`, with the app service headers kept as
  compatibility wrappers. `lib/net/src/msgprocessor.c` and
  `lib/net/src/msg_headers.c` now use the lib-owned planner contract instead
  of the header/block sync app service headers, and the lib-to-app include
  baseline is down from 28 to 24.
- The header-anchor repair path no longer requires the net header handler to
  include the app chain-tip service. The current CSR-less fallback routes
  through boot-owned chain-state callbacks, and the lib-to-app include
  baseline is down from 24 to 23.
- Peer header votes for the quorum oracle are now callback-injected from boot.
  `lib/net/src/msg_headers.c` records accepted fast-sync peer header votes
  through the message-processor callback surface, while boot owns
  `quorum_oracle_record_peer_header_vote()`. The lib-to-app include baseline
  is down from 23 to 22.
- Process-block `node_db` open checks now route through the runtime boundary.
  `config/src/runtime.c` owns the one `struct node_db` layout check, while
  `lib/validation/src/process_block.c` keeps the DB handle opaque and no
  longer includes the DB model header. The lib-to-app include baseline is down
  from 22 to 21.
- Process-block flush-policy DB state persistence, sync-batch flush, and WAL
  checkpoint operations now route through the same runtime boundary.
  `lib/validation/src/process_block_flush_policy.c` no longer includes the DB
  model header or dereferences `struct node_db`; the lib-to-app include
  baseline is down from 21 to 20.
- Process-block self-heal durable UTXO max-height checks now route through the
  runtime boundary. `config/src/runtime.c` owns the SQLite query over `utxos`,
  while `lib/validation/src/process_block_self_heal.c` keeps the DB handle
  opaque for that check and no longer includes the DB model header. The
  lib-to-app include baseline is down from 20 to 19.
- Process-block self-heal tx-index recovery now routes through a runtime-owned
  `app_runtime_tx_index_hit` result. `config/src/runtime.c` owns the TxIndex
  model lookup, while `lib/validation/src/process_block_self_heal.c` keeps the
  model record type opaque and no longer includes the TxIndex model header.
  The lib-to-app include baseline is down from 19 to 18.
- Fast-sync chunk apply now uses direct SQLite bind calls plus the lib-side
  `AR_STEP_WRITE` helper instead of direct ActiveRecord bind/step macros.
  `lib/net/src/fast_sync.c` no longer directly includes the ActiveRecord or DB
  model headers, and the lib-to-app include baseline is down from 18 to 16.
- Fast-sync snapshot prebuild now takes a caller-owned serializer callback.
  Boot injects the UTXO model serializer through
  `boot_serialize_utxo_snapshot()`, while `lib/net/src/fast_sync.c` owns only
  protocol pathing and metadata publishing. The lib-to-app include baseline is
  down from 16 to 15.
- Header-sync snapshot active/anchor access is now callback-injected through
  the message processor. Boot wires `snapsync_is_active()` plus anchor get/set
  callbacks, while `lib/net/src/msg_headers.c` uses
  `msg_processor_snapshot_active()` / anchor helpers and no longer includes the
  snapshot sync service. The lib-to-app include baseline is down from 15 to
  14.
- Header activation, block-file scanning, height-repair state, and
  post-activation anchor repair are now callback-injected through the message
  processor. Boot owns `activation_request_connect()`,
  `activation_clear_anchor()`, `scan_block_files_mark_data()`,
  `block_index_heights_repaired()`, and `bii_repair_post_activation_anchor()`,
  while `lib/net/src/msg_headers.c` uses app-free message-processor helpers.
  The lib-to-app include baseline is down from 14 to 12.
- Header best-tip promotion and snapshot-anchor recommit now go through
  boot-owned message-processor callbacks. Boot owns
  `csr_commit_header_tip()` / `csr_commit_tip()` and the `ZCL_TESTING`
  fallback, while `lib/net/src/msg_headers.c` uses
  `msg_processor_commit_header_tip()` /
  `msg_processor_recommit_snapshot_anchor()` and no longer includes the
  chain-state repository. The lib-to-app include baseline is down from 12 to
  11.
- Stale `process_block_core.c` app includes for deleted legacy engine surfaces
  were removed. Gap-fill wakeups are now boot-injected through a
  mutex-protected `process_block_set_gap_fill_kick()` hook, so validation no
  longer includes the gap-fill service or chain-tip service and keeps only the
  real chain-evidence/chain-state-repository app edges. The lib-to-app include
  baseline is down from 11 to 3.
- Process-block tip publication is now boot-owned through
  `process_block_set_tip_publication_hooks()`. Validation passes
  `process_block_tip_evidence` over the hook boundary, while boot translates
  that evidence to the chain-evidence controller / CSR app services and owns
  the test fallback. `lib/validation/src/process_block_core.c` no longer
  includes chain-evidence or chain-state-repository headers, and the
  lib-to-app include baseline is down from 3 to 1.
- Process-block runtime hook dispatch and failed-child propagation were split
  out of `process_block_core.c`. `process_block_runtime_hooks.c` now owns the
  mutex-protected gap-fill and tip-publication callback bridges, while
  `process_block_failed_child.c` owns the bounded `BLOCK_FAILED_CHILD`
  propagation helper and OOM-amplifier guards. `process_block_core.c` is down
  from 1065 to 893 lines.
- Block-index disk placement and hydration moved from
  `process_block_core.c` into `process_block_index.c`. The new file owns
  `find_block_pos()`, `block_index_refresh_header()`,
  `block_index_hydrate_from_disk()`, and the test hydration wrapper. At that
  slice, `process_block_core.c` was down from 893 to 776 lines.
- Tip-publication evidence and commit mechanics moved from
  `process_block_core.c` into `process_block_tip_publish.c`. The new file owns
  `process_block_tip_is_best_work()`, `process_block_commit_tip()`,
  `update_tip()`, `process_block_commit_tip_ext()`, and the test wrappers,
  leaving `process_block_core.c` focused on chain selection and active-tip
  child discovery. `process_block_core.c` is down from 776 to 486 lines.
- Active-tip child discovery and disk verification moved from
  `process_block_core.c` into `process_block_tip_child.c`. The new file owns
  `process_block_verify_active_tip_child_on_disk()`,
  `find_best_active_tip_child()`, and
  `find_verified_unlinked_active_tip_child()`, leaving
  `process_block_core.c` focused on chain selection and contextual-header
  skip logic. Stale monolith includes were also pruned, and
  `process_block_core.c` is down from 486 to 247 lines.
- Contextual-header skip policy moved from `process_block_core.c` into
  `process_block_contextual_header.c`. The new file owns
  `process_block_should_skip_contextual_header()` plus its sparse
  retarget/MTP-window helper, leaving `process_block_core.c` focused on
  best-work chain selection. `process_block_core.c` is down from 247 to 177
  lines.
- The snapshot-sync router contract now lives in
  `lib/net/include/net/snapshot_sync_contract.h`, with
  `app/services/include/services/snapshot_sync_service.h` kept as a
  compatibility wrapper for app callers. `lib/net/src/msgprocessor_snapshot.c`
  now includes the lib-owned contract instead of the app service header, and
  the lib-to-app include baseline is empty.
- Read-only chain diagnostics no longer flush consensus state as a side
  effect. `getdataintegrity`, `gethodlwave`, and `gethodlwaveimage` scan the
  persisted read models instead of calling `coins_view_cache_flush()`, dropping
  the E6 one-write-path baseline from 24 to 21 write surfaces.
- The redundant `coins_view_sqlite_batch_write()` compatibility wrapper was
  deleted. Vtable, reducer-stage, and test callers now use
  `coins_view_sqlite_batch_write_ex(..., NULL)` when no path commitment write
  is needed, leaving one SQLite coins flush entry point and dropping the E6
  one-write-path baseline from 21 to 17 write surfaces.
- The `active_chain_set_tip()` compatibility alias was deleted, and tests now
  call `active_chain_move_window_tip()` directly when they need to seed the
  in-memory active-chain cache. Production process-block flushing now refuses
  to fall back to generic `coins_view_cache_flush()` when the reducer SQLite
  writer was not installed; only `ZCL_TESTING` keeps that fallback for harness
  setup. The E6 one-write-path baseline is down from 17 to 16 write surfaces.
- Boot reindex no longer calls `coins_view_cache_flush()` directly.
  `reindex_chainstate()` now routes start, periodic, and final UTXO flushes
  through a local helper that calls `coins_view_sqlite_batch_write_ex()` and
  clears the cache only after a successful durable write. The E6 one-write-path
  baseline is down from 16 to 13 write surfaces.
- Shutdown no longer calls `coins_view_cache_flush()` directly. Emergency,
  network-quiesce, and final shutdown UTXO flushes now route through a local
  helper that calls `coins_view_sqlite_batch_write_ex()` and clears the cache
  only after a successful durable write. The E6 one-write-path baseline is
  down from 13 to 10 write surfaces.
- Runtime `reindexchainstate` no longer owns a second replay writer. The RPC
  is now an explicit retired-operation compatibility error that directs
  operators to restart with `-reindex-chainstate`, whose boot path already uses
  the reducer/boot SQLite writer. `repairutxos` no longer flushes the
  projection-backed coins cache; repaired UTXOs persist through the UTXO model
  and the cache remains a process-local read cache. The E6 one-write-path
  baseline is down from 10 to 5 write surfaces.
- `coins_view_cache_flush()` is no longer a production-visible API. The
  remaining child-cache flush behavior is renamed
  `coins_view_cache_flush_for_testing()` behind `ZCL_TESTING`, and production
  comments now refer to the flush policy rather than the old generic cache
  flush entry point. The E6 one-write-path baseline is down from 5 to 3 write
  surfaces.
- The E6 one-write-path baseline is empty. The canonical
  `coins_view_sqlite_batch_write_ex()` contract/implementation and the
  process-block flush-policy call are explicitly tagged as destination writer
  surfaces, alongside the already-tagged reducer, boot-reindex, shutdown, and
  vtable-adapter surfaces. Untagged new chain-state writers now fail the
  ratchet without grandfathering.
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
- Test-only small-projection comparison helpers were removed from the
  production `lib/storage` API. The contacts, onion-announcement, and HODL
  projection parity check now lives in `test_small_projections`, which compares
  the projection SQLite files directly against the legacy fixture database.

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

### E6 One-Write-Path Guardrail

`tools/scripts/one_write_path_baseline.txt` is empty. Canonical reducer, boot,
shutdown, vtable-adapter, process-block flush-policy, and SQLite writer
surfaces carry inline `one-write-path-ok:<tag>` markers. Any untagged new
chain-state writer fails the ratchet.

The final form remains one durable writer and one cursor authority. Current
guardrail: low-level active-chain cache/window moves and stale `tip_finalize`
cursors cannot regress the public reducer tip.

### Supervisor Debt

`tools/scripts/supervisor_baseline.txt` is empty. Every long-running service
that gate tracks now registers a supervisor liveness contract.

### Typed Blocker Debt

`tools/scripts/typed_blocker_baseline.txt` is empty. Raw blocker string fields
and legacy blocker setters are not grandfathered; keep this gate at zero.

### Controller And Layering Debt

- Lib-layering debt is at zero grandfathered entries. Keep
  `tools/scripts/lib_layering_baseline.txt` empty.
- Controller raw-SQL debt is at zero grandfathered files. Keep
  `tools/lint/no_raw_sqlite_in_controllers_baseline.txt` empty.
- `lib/validation/src/process_block_core.c` now owns best-work chain
  selection only.
- `lib/validation/src/process_block_self_heal.c` is the next obvious
  process-block split candidate: it still combines missing-UTXO recovery
  sources, legacy-RPC parsing, scan counters, and hot-loop pause signaling.

## Next Work Order

1. Delete zero-purpose scaffolding and stale build references.
2. Keep production terminology clean; normalize remaining historical test/doc
   fixture names only when touched for adjacent work.
3. Keep import/catchup/legacy-import code below the file-size ceiling while
   moving remaining mixed-purpose code toward the correct framework shape.
4. Split remaining mixed-purpose process-block files by responsibility
   (`process_block_self_heal.c` is the next obvious target).
5. Keep every lint baseline empty while continuing process-block and
   mixed-purpose file cleanup.
6. Run `make lint`, rebuild `test_parallel`, run the suite, then prove live
   node progress with a soak.

## Latest Verification

- `git diff --check`: pass after splitting contextual-header skip policy out
  of `process_block_core.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make lint`: pass after the contextual-header split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_contextual_header.c`.
- `./test_parallel --timeout=180`: pass after the contextual-header split,
  `0/279` groups failed in 56.0s. This includes the
  `skip_contextual:*` chain tests and the process-block split guard in
  `test_make_lint_gates`.
- Quick live sample attempt at 2026-06-01 13:06:19 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after emptying the E6 baseline.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_one_write_path.sh`: pass with 0 grandfathered write
  surfaces and no new violations.
- All tracked lint baselines/allowlists are empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after emptying the E6 baseline and tagging the
  canonical writer surfaces.
- `make lint`: pass after emptying the E6 baseline; E1, E2, E6, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, lib-layering, and
  raw-malloc gates all report zero grandfathered entries.
- `./test_parallel --timeout=180`: pass after emptying the E6 baseline,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:56:06 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after making the cache flush helper test-only.
- `git diff --check`: pass.
- `tools/scripts/check_one_write_path.sh`: pass with 3 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=coins --timeout=120 --verbose`,
  `./test_parallel --only=chain_stall_repro --timeout=120 --verbose`,
  `./test_parallel --only=consensus_compat --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the test-only cache flush shrink; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  3 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after the test-only cache flush shrink,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:49:34 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after retiring runtime `reindexchainstate` replay
  and removing the `repairutxos` coins-cache flushes.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 5 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `ZCL_TEST_ONLY=rpc_safety ./test_zcl`,
  `./test_parallel --only=rpc --timeout=120 --verbose`,
  `./test_parallel --only=chain --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the runtime reindex/repair flush shrink; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  5 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after the runtime
  reindex/repair-flush shrink, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:41:35 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing shutdown UTXO flushes through the
  SQLite coins writer.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 10 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=boot --timeout=120 --verbose`,
  `./test_parallel --only=shutdown --timeout=120 --verbose`,
  `./test_parallel --only=coins --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the shutdown flush routing; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, lib-layering, and
  raw-malloc gates remain at zero active debt, while E6 is 10 grandfathered
  write surfaces.
- `./test_parallel --timeout=180`: pass after the shutdown flush routing,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:28:33 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing boot reindex UTXO flushes through the
  SQLite coins writer.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 13 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=boot --timeout=120 --verbose`,
  `./test_parallel --only=block_index --timeout=120 --verbose`,
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`,
  `./test_parallel --only=coins --timeout=120 --verbose`, and
  `./test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the boot reindex flush routing; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  13 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after the boot reindex flush routing,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:20:19 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the `active_chain_set_tip()`
  compatibility alias and making the process-block `coins_view_cache_flush()`
  fallback test-only.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 16 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=chain --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`,
  `./test_parallel --only=tip_finalize --timeout=120 --verbose`,
  `./test_parallel --only=header_admit --timeout=120 --verbose`,
  `./test_parallel --only=utxo_activation --timeout=120 --verbose`, and
  `./test_parallel --only=reducer_stage --timeout=120 --verbose`.
- `make lint`: pass after the active-chain alias deletion and process-block
  fallback tightening; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, lib-layering, and raw-malloc gates
  remain at zero active debt, while E6 is 16 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after the active-chain alias deletion
  and process-block fallback tightening, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:11:24 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the redundant
  `coins_view_sqlite_batch_write()` wrapper and routing callers through
  `coins_view_sqlite_batch_write_ex(..., NULL)`.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 17 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=coins --timeout=120 --verbose` and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the coins SQLite wrapper deletion; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  17 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after the coins SQLite wrapper
  deletion, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:02:56 UTC after the coins SQLite
  wrapper deletion did not prove live-node health: no `zclassic23` process was
  running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, `18232`, or `8232`
  listener, `systemctl --user status zclassic23` could not connect to the
  user bus, and the recent read-only journal checks had no entries. The
  service was not restarted; this slice stayed read-only and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after moving active-tip child discovery and disk
  verification into `process_block_tip_child.c`.
- `make -j$(nproc) test_parallel`: pass after rebuilding the parallel runner
  for the new process-block tip-child boundary assertions.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 21 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=chain --timeout=120 --verbose`,
  `./test_parallel --only=torn_index --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`, and
  `./test_parallel --only=block_scan --timeout=120 --verbose`.
- `make lint`: pass after the active-tip child split; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  21 grandfathered write surfaces.
- `./test_parallel --timeout=180`: pass after rebuilding `test_parallel`,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 11:50:21 UTC after the
  active-tip child split did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus,
  and the recent read-only journal checks had no entries. The service was not
  restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting header activation,
  block-file scan, height-repair state, and post-activation anchor repair
  through the message processor.
- `tools/scripts/check_lib_layering.sh`: pass with 12 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c`
  `coins_view_cache_flush()` baseline line numbers shifted after adding the
  boot-owned callbacks.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=header_sync --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=block_index_integrity --timeout=120 --verbose`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `make lint`: pass after the header activation/index callback injection; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 12 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the header activation/index
  callback injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:49:59 UTC after the header
  activation/index callback injection did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` exited 7 for both
  `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or
  `18232` listener, the previous 20 minutes of `zclassic23.service` journal
  had no entries, and the broader read-only journal scan still shows the
  earlier OOM kill at 2026-06-01 07:58:58 UTC. The service was not restarted;
  this slice stayed read-only for live checks and preserved the `8023` port
  expectation.
- `make -j$(nproc)`: pass after callback-injecting header-sync snapshot
  active/anchor access through the message processor.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  header snapshot callback lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 14 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c`
  `coins_view_cache_flush()` baseline line numbers shifted after adding the
  boot-owned callbacks.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=header_sync --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=snapshot_sync_service --timeout=120 --verbose`.
- `make lint`: pass after the header snapshot callback injection; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 14 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the header snapshot callback
  injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:38:28 UTC after the header
  snapshot callback injection did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener, the
  previous 20 minutes of `zclassic23.service` journal had no entries, and the
  broader read-only journal scan still shows the earlier OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting fast-sync snapshot
  serialization from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded fast-sync layering lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 15 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c` baseline
  line numbers shifted after adding the boot-owned serializer callback.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=fast_sync --timeout=120 --verbose`,
  `./test_parallel --only=snapshot_sync_service --timeout=120 --verbose`, and
  `./test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the fast-sync serializer callback injection; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 15 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the fast-sync serializer
  callback injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:27:00 UTC after the fast-sync
  serializer callback injection did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` exited 7 for both
  `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or
  `18232` listener, the previous 20 minutes of `zclassic23.service` journal
  had no entries, and the broader read-only journal scan still shows the
  earlier OOM kill at 2026-06-01 07:58:58 UTC. The service was not restarted;
  this slice stayed read-only for live checks and preserved the `8023` port
  expectation.
- `make -j$(nproc)`: pass after removing fast-sync's direct ActiveRecord and
  DB model includes.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  fast-sync SQLite helper lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 16 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=fast_sync --timeout=120 --verbose`, and
  `./test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the fast-sync AR/DB include removal; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 16 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the fast-sync AR/DB include
  removal, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:18:00 UTC after the fast-sync
  AR/DB include removal did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener, the
  previous 20 minutes of `zclassic23.service` journal had no entries, and the
  broader read-only journal scan still shows the earlier OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block self-heal tx-index
  lookup through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded self-heal runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 18 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=self_heal --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`, and
  `./test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the self-heal tx-index/runtime boundary move; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 18 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the self-heal tx-index/runtime
  boundary move, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 09:07:58 UTC after the
  self-heal tx-index/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`,
  or `18232` listener, and a read-only journal scan still shows the earlier
  `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC. The service was
  not restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block self-heal durable UTXO
  max-height reads through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded process-block runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 19 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=self_heal --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`, and
  `./test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the self-heal/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 19 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the self-heal/runtime boundary
  move, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:58:37 UTC after the
  self-heal/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`,
  or `18232` listener, and a read-only journal scan still shows the earlier
  `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC. The service was
  not restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block flush-policy DB state,
  sync-batch, and WAL checkpoint operations through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded process-block runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 20 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `process_block_flush_policy.c`
  baseline line numbers shifted after removing the DB model include.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`,
  `./test_parallel --only=wallet_flush_rollback --timeout=120 --verbose`, and
  `./test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the flush-policy/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 20 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the flush-policy/runtime
  boundary move, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 08:47:50 UTC after the
  flush-policy/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `zclassic23`,
  `8023`, `8033`, or `18232` listener, and a read-only journal scan still
  shows the earlier `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC.
  The service was not restarted; this slice stayed read-only for live checks
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block `node_db` open checks
  through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  process-block/runtime lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 21 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=validation --timeout=120 --verbose`, and
  `./test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the process-block/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 21 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the process-block/runtime
  boundary move, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:39:09 UTC after the
  process-block/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `zclassic23`,
  `8023`, `8033`, or `18232` listener, and a read-only journal scan still
  shows the earlier `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC.
  The service was not restarted; this slice stayed read-only for live checks
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting quorum-oracle peer header
  votes from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  header-vote callback lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 22 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c` baseline line
  numbers shifted with the new boot-owned callback.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=header_sync --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=sync_service --timeout=120 --verbose`, and
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`.
- `make lint`: pass after the header-vote callback injection; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 22 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the header-vote callback
  injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:29:36 UTC after the header-vote
  callback injection did not prove live-node health: no `zclassic23` process
  was running, `zcl-rpc` returned connection failure, `ss` showed no
  `zclassic23`, `8023`, `8033`, or `18232` listener, and a read-only journal
  scan still shows the earlier `zclassic23.service` OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after isolating the `msg_headers.c` chain-tip
  test fallback from the app service header.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 23 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=header_sync --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=sync_service --timeout=120 --verbose`.
- `make lint`: pass after the chain-tip fallback isolation; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 23 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the chain-tip fallback
  isolation, `0/279` groups failed in 61.1s.
- Quick live sample attempt at 2026-06-01 08:20:54 UTC after the chain-tip
  fallback isolation did not prove live-node health: no `zclassic23` process
  was running, `zcl-rpc` returned connection failure, `ss` showed no
  `zclassic23`, `8023`, `8033`, or `18232` listener, and a read-only journal
  sample still reported `zclassic23.service` was OOM-killed at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after moving the sync planner API contract into
  `lib/sync/include/sync/sync_planner.h`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate ownership assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 24 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=header_sync --timeout=120 --verbose`,
  `./test_parallel --only=sync_service --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=integrity --timeout=120 --verbose`.
- `make lint`: pass after the sync planner contract move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 24 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the sync planner contract move,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:10:13 UTC after the sync planner
  contract move did not prove live-node health: `systemctl --user is-active
  zclassic23` could not connect to the user bus, `zcl-rpc` returned connection
  failure, `ss` showed no `zclassic23`, `8023`, `8033`, or `18232` listener,
  and a read-only journal sample reported `zclassic23.service` was OOM-killed
  at 2026-06-01 07:58:58 UTC. The service was not restarted because this slice
  was constrained to read-only live checks.
- `make -j$(nproc)`: pass after moving snapshot-sync accessors into
  `lib/net/src/msgprocessor_snapshot.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate ownership assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 28 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=snapshot_sync_service --timeout=120 --verbose`, and
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`.
- `make lint`: pass after the snapshot-sync accessor move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 28 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the snapshot-sync accessor move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:54:10 UTC after the snapshot-sync accessor
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting ZMSG, file-offer, and
  file-service P2P persistence from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  P2P app-persistence callback lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 29 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=file_market --timeout=120 --verbose`, and
  `./test_parallel --only=models --timeout=120 --verbose`.
- `make lint`: pass after the P2P app-persistence callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 29 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the P2P app-persistence callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:44:34 UTC after the P2P app-persistence callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after moving block-sync planning for invalid-block
  retries and valid-block acceptance behind net-internal message-processor
  helpers, and removing the final app-service include from
  `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 39 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=sync_service --timeout=120 --verbose` (2 matched
  groups).
- `make lint`: pass after the block-sync planning helper move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 39 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the block-sync planning helper
  move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:06:19 UTC after the block-sync planning
  helper move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting the block-connected
  observer from boot and removing the sync monitor service include from
  `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 40 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=chain_activation_controller --timeout=120 --verbose`.
- `make lint`: pass after the block-connected observer callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 40 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the block-connected observer
  callback move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 06:54:50 UTC after the block-connected
  observer callback move: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting P2P block reducer
  submission from boot and removing app controller/model/activation/snapshot
  includes from `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 41 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=chain_activation_controller --timeout=120 --verbose`.
- `make lint`: pass after the P2P block callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 41 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the P2P block callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 06:43:25 UTC after the P2P block callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting tx wallet persistence and
  the snapshot-active query from boot, removing all app controller/model/service
  includes from `lib/net/src/msg_tx.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 46 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callbacks.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`, and
  `./test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the tx callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 46 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the tx callback move,
  `0/279` groups failed in 85.1s.
- Quick live sample at 2026-06-01 06:32:22 UTC after the tx callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting metrics service/model
  gauges and connman known-ZCL23 peer selection from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  metrics/connman callback boundary and lint-gate assertion update.
- `tools/scripts/check_lib_layering.sh`: pass with 50 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the boot-owned callbacks.
- Focused filtered tests passed:
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=mcp_metrics --timeout=120 --verbose`.
- `make lint`: pass after the metrics/connman callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 50 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the metrics/connman callback
  move, `0/279` groups failed in 87.1s.
- Quick live sample at 2026-06-01 06:19:03 UTC after the metrics/connman
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting handshake peer persistence
  from boot, removing `msg_version.c`'s Peer model/database includes, and
  logging the boot-owned peer-save false-return path.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  peer-save callback move and the lint-gate assertion update.
- `tools/scripts/check_lib_layering.sh`: pass with 55 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the boot-owned peer-save helper.
- Focused filtered tests passed:
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=peer_lifecycle --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the peer-save callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 55 grandfathered includes.
- `./test_parallel --timeout=180`: pass after the peer-save callback move,
  `0/279` groups failed in 78.1s.
- Quick live sample at 2026-06-01 06:05:55 UTC after the peer-save callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting compact-block reducer
  submission from boot and removing `msg_compact.c`'s activation-service
  include.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  compact-block callback move.
- `make lint`: pass after the compact-block callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 57 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 57 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the callback helper.
- Focused filtered tests passed:
  `./test_parallel --only=compact_blocks --timeout=120 --verbose`,
  `./test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the compact-block callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 05:49:27 UTC after the compact-block
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after removing the test-only small-projection
  comparison helpers from the production storage API.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  local projection-table comparison test move.
- `make lint`: pass after the small-projection production-surface cleanup; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 58 grandfathered includes.
- `./test_parallel --only=small_projections --timeout=120 --verbose`: pass
  with the parity checks comparing projection SQLite files directly against
  the legacy fixture DB.
- `./test_parallel --timeout=180`: pass after the small-projection production
  API cleanup, `0/279` groups failed in 77.1s.
- Production stale-surface search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]'`
  now returns only generated wallet CSS `box-shadow` matches under
  `app/views`, not reducer/cutover/shadow code.
- Quick live sample at 2026-06-01 05:34:31 UTC after the small-projection
  cleanup: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting onion-service app handlers
  from boot, removing `onion_service.c`'s blog-controller include, and adding
  direct `/blog` route coverage for the injected handler. `make test_parallel`
  was then run explicitly to rebuild the parallel runner.
- `make lint`: pass after the onion-service callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 58 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 58 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=blog --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the onion-service callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 05:23:49 UTC after the onion-service
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting connman onion peer discovery
  from boot and removing `connman.c`'s blog-controller include.
- `make lint`: pass after the connman callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 59 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 59 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=net --timeout=120 --verbose`,
  `./test_parallel --only=blog --timeout=120 --verbose`,
  `./test_parallel --only=connman_addnode_fallback --timeout=120 --verbose`,
  and `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the connman callback move,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 05:09:42 UTC after the connman callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving mined-block submission behind a
  caller-owned `gen_context` callback and removing the mining library's app
  activation-service include.
- `make lint`: pass after the mining callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 60 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 60 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `./test_parallel --only=mining --timeout=120 --verbose` and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the mining callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:59:05 UTC after the mining callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving addrman sidecar integrity to `lib/net`
  and generic SHA3 sidecar I/O to `lib/storage`.
- `tools/scripts/check_lib_layering.sh`: pass with 61 grandfathered
  lib-to-app includes and no new violations.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved sidecar sources.
- Focused filtered tests passed:
  `./test_parallel --only=addrman_integrity --timeout=120 --verbose`,
  `./test_parallel --only=block_index_integrity --timeout=120 --verbose`,
  `./test_parallel --only=block_index_sidecar_port --timeout=120 --verbose`,
  and `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the sidecar ownership move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:47:38 UTC after the sidecar ownership
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving swap-contract persisted records and
  SQLite persistence into the SwapContract model shape.
- `make lint`: pass after the swap persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 62 grandfathered includes.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved SwapContract model persistence sources.
- Focused filtered tests passed:
  `./test_parallel --only=htlc --timeout=120 --verbose`,
  `./test_parallel --only=protocols --timeout=120 --verbose`,
  `./test_parallel --only=models --timeout=120 --verbose`,
  `./test_parallel --only=db_validators --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the swap persistence move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:38:22 UTC after the swap persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving ZNAM at-rest records and SQLite
  persistence into the Znam model shape.
- `make lint`: pass after the ZNAM persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 65 grandfathered includes.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved ZNAM model persistence sources.
- Focused filtered tests passed:
  `./test_parallel --only=znam --timeout=120 --verbose`,
  `./test_parallel --only=protocols --timeout=120 --verbose`,
  `./test_parallel --only=models --timeout=120 --verbose`,
  `./test_parallel --only=db_validators --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the ZNAM persistence move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:28:58 UTC after the ZNAM persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan since
  2026-06-01 04:22:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH`
  signal. This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving ZMSG SQLite persistence into the Zmsg
  model shape.
- `make lint`: pass after the ZMSG persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 68 grandfathered includes.
- Focused filtered tests passed:
  `./test_parallel --only=protocols --timeout=120 --verbose`,
  `./test_parallel --only=models --timeout=120 --verbose`,
  `./test_parallel --only=db_validators --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the ZMSG persistence move,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 04:16:14 UTC after the ZMSG persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1353559`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan since
  2026-06-01 04:10:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH`
  signal. This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving schema migration and file-offer
  persistence into the Model shape.
- `make lint`: pass after moving schema migration into `app/models` and
  file-offer SQLite persistence into the FileOffer model; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 71 grandfathered includes.
- `make test_parallel`: pass after rebuilding the suite binary with the moved
  schema migration and FileOffer model persistence sources.
- Focused filtered tests passed:
  `./test_parallel --only=schema_migration --timeout=120 --verbose`,
  `./test_parallel --only=file_market --timeout=120 --verbose`, and
  `./test_parallel --only=models --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the Model-shape persistence
  move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:04:04 UTC after the Model-shape
  persistence move: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`,
  `gettxoutsetinfo.height=3130701`, `txouts=1353559`, RPC listened on
  `127.0.0.1:18232`, P2P listened on `0.0.0.0:8023` / `[::]:8023`, and a
  journal scan since 2026-06-01 03:58:00 UTC found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after moving `zcl_node_db_path()` into `lib/util`,
  moving UTXO script classification into `lib/script`, removing the
  storage-layer UTXO model include, and replacing the net internal service
  include with a forward declaration.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, and raw-malloc gates remain at zero
  active debt, E6 is 24 grandfathered write surfaces, and lib-layering is 75
  grandfathered includes.
- `make test_parallel`: pass after adding direct coverage for
  `zcl_node_db_path()` and `utxo_classify_script()`.
- Focused filtered tests passed:
  `./test_parallel --only=path_check --timeout=120 --verbose`,
  `./test_parallel --only=script --timeout=120 --verbose`,
  `./test_parallel --only=coins_view --timeout=120 --verbose`,
  `./test_parallel --only=fast_sync --timeout=120 --verbose`,
  `./test_parallel --only=tor --timeout=120 --verbose`,
  `./test_parallel --only=net --timeout=120 --verbose`, and
  `./test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `./test_parallel --timeout=180`: pass after the lib-layering shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 03:47:30 UTC after the lib-layering shrink:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 03:31:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, or `DB_ERR_TIP_MISMATCH` signal. This is a
  continuity check, not the final soak.
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
