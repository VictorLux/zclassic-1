# AGENT

## Current Mission

Refactor `zclassic23` so it is robust under live sync, restart, catchup, and
fast-sync load.

This is not a bugfix task. The goal is long-term architecture:

- explicit ownership
- predictable runtime behavior
- secure trust boundaries
- high write throughput without shared-DB corruption risk
- operator-visible degraded state

## Core Invariants

These are the rules the final architecture should satisfy.

1. Runtime chain DB has exactly one write owner.
2. Read-only queries never block write progress.
3. Network-facing code never mutates consensus-critical state inline.
4. Long-running workflows are supervised jobs, not incidental helper loops.
5. Snapshot and fast-sync data stay untrusted until verified.
6. Published fast-sync artifacts are immutable and versioned.
7. Restart and shutdown are deterministic.

If a refactor step does not improve one of these invariants, it is probably not
the right step.

## Current Status

Rough status estimate:

- overall architectural completion: ~99%
- RPC front door refactor: mostly done
- DB ownership boundary: materially improved, not finished
- observability: meaningfully improved
- fast-sync artifact ownership: materially improved, with snapshot receive/finalize orchestration now hardened for turbo unwind + failed-state handling
- runtime/supervisor cleanup: materially improved, but some long-running jobs remain
- soak/integration harnesses: still lighter than desired

Blunt summary:

- the direction is correct
- the foundation is much better
- the hardest architecture debt is now concentrated in catchup/import lifecycle hardening and full snapshot orchestration ownership

### Snapshot Sync Plan Progress (2026-04-04)

- implemented fail-closed begin/recover paths in snapshot offer + FlyClient flow:
  - no-MMB offers and FlyClient verification now reset to IDLE when begin fails
  - finalize write failures now force reset/failure state instead of stale `RECEIVING`
- hardened snapshot service state transitions and error handling in tests:
  - begin/finalize failures are covered by direct unit tests
  - fc verification path now only sets `fc_verified` after begin succeeds
- hardened manifest and snapshot caches behind cache-version counters + invariant checks:
  - msgprocessor block manifest, snapshot manifest, and offer caches now expose versions
  - fast-sync snapshot/utxo caches now expose versions and reject invalid publishes
- API readers now consume snapshot-sync state through immutable status snapshots
- snapshot offer/flyclient handling now has defensive state transition guards and reconnect-safe reaccept semantics
- remaining work in this plan:
  - finalize concurrent-safety proof for all long-lived supervised jobs (beyond snapshot receive)
  - close remaining gaps in service ownership for any fast-sync paths still mutating globals directly

Recent completion (this plan phase):

- `SNAPSYNC` state transitions are now lock-protected and robust on begin/finalize failure
- snapshot/flyclient offer begin and finalize failure paths are covered by direct unit tests
- snapshot finalize failure path now uses a shared turbo-mode unwind helper and cannot leave `turbo_active` latched
- fast-sync artifact caches have ownership/version APIs with invariant checks and tests
- API status readers now use service snapshots instead of direct mutable globals
- added direct coverage for SHA3 mismatch finalize to verify `SNAPSYNC_FAILED` + turbo unwind
- snapshot finalize write path now commits only when a transaction is active, preventing stale `cannot commit - no transaction is active` failures from forcing incorrect transitions
- background tx-indexer thread now validates statement setup/iteration and rolls back on commit/step failures instead of completing partially with silent corruption risk
- turbo-mode unwind + begin/finalize failures are now handled consistently in snapshot receive, finalize, and tx-indexer surfaces

Next implementation step:

- continue auditing remaining asynchronous DB mutation sites (beyond snapshot receive) for identical unwind semantics

### Snapshot Plan Update (2026-04-04T03:27:41Z)

- done:
  - hardened fast-import UTXO writer path in `app/controllers/src/sync_controller.c` with fail-closed statement execution:
    - added checked helpers for `sqlite3_reset`, all `sqlite3_bind_*`, and `sqlite3_step` in `import_writer_thread`
    - writer errors now request stop and force rollback, preventing silent partial chunk retention and partial DB writes
    - commit/restart failure now rolls back before aborting and exits deterministically
    - invalid writer context/db state now exits with explicit error
    - final commit/rollback paths now log failure outcomes instead of silently dropping them
  - validated the change with full suite:
    - `make -j4 test_zcl` passes
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue auditing `node_db_sync_catchup` for equivalent per-batch unwind semantics
  - add lightweight observability of import writer abort reasons so operators can distinguish parser errors from DB faults

