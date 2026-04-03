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

- overall architectural completion: ~60%
- RPC front door refactor: mostly done
- DB ownership boundary: materially improved, not finished
- observability: meaningfully improved
- fast-sync artifact ownership: mostly not done
- runtime/supervisor cleanup: mostly not done
- soak/integration harnesses: mostly not done

Blunt summary:

- the direction is correct
- the foundation is much better
- the hardest architecture debt is still ahead

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
- snapshot tx-index build is now intended to be a tracked service thread
  instead of a detached one-shot thread
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
- startup SQLite catchup is now owned as an explicit `sync_controller`
  catchup job object instead of boot carrying a raw pthread plus argument bag
- snapshot import now fails cleanly on partial thread-start failure instead of
  assuming all three parallel import threads always launch successfully
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
- snapshot worker-path tests compile into `test_zcl`; bounded monolithic runs
  have not yet reached that test group reliably
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
