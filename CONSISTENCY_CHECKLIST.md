# Consistency Refactor Checklist

## Goal

Make the codebase more predictable across layers by standardizing:

- where SQL lives
- how SQLite statements are executed
- how runtime state is passed
- how large controllers/services are split
- which helpers/macros are the preferred default

This is a consistency program, not a one-shot cleanup. Each slice should
improve one repeated pattern without broad, risky rewrites.

## Principles

- prefer boundary consistency over adding more macros by default
- move persistence ownership toward models and read-model helpers
- keep controllers thin: parse input, call helpers/services, format output
- keep services workflow-oriented, not schema-owning
- use small helpers when they clarify lifecycle; use macros only for narrow,
  repeated boilerplate
- avoid parallel abstractions for the same problem
- make state-machine transitions explicit and event-observable for long-lived
  workflows such as fast sync and recovery

## Layer Ownership Rules

- controllers own transport adaptation, request validation, and response
  formatting
- services own workflow, state machines, retry policy, orchestration, and
  trust/fallback decisions
- models own durable app state, projection tables, and control-plane metadata
- ActiveRecord remains the default model-layer persistence pattern for
  app-owned state and diagnostics
- events are the preferred observability path for long-lived workflow
  transitions and asynchronous projections
- `config/` wires dependencies, startup order, and service registration; it
  should not keep absorbing runtime policy

### Design Pattern Defaults

- use read-model helpers for controller-facing queries that assemble page or
  API summaries
- use service-local state machines for multi-step sync/recovery workflows
- use ActiveRecord models or narrow model helpers for persisted metadata,
  caches, and diagnostics
- use event emission plus projection consumers when multiple operator surfaces
  need the same sync/progress story
- use macros only for stable repeated boilerplate, not to hide workflow
  decisions or SQL ownership

## Checklist

### 1. Controller Query Ownership

- [ ] audit controller-owned read SQL and group it by domain
- [ ] extract wallet-view read queries into wallet projection/read helpers
- [ ] extract explorer token/address/block stats reads into explorer read helpers
- [ ] extract API aggregate/stat queries into dedicated read helpers
- [ ] remove duplicated scalar query patterns once helper coverage exists

Exit criteria:

- controllers mostly call read helpers instead of owning multi-query report SQL

### 2. SQLite Call Pattern Standardization

- [ ] standardize read-only scalar/list query helpers for controllers/services
- [ ] standardize transaction helpers outside model/test code where repetition exists
- [ ] replace hand-rolled prepare/step/finalize blocks in low-risk files first
- [ ] keep dynamic SQL behind narrow helpers with explicit whitelisting

Exit criteria:

- raw SQLite lifecycle code follows one small set of recognizable patterns

### 3. Wallet View Consistency

- [ ] consolidate wallet-view query helpers behind one projection-oriented surface
- [ ] replace page-local formatting drift with shared formatting helpers
- [ ] reduce page-local globals by moving wallet-view state into an explicit context
- [ ] keep wallet view page handlers focused on rendering and request handling

Exit criteria:

- wallet view pages share one query/formatting vocabulary

### 4. Explorer And API Surface Split

- [ ] split `explorer_controller.c` by resource area
- [ ] split `explorer_factoids.c` by factoid/stat domain where practical
- [ ] split `api_controller.c` by API resource or feature group
- [ ] move reusable read logic into shared explorer/API helpers before splitting

Exit criteria:

- no single explorer/API controller acts like a subsystem dump

### 5. Runtime Context Cleanup

- [ ] inventory remaining controller/service globals that should be explicit state
- [ ] move wallet-view globals behind a dedicated runtime/context object
- [ ] keep `config/` as composition-only by shrinking remaining ambient globals
- [ ] convert service-local file-static counters/timers to explicit state where useful

Exit criteria:

- new code reaches dependencies through contexts by default, not ambient globals

### 6. Bulk Import / Rebuild Helpers

- [ ] extract repeated PRAGMA/index/drop/create sequences into dedicated helpers
- [ ] table-drive repetitive index rebuild/drop lists where it improves clarity
- [ ] consolidate repeated import transaction scaffolding
- [ ] keep high-volume import loops readable and explicit

Exit criteria:

- import/rebuild code is repetitive because of data shape, not because of setup noise

### 7. Fast Sync And Snapshot Hardening