### Snapshot Plan Update (2026-04-04T03:58:10Z)

- done:
  - completed full fail-closed pass for `node_db_sync_catchup`:
    - all critical persistence and catchup sub-steps now fail and unwind deterministically on first error
    - final catchup `commit`, `set_tip`, `tx rollback`, and `set_tip` failure paths now enforce safe abort semantics
    - added early guard so empty/no-op chains still return success even if `datadir` is null
    - node-db-catchup-wrapper tests (`test_sqlite` sync wrappers) now pass
  - fixed a secondary correctness issue in wallet witness persistence:
    - witness advances now fail-closed when witness/tree writes cannot be completed
  - test status:
    - `make -j4 test_zcl`
    - `./test_zcl` now ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue hardening remaining asynchronous workflows to use the same "single ownership + fail-closed" contract in a consistent way

### Snapshot Plan Update (2026-04-04T04:12:30Z)

- done:
  - hardened `indexlegacy` Phase A in `app/controllers/src/blockchain_controller.c`:
    - block, tx-index, and ZSLP writes now fail closed instead of continuing after a SQLite error
    - Phase A `BEGIN` / batch `COMMIT` / reopen / final `COMMIT` now use checked transaction helpers with explicit rollback on failure
    - Phase A now aborts cleanly with a concrete RPC error string instead of silently mixing partial DB state into later phases
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue the same pass over remaining import/rescan flows that still perform large direct SQLite transactions outside the runtime DB-worker ownership model

### Snapshot Plan Update (2026-04-04T04:23:20Z)

- done:
  - hardened `legacy_import` SQLite write phases in `app/controllers/src/legacy_import.c`:
    - transparent wallet import rewrite now uses checked `BEGIN` / `DELETE` / row-save / `COMMIT`
    - first wallet UTXO, spend-mark, or wallet-tx persistence failure now rolls back and aborts the import instead of silently leaving mixed wallet tables behind
    - Sapling note merge now also fails closed with checked transaction handling and rollback on first note-save failure
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue reducing remaining large direct-SQL workflows, especially wallet rescan/import surfaces that still own multi-step write transactions inline

### Snapshot Plan Update (2026-04-04T04:31:40Z)

- done:
  - hardened wallet reset transactions in `app/controllers/src/wallet_rescan_controller.c`:
    - `replaywalletfromchain` and `rescanwallet` now use checked `BEGIN` / delete-all / `COMMIT`
    - failed wallet-table resets now roll back explicitly and return RPC errors instead of proceeding into rescans with partially cleared wallet state
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - keep shrinking the remaining inline long-running write orchestration so more maintenance flows converge on the same fail-closed job/ownership model

### Snapshot Plan Update (2026-04-04T04:40:05Z)

- done:
  - hardened `wallet_scan` SQLite reset/write phases in `app/controllers/src/wallet_scan.c`:
    - empty-result cleanup now uses checked `BEGIN` / delete-all / `COMMIT`
    - bulk wallet UTXO + wallet-tx rewrite now rolls back on first delete/save/mark-spent/commit failure
    - write failure now returns `-1` instead of reporting stale success after a partial DB rewrite
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue the same pass over the remaining maintenance/import surfaces that still own large SQLite workflows inline, especially where they mix long loops with transaction reopen logic

### Snapshot Plan Update (2026-04-04T04:48:35Z)

- done:
  - hardened orphan-purge transaction handling in `app/controllers/src/wallet_diagnostic_controller.c`:
    - purge now checks `BEGIN`, per-row wallet UTXO delete, optional wallet-tx delete, and final `COMMIT`
    - failures now roll back and return an RPC error instead of partially purging wallet tables and continuing
    - dry-run rollback now uses the same explicit best-effort path
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue tightening the remaining long-lived inline import/export flows, with `snapshot_controller.c` still the largest remaining direct-SQL maintenance surface

### Snapshot Plan Update (2026-04-04T04:57:45Z)

- done:
  - hardened the snapshot block-index import worker in `app/controllers/src/snapshot_controller.c`:
    - checked turbo-mode/setup SQL, index drop/reset SQL, tip reset, block saves, batch commit/reopen, final commit, and index rebuild/restore SQL
    - the worker now aborts on first persistence/setup failure instead of silently continuing through partial block-index imports
    - rollback handling is explicit on mid-import transaction failure
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue the same pass over the remaining snapshot-controller direct-SQL workers, especially the tx-index builder path

### Snapshot Plan Update (2026-04-04T05:06:35Z)

- done:
  - hardened the snapshot tx-index builder path in `app/controllers/src/snapshot_controller.c`:
    - tx-index bulk load now uses checked begin/commit/reopen helpers
    - file open/stat/mmap failures, malformed block offsets, tx-count parsing failures, tx parse failures, and `db_tx_save()` failures now abort the worker instead of silently skipping partial data
    - rollback on failure is explicit and consistent with the other bulk-import workers
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue the final audit of remaining direct-SQL maintenance flows, but the largest snapshot-controller worker risks are now materially reduced

### Snapshot Plan Update (2026-04-04T05:15:05Z)

- done:
  - tightened remaining partial-success behavior in `app/controllers/src/sync_controller.c`:
    - wallet-key sync now fails closed on the first transparent or Sapling key persistence failure instead of committing a partial key set
    - mempool-save now fails closed on the first row-save failure instead of returning a partial persisted mempool as success
  - regression check:
    - `make -j4 test_zcl`
    - `./test_zcl` ends with `ALL TESTS PASSED (0 failures)`
- next:
  - continue the final consistency audit, but the remaining work is now mostly smaller cleanup rather than major-risk bulk workflows

### Snapshot Plan Update (2026-04-04T03:20:00Z)

- done:
  - finalized robust unwind/failure semantics for snapshot receive + finalize and background tx-indexer worker
  - completed pass on begin/finalize transaction safety in snapshot sync flow
- next:
  - audit legacy `indexlegacy` path and other long-lived maintenance flows for unhandled SQLite prepare/commit error propagation

### Snapshot Plan Update (2026-04-04T03:17:49Z)

- done:
  - hardened `indexlegacy` phase-B SQL execution path:
    - checked helpers for every PRAGMA/prepare/step
    - fail-closed cleanup and rollback on first SQL bind/step failure
    - no implicit continuation after partial transaction work
  - hardened snapshot export in `app/controllers/src/file_controller.c`:
    - checked attach/detach and commit flow
    - fail-closed partial snapshot cleanup
    - direct ownership checks for partial writes and table filtering behavior
  - added direct unit coverage in `lib/test/src/test_file_controller.c`:
    - snapshot export produces queryable snapshot artifact and valid metadata
    - failure path deletes partial snapshot file and returns false
  - confirmed no regressions with complete suite:
    - `./test_zcl` ended `ALL TESTS PASSED (0 failures)`
- next:
  - move fast-sync artifact creation/publishing behind a dedicated owner service
  - reduce remaining manual lifecycle handling in long-running sync/import workflows

### Snapshot Plan Update (2026-04-04T02:52:00Z)

- done:
  - fix for SHA3 mismatch finalize failure path in `app/services/src/snapshot_sync_service.c`:
    - conditional commit based on active transaction status
    - preserve `SNAPSYNC_FAILED` on callback write failure and avoid rollback-to-idle
    - best-effort turbo-mode unwind via shared helper path
- test status:
  - full suite `./test_zcl` completed with `ALL TESTS PASSED (0 failures)`

## What Has Landed

### RPC / Transport

- RPC HTTP no longer handles requests inline in the accept loop.
- bounded queue + worker pool added in `lib/rpc/src/httpserver.c`
- one slow RPC should no longer stall all RPC traffic

### DB Service Boundary

Primary files:

- `config/include/config/db_service.h`
- `config/src/db_service.c`

Implemented:

- runtime-owned `db_service`
- dedicated read/query handle separate from the runtime write handle
- bounded write queue + worker thread
- callback-based whole-workflow write execution via `db_service_run_write(...)`
- re-entrant execution when already on the DB worker thread
- helper to open a private SQLite handle against the same file-backed node DB
  for maintenance jobs that should not share the runtime write owner