- [ ] move fast-sync/export ownership behind explicit services
- [ ] make manifest/export invalidation rules explicit and testable
- [ ] formalize secure snapshot progression as a service-owned state machine
- [ ] emit event-level observability for offer/proof/request/verify/activate
- [ ] define persisted sync/export diagnostics behind model/read-helper boundaries
- [ ] add acceptance tooling for fresh-node secure sync from trusted peers

Exit criteria:

- a fresh node can securely and predictably fast-sync from a trusted peer with
  explicit state, events, and operator diagnostics

## Recommended Execution Order

1. wallet-view helper consistency
2. controller read-helper extraction
3. explorer/API splits
4. runtime context cleanup
5. bulk import helper cleanup
6. fast sync and snapshot hardening

## Execution Phases

### Phase 1. Normalize Boundaries

- [ ] finish documenting preferred ownership for query helpers, services,
  models, and events in the files currently being touched
- [ ] stop introducing new controller-owned multi-query SQLite report logic
- [ ] stop introducing new boot-time policy logic outside dedicated services

Acceptance gate:

- new slices follow existing architectural boundaries instead of adding fresh
  exceptions

### Phase 2. Consolidate Read Models

- [ ] finish extracting wallet-view page queries into projection/read helpers
- [ ] continue extracting explorer/API summary reads into shared helpers
- [ ] define dedicated sync/read helper surfaces for operator diagnostics

Acceptance gate:

- controllers mostly read through one shared vocabulary per domain

### Phase 3. Promote Workflow Ownership To Services

- [ ] move sync and bootstrap policy out of controllers and boot wiring
- [ ] represent secure fast-sync progression as explicit service-owned states
- [ ] keep retry and fallback policy behind service APIs instead of message or
  boot code

Acceptance gate:

- long-lived workflows are owned by services, not scattered across transport,
  controllers, and boot

### Phase 4. Persist Control-Plane State

- [ ] add model-owned metadata for export freshness and snapshot status
- [ ] add model-owned diagnostics for the last secure fast-sync attempts
- [ ] keep controller/API diagnostics on top of those model/read-helper
  surfaces

Acceptance gate:

- important sync/export state survives restart and is queryable without log
  scraping

### Phase 5. Complete Event Observability

- [ ] emit events for material sync/export state transitions
- [ ] project those events into operator-facing diagnostics where useful
- [ ] standardize event naming around service/state-machine transitions

Acceptance gate:

- operators can reconstruct sync progress and failure reasons from structured
  state plus events

### Phase 6. Acceptance And Cleanup

- [ ] add deterministic secure-fast-sync acceptance coverage
- [ ] add trusted-peer bootstrap validation scripts
- [ ] shrink remaining oversized controller/service files once ownership is
  clearer

Acceptance gate:

- secure sync is reproducible in automation and the remaining large files are
  split along clearer boundaries

## Current Slice

In progress:

- keep the consistency work tracked as an explicit step-by-step program
- extend shared controller SQLite helpers beyond scalar-only reads
- replace low-risk explorer/API prepare-step-finalize boilerplate with those helpers
- consolidate repeated explorer/API domain summaries behind shared read helpers

Recent slices:

- added wallet-view projection helpers for send/receive page queries
- standardized shared test DB macros across repeated fixture/setup code
- started explorer/API read-helper extraction with shared text and fixed-width
  integer row helpers
- started consolidating repeated ZSLP summary reads behind one shared helper
- started consolidating repeated address/privacy summaries behind shared helpers
- started consolidating repeated UTXO summaries behind shared helpers
- started consolidating repeated OP_RETURN and transaction summaries behind shared helpers
- started consolidating repeated chain height/block-count summaries behind shared helpers
- started consolidating repeated read-only node DB open/timeout boilerplate behind shared helpers
- started consolidating repeated first-privacy-height summaries behind shared helpers
- started tracking secure fast-sync hardening as a dedicated architectural
  program, not just a runtime bug hunt
- tightened file-manifest ownership so cached manifests rebuild when
  `consensus_snapshot.db` exists but `file_index=254` is missing, and so hot
  `block_index.bin` artifacts are skipped instead of being served through a
  stale manifest
- added explicit manifest refresh hooks so snapshot export can rebuild both the
  controller-side manifest cache and the live file-service manifest instead of
  relying on startup-only background hashing
- added an explicit manifest-status read surface so operators and higher-level
  services can see whether snapshot/block-index artifacts are present versus
  actually being served