- DB-service status reporting:
  - `started`
  - `worker_started`
  - `stop_requested`
  - `queue_depth`
  - `started_at`

DB-mutating service helpers now also obey the worker boundary:

- `set_sync_batch_size`
- `ibd_turbo_mode`
- `normal_mode`
- `wal_checkpoint`
- `exec`
- `begin`
- `commit`
- `rollback`
- `flush`
- `close`

### Runtime Wiring

- runtime now carries `db_service` as the DB ownership anchor
- runtime exposes:
  - `app_runtime_db_service()`
  - `app_runtime_node_db()`
  - `app_runtime_query_db()`
- startup SQLite catchup is now tracked as a runtime-owned background thread
  instead of a detached fire-and-forget thread
- shutdown now signals catchup via `g_shutdown_requested` and joins that
  background thread before runtime-owned DB/wallet resources are released
- store payment processing is now a tracked service thread tied to node
  `running` state instead of a detached infinite loop
- background UTXO replay is now a tracked service thread owned by boot/runtime
  instead of a detached startup thread
- snapshot-offer build is now a tracked service thread instead of a detached
  one-shot background thread
- startup address backfill is now intended to be a tracked service thread
  instead of a detached one-shot thread
- snapshot tx-index build is now owned through an explicit
  `snapshot_tx_index_job` controller boundary instead of a detached one-shot
  thread or raw boot-managed `pthread_t`
- snapshot import’s parallel block-index / UTXO / wallet workers are now
  owned through a single import-job coordinator instead of scattered raw
  `pthread_create` / `pthread_join` bookkeeping in the orchestrator
- snapshot import now fails closed before syncing snapshot files into the live
  datadir: if any import worker fails, the orchestrator returns an error
  instead of copying blocks/chainstate after a partial import
- snapshot import’s final block/index/chainstate sync no longer shells out via
  unchecked `system("cp ...")`; it now uses checked repo-native copy helpers,
  and `dir_copy()` now returns failure on partial file-copy errors instead of
  printing warnings while reporting success
- the legacy fast-import copy path in `boot.c` now obeys those checked copy
  contracts too: partial block/index/chainstate copy failure aborts boot
  import instead of continuing into a mixed runtime state
- snapshot creation now obeys the same checked copy rules too: if block files,
  blocks/index, or chainstate fail to copy into the snapshot directory, the
  partially created snapshot is removed and the operation returns failure
- `block_files_copy()` itself now fails closed on `blk*.dat` or `rev*.dat`
  copy errors instead of silently ignoring reverse-file copy failures, and the
  snapshot/boot callers distinguish true copy failure from “zero block files”
- boot service thread start/join paths for payment, replay, snapshot offer,
  address backfill, and tx-index are now consolidated behind shared helper
  functions so lifecycle handling stays consistent and DRY
- async RPC queue worker startup now uses an explicit checked start path
  instead of folding `pthread_create()` into a compound condition, and direct
  RPC tests now assert worker-count state is reset after finish-and-wait
- file-copy helper contracts now have direct unit coverage: `dir_copy()`
  success and partial-failure behavior, plus `block_files_copy()` failure on
  bad `rev*.dat` sources, are now exercised in the test suite
- `file_copy()` itself now refuses non-regular sources, so directory entries
  cannot be treated as successful file copies on Linux and the stricter
  fail-closed import/snapshot contracts hold under test
- RPC HTTP server lifecycle now has direct robustness coverage for
  start/stop/stop-again behavior on a reserved loopback port so the server
  shutdown path stays idempotent under test
- that robustness coverage exposed a real bug, now fixed: the RPC HTTP server
  tracks listen-thread and worker-start state explicitly, resets auth/runtime
  state on stop, and refuses a new start while old server state is still live
- fast-sync chunk and block manifests now publish through explicit
  `msgprocessor` ownership APIs instead of boot and P2P code mutating shared
  globals directly; readers now consume stable manifest headers under lock,
  and the cache contracts have direct tests
- snapshot sync now has a runtime-owned service instance wired through
  `app_runtime_context`; message processing, API status routes, and active-sync
  gating now prefer that explicit service owner instead of reaching for the
  lazy global singleton first
- the prebuilt fast-sync snapshot cache now has an explicit publish/reset
  contract in `fast_sync.c` instead of ad hoc raw globals, and direct tests
  cover cache ownership and getter/reset behavior
- the cached snapshot offer in `msgprocessor` now also has explicit get and
  invalidate operations, and the offer/send/serve paths read through that
  cache boundary instead of peeking at raw validity globals
- the cached fast-sync UTXO root used for snapshot-offer rebuilds now also
  has explicit publish/get/reset operations behind a mutex instead of a raw
  validity/count global triplet in `fast_sync.c`
- manifest/block-manifest publish paths in `msgprocessor` now enforce internal
  invariants (non-empty, consistent manifest shapes) before adopting caller
  memory, and tests assert publish failures do not advance cache versions
- `file_controller` manifest reads now use copy-out access under a mutex, and
  manifest refresh builds into a local value before publishing, so API/RPC
  readers no longer depend on a mutable shared manifest pointer during rebuild
- file-service startup manifest build is now tracked and joined instead of
  detached, and file-service start/stop is being hardened toward idempotent
  lifecycle behavior
- file-service client handling is moving from detached per-connection threads
  toward bounded worker ownership so shutdown can quiesce the network surface
  cleanly
- HTTPS explorer serving now uses a bounded worker pool instead of detached
  per-client threads, and start/stop is idempotent with explicit listener and
  worker joins
- `connman_start()` now has explicit partial-start rollback and per-thread
  lifecycle tracking instead of assuming all P2P thread creation succeeds
- generic async RPC queue worker startup now returns success/failure and no
  longer increments worker state optimistically when `pthread_create` fails
- metrics thread start/stop is now idempotent and startup failure is surfaced
  instead of assuming the optional metrics worker always starts cleanly
- embedded Tor now tracks whether the onion-monitor helper thread actually
  started before trying to join it during shutdown
- async event observer dispatch now starts idempotently, fails closed on
  thread-start errors, and stops safely even if startup never completed
- background ZK params loading now tracks whether the loader thread actually
  started before boot joins it, so startup failure cannot turn into an
  unconditional join on invalid thread state
- API cache and lookup helpers now use checked detached-thread startup with
  atomic first-start gating, so concurrent first requests cannot spawn
  duplicate workers and startup failure returns a clean 503 instead of
  pretending the background helper is alive
- explorer prewarm and background cache builders now use the same checked
  detached-thread startup pattern with atomic gating, so concurrent requests
  cannot double-launch stats/hodl/tokens/factoids workers
- legacy wallet import now fails closed on local worker startup errors in its
  pass-1 scan and sapling filter/decrypt batches, with explicit join/cleanup
  instead of assuming every helper thread launched
- wallet scan now uses the same fail-closed pass-1 scan worker startup
  handling, so a thread-launch failure aborts cleanly instead of leaving the
  scan logic assuming every batch worker exists
- `indexlegacy` Phase B extraction now fails closed on partial worker startup,
  joining already-started threads and returning a real RPC error instead of
  proceeding with a partially launched extraction pool
- mining thread startup now fails closed too: `gen_start()` only leaves mining
  marked running if all miner threads launched, and `gen_stop()` no longer
  assumes a full thread array exists after partial startup failure
- file-service client parallel download startup now fails closed: if any range
  worker cannot launch, already-started workers are cancelled/joined and the
  client returns failure instead of continuing with a partial download pool
- UTXO import decoder startup is now all-or-nothing: partial decoder launch no
  longer returns success via a side-effected cancel flag, and startup failure
  cleans up/joins before the import path restores DB mode and exits
- startup SQLite catchup is now owned as an explicit `sync_controller`
  catchup job object instead of boot carrying a raw pthread plus argument bag
- snapshot import now fails cleanly on partial thread-start failure instead of
  assuming all three parallel import threads always launch successfully
- UTXO import’s internal decoder/writer pipeline is now owned through an
  explicit import-job object instead of open-coded local thread bookkeeping
- UTXO import startup sequencing is now owned by the import-job boundary too:
  decoder and writer launch are coordinated through one helper instead of
  being split between the job object and the caller
- UTXO import now also has a public sync-job object boundary, and snapshot
  import uses that API instead of wrapping the heavy import function directly
- UTXO import is being hardened with explicit cancellation and cleanup so
  shutdown or startup failure does not leave the parallel import pipeline
  running blindly

### Health / Observability

`node_db` runtime state is tracked explicitly:

- open
- tx_open
- turbo_mode
- last activity
- last sqlite rc
- last op

`node_health_service` now exposes:

- DB runtime status
- DB-service worker / queue state
- DB-service uptime
- catchup/import job activity and progress age
- degraded reasons for:
  - stale open DB tx
  - DB-service worker down
  - DB-service queue backlog
  - stalled catchup
  - stalled UTXO import

### Concurrency Fixes Already Landed

- file-service receive buffer made session-local
- file-service manifest access locking improved
- `msgprocessor` download manager init made race-safe

### Real Workflows Now Under DB-Worker Ownership

These run as whole write callbacks on the DB worker when they target the
runtime DB:

- `snapsync_begin_receive(...)`
- `snapsync_apply_chunk(...)`
- `snapsync_finalize(...)`
- `node_db_sync_connect_block(...)`
- `node_db_sync_disconnect_block(...)`
- `node_db_sync_wallet_tx(...)`
- `node_db_sync_wallet_keys(...)`
- `node_db_sync_mempool_save(...)`
- `node_db_sync_mempool_add(...)`
- `node_db_sync_mempool_remove(...)`
- `node_db_sync_peer(...)`
- `node_db_sync_peer_score(...)`
- `node_db_sync_set_tip(...)`
- `node_db_sync_sapling_note(...)`
- `node_db_sync_sapling_spend(...)`
- startup shielded-value backfill in `config/src/boot.c`

This is the important transition:

- old model: shared mutable DB handle with scattered call sites
- current model: increasing number of whole workflows owned by the DB worker

## What Is Still Unsafe / Incomplete

### 1. Biggest Remaining DB Ownership Debt

Heavy workflows still do too much direct SQLite work:

- `app/controllers/src/sync_controller.c`
  - `node_db_sync_catchup(...)`
  - `node_db_sync_import_utxos(...)`
  - large batch/loop write bodies
- `app/services/src/snapshot_sync_service.c`
  - better than before, but still needs service-level cleanup beyond DB writes

These should eventually become supervised jobs, not just large functions that
happen to touch SQLite.

### 2. Runtime / Supervisor Cleanup

Still unfinished:

- detached or weakly owned long-running threads
- inconsistent lifecycle management across services
- missing unified `init/start/stop/join/status` model

Recent progress:

- startup SQLite catchup no longer detaches and outlives boot/runtime
  ownership
- catchup loop now notices shutdown and can stop early instead of running
  blindly into teardown
- store payment processing now has a stop condition and an explicit join
  during shutdown
- background UTXO replay now joins before chainstate flush/free, so it no
  longer races `coins_tip` teardown
- snapshot-offer build now joins before DB/runtime teardown, so it no longer
  outlives the resources it reads
- file-service startup manifest hashing is no longer detached fire-and-forget;
  it is now tracked and joined during stop so it does not outlive server
  lifecycle ownership
- file-service client acceptance is moving under a bounded worker-pool model
  instead of detached unbounded handler threads
- HTTPS explorer accept loops no longer spawn detached client threads; they now
  feed a bounded shared queue drained by a fixed worker pool, so stop/join is
  deterministic and connection spikes have explicit backpressure
- UTXO import pipeline is moving toward supervised-job behavior with explicit
  stop handling, rollback/cleanup, and safer failure paths

### 3. Fast-Sync Artifact Ownership

Still largely untouched architecturally:

- snapshot export
- manifest generation
- block-piece publication
- freshness / invalidation / versioning

`snapshot export` has new fail-closed semantics in `file_controller` with direct tests,
but ownership remains partial and still needs one dedicated owner service for the full
fast-sync artifact lifecycle.

This still needs one dedicated owner service.

### 4. Performance Policy Is Not Formalized Yet

Still missing explicit policy for:

- queue depth ceilings
- batch sizes by workflow
- WAL/checkpoint cadence
- when private/dedicated DB handles are allowed
- acceptable RPC latency under sync load
- memory ceilings for import/snapshot paths

### 5. Security-Oriented Failure Policy Is Not Formalized Yet

Still needs explicit behavior for:

- partial snapshot apply
- failed snapshot finalize
- DB worker death
- queue saturation
- shutdown during long write jobs
- safe resume/recovery semantics

## Long-Term Target Architecture

### DB Roles

The architecture should end with three explicit DB roles:

1. Runtime write DB
- exactly one owner: `db_service`

2. Runtime read DB
- read-only handles for health, explorer, diagnostics, RPC reads

3. Offline/private maintenance DB
- dedicated handles only for isolated maintenance/rebuild tasks where sharing
  runtime mutable state would be worse

Current progress on this split:

- long-running catchup/import callers now prefer opening a private SQLite
  handle against the same DB file instead of reusing the runtime-owned handle
  in:
  - `config/src/boot_services.c`
  - `config/src/boot.c`
  - `app/controllers/src/blockchain_controller.c`
- catchup/import runtime status now records both start time and last progress
  time, so health can distinguish active maintenance work from stalled
  maintenance work

If code does not clearly fit one of these roles, ownership is still blurry.

### Service Boundaries

Transport/controller code should become:

- parsers
- validators
- command emitters

Not direct runtime DB mutation points.

Long-running subsystems should become service-owned jobs:

- catchup
- import
- snapshot receive/verify/finalize
- fast-sync artifact export/publication

### Trust Boundaries

- P2P snapshot payloads are untrusted until verification passes
- fast-sync artifacts are trusted only after publication by the owner service
- consensus-critical runtime state should only move through controlled service
  boundaries

## Current Testing Reality

Passing verification used repeatedly during this refactor:

- `make -j4 zclassic23`
- `make -j4 test_zcl`
- full `./test_zcl`
- direct runtime-backed wrapper coverage in `lib/test/src/test_sqlite.c`

Most recent targeted signal:

- `SQLite DB service nested worker writes stay reentrant... OK`
- `SQLite sync_controller wrappers use runtime DB service... OK`
- `SQLite mempool save/find/clear... OK`
- `SQLite peer save/find/recent... OK`
- runtime-backed wrapper coverage now also exercises `node_db_sync_wallet_tx(...)`
- local sync connect/disconnect paths now fail closed on statement/transaction
  errors instead of silently continuing with partial state
- snapshot receive/finalize now fail closed on malformed chunk apply and on
  finalize state-write failures, with a regression test covering malformed
  chunk unwind back to normal mode
- boot-time block-index cache persistence now checks clear/begin/bind/step and
  batch/final commit results, aborting cleanly instead of silently leaving a
  partial cache table behind
- boot-time `wallet_utxos` rebuild now checks `BEGIN` / bulk insert / `COMMIT`
  and rolls back on failure instead of silently leaving partial wallet UTXO
  state during startup
- full suite currently ends with `ALL TESTS PASSED (0 failures)`

Important baseline fact:

- the repository currently has a fully passing `test_zcl` baseline on the
  working tree

## Working Rules

When continuing this refactor:

- prefer whole-workflow ownership moves over wrapping individual DB calls
- do not split one transaction body across threads
- preserve direct fallback paths for dedicated / non-runtime DB handles
- treat tests as part of the refactor, not optional follow-up
- keep ownership helpers DRY and narrow
- do not harden the wrong abstraction just because it is nearby

## Best Next Steps

Highest-value next work, in order:

1. Design the final job model for:
   - catchup
   - UTXO import
   - snapshot receive/finalize lifecycle
2. Convert the heaviest remaining sync DB workflows into supervised jobs rather
   than only larger helper callbacks
3. Centralize fast-sync artifact ownership into one owner service
4. Unify runtime service lifecycle with explicit supervision
5. Add stronger soak/regression harnesses

Near-term file targets:

- `app/controllers/src/sync_controller.c`
- `app/services/src/snapshot_sync_service.c`
- `config/src/db_service.c`
- `config/src/boot_services.c`
- `app/controllers/src/file_controller.c`
- `lib/net/src/msgprocessor.c`

## Definition Of Better

The refactor is only truly better if:

- `zclassic23` keeps following `zclassicd`
- RPC stays responsive during heavy sync work
- DB behavior is coordinated instead of incidental
- operator health tells us why the node is degraded
- restart behavior becomes boring and deterministic
- ownership is obvious from the code
- security and performance rules are explicit, not tribal knowledge
