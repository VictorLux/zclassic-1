# ZClassic23 Architecture Plan

## Goal

Refactor `zclassic23` into a clean, modern C23 architecture that keeps:

- upstream-style consensus and P2P correctness
- MVC ergonomics for app features
- ActiveRecord-backed persistence
- event-driven observability
- high-throughput node performance

The main constraint is that this is still a full node. Consensus, chainstate,
P2P, and storage correctness come first. MVC exists around the node, not in
place of it.

Execution checklist:

- see [REFACTOR_CHECKLIST.md](REFACTOR_CHECKLIST.md)

## Current Problems

The current tree already has `app/models`, `app/controllers`, and `app/views`,
but the runtime boundaries are still blurred:

- `config/boot.c` pulls in most of the system directly
- `lib/net/src/msgprocessor.c` mixes protocol handling, sync orchestration,
  snapshot logic, and recovery heuristics
- controllers contain both transport logic and business orchestration
- services exist ad hoc, not as a first-class layer
- some controller/model code reaches into globals instead of explicit context

The result is hard-to-debug sync behavior, especially for stalls and recovery.

## Design Principles

1. Consensus and P2P stay in `lib/`.
2. App MVC stays in `app/`.
3. Cross-cutting orchestration moves into `app/services/`.
4. `config/` performs composition only: build context, wire dependencies,
   start services, stop services.
5. ActiveRecord models persist state but do not run the node.
6. Events are the canonical observability path.
7. Hot paths avoid allocation-heavy or callback-heavy abstraction.
8. Controllers adapt inputs and outputs only; they do not own workflow logic.

## Runtime Composition

`config/` should be the explicit composition root for app-layer dependencies.

Current direction:

- use `app_runtime_context` to pass app-owned resources into long-lived runtime
  components
- expose one shared runtime registry for core consumers that still need
  app-owned resources during the transition away from globals
- keep `msgprocessor` and boot/service code dependent on injected runtime state
  instead of discovering `node_db`, `wallet`, or `mempool` through globals
- keep startup and shutdown ownership explicit in `config/`, with named phases
  for stop, persist, quiesce, and release

## Target Layers

### 1. Core Node Layer

Location:

- `lib/chain`
- `lib/coins`
- `lib/consensus`
- `lib/core`
- `lib/net`
- `lib/primitives`
- `lib/rpc`
- `lib/script`
- `lib/storage`
- `lib/validation`
- `lib/wallet`

Responsibility:

- consensus rules
- active chain and block index
- P2P wire protocol
- download scheduling
- block and tx validation
- wallet primitives
- raw storage engines

Rules:

- no HTML, route, or explorer logic
- no direct SQLite app-model assumptions in validation hot paths
- no controller includes
- event emission is allowed

### 2. App Model Layer

Location:

- `app/models`

Responsibility:

- SQLite-backed domain models
- ActiveRecord validation and callbacks
- application read models
- explorer caches, peer metadata, wallet-facing projections

Rules:

- no networking
- no RPC route handling
- no orchestration loops
- no direct dependency on HTTP or P2P node structs if avoidable

### 3. Service Layer

Location:

- `app/services`

Responsibility:

- long-lived workflows
- state machines above raw protocol handling
- coordination across core + models + events
- health/status aggregation
- sync orchestration
- snapshot lifecycle
- wallet indexing/replay
- peer policy and app-level recovery

This becomes the real application layer.

### 4. Controller Layer

Location:

- `app/controllers`

Responsibility:

- RPC method handlers
- HTTP route handlers
- argument validation
- response serialization selection
- calling services

Rules:

- thin
- stateless where possible
- no direct chain mutation except through service/core APIs

### 5. View Layer

Location:

- `app/views`

Responsibility:

- HTML rendering
- JSON response formatting
- explorer formatting helpers

Rules:

- no DB queries
- no business rules
- no chain mutation

### 6. Composition Layer

Location:

- `config`
- `main.c`

Responsibility:

- construct `app_context`
- wire service registry
- start and stop node subsystems
- provide process lifecycle

Rules:

- no feature logic
- no direct route logic
- no chain mutation beyond startup/shutdown composition

## Target Runtime Pattern

### Request Path

`transport -> controller -> service -> model/core -> event -> view`

Examples:

- RPC `getpeerinfo`
  `rpc/server` -> `network_controller` -> `network_service` -> `connman`
- HTTP `/api/health`
  `https_server` -> `api_controller` -> `node_health_service` -> models/events/net
- Wallet rescan
  `wallet controller` -> `wallet_sync_service` -> wallet/core + models

### Consensus Path

`connman/msgprocessor -> sync service / download service -> validation -> storage -> events`

Controllers must not sit in this path.

## Service Catalog

These services should exist explicitly.

### `node_runtime_service`

Owns:

- app-level runtime state
- startup readiness
- shutdown coordination
- exported process context

Replaces:

- scattered globals in boot/runtime composition

### `sync_service`

Owns:

- high-level sync phase
- headers-first orchestration
- block download progression
- stall detection and recovery policy
- legacy peer vs `zclassic23` peer sync strategy

Consumes:

- `msgprocessor`
- `download_manager`
- `validation`
- events

This is the most important extraction.

### `snapshot_service`

Owns:

- snapshot offer acceptance
- chunk application
- verification
- transition back to normal sync

Current candidate:

- `snapshot_sync_service` now lives in `app/services`
- next step is shrinking `msgprocessor` so snapshot wire follow-up is service-driven

### `wallet_sync_service`

Owns:

- wallet transaction indexing
- Sapling witness advancement
- background rescan/replay
- wallet projection updates into SQLite

Current candidate:

- split `sync_controller.c` into wallet/indexing service + persistence adapters

### `node_health_service`

Owns:

- health snapshot
- readiness
- monitoring status
- API/RPC health output normalization

This extraction has already started.

### `explorer_query_service`

Owns:

- explorer-facing read queries
- cached aggregate stats
- read-only projection access

Keeps query logic out of controllers.

### `peer_policy_service`

Owns:

- app-level peer preference
- trusted legacy peers
- fast-sync peer eligibility
- recovery peer selection

### `event_projection_service`

Owns:

- consuming events asynchronously
- updating read models and metrics
- decoupling expensive post-processing from consensus hot paths

## ActiveRecord Pattern

ActiveRecord should remain, but only for app projections and control-plane
state, not for consensus-critical storage.

### Good ActiveRecord Uses

- explorer block/tx/address projections
- wallet-visible transaction history
- peer metadata and scoring cache
- file service metadata
- store/blog application tables
- health snapshots and diagnostics materialization

### Bad ActiveRecord Uses

- canonical active chain state
- consensus-critical UTXO truth
- block validation decisions
- mempool admission rules
- hot-path P2P state mutation

## Storage Pattern

Use a split storage model.

### Canonical Stores

- block files
- block index database
- coins/chainstate storage
- wallet cryptographic state

These remain in `lib/storage`, `lib/coins`, `lib/wallet`, `lib/validation`.

### Projection Stores

- SQLite `node.db`
- ActiveRecord models
- explorer caches
- peer and operational metadata

These live under `app/models` and are eventually updated by services or async
event projections.

## Event-Driven Pattern

The event system is already one of the stronger parts of the codebase. Lean
into it.

### Event Roles

- hot-path observability
- async projection updates
- health/error aggregation
- replayable diagnostics
- internal decoupling between core and app concerns

### Event Rules

1. Core emits events, does not know who listens.
2. Services may listen and react.
3. Controllers never own event pipelines.
4. Long-running reactions happen on async observers.
5. Events must be small, stable, and meaningful.

### Suggested Event Families

- protocol events
- sync lifecycle events
- validation events
- storage events
- wallet events
- projection events
- service lifecycle events

## Performance Pattern

This architecture should not regress node performance.

### Hot Path Rules

- no ActiveRecord writes directly inside tight validation loops if avoidable
- prefer append-only event emission + async consumption
- keep protocol parsing and validation structs plain C
- pass explicit context pointers, not dynamic registries, in hot loops
- avoid JSON or template generation in core code
- keep services mostly orchestration, not abstraction-heavy wrappers

### High-Performance Boundaries

- `msgprocessor` parses and dispatches wire messages
- `sync_service` decides what to do next
- `validation` validates
- `storage` persists canonical state
- `event_projection_service` updates SQLite projections later

That is the upstream-compatible path.

## Dependency Direction

Allowed:

- `app/controllers` -> `app/services`, `app/views`, selected `lib/*`
- `app/services` -> `app/models`, `lib/*`, `event/*`
- `app/models` -> `lib/core`, `lib/json`, SQLite helpers
- `config` -> everything for composition only
- `lib/*` -> `lib/*`, `event/*`

Disallowed:

- `lib/*` -> `app/controllers`
- `lib/*` -> `app/views`
- `app/models` -> `app/controllers`
- `app/views` -> `app/models`
- `config` becoming a feature layer

## Context Objects

Replace global sprawl with explicit context structs.

### `struct node_runtime`

Contains:

- main state
- mempool
- coins tip
- connman
- rpc table
- wallet
- node db
- metrics
- current services

### `struct service_registry`

Contains:

- `sync_service`
- `snapshot_service`
- `wallet_sync_service`
- `node_health_service`
- `explorer_query_service`
- `peer_policy_service`

Controllers should accept or access this registry through composition, not
through unrelated globals.

## Migration Plan

### Phase 1. Service First

- create `app/services`
- move existing non-controller workflows there
- keep APIs stable

Targets:

- `snapshot_sync_service`
- node health
- explorer query aggregation

### Phase 2. Extract Sync Engine

- move sync phase/stall/header/block policy out of `msgprocessor.c`
- leave `msgprocessor` as protocol adapter
- make `sync_service` the owner of:
  - sync phase
  - stall recovery
  - re-request policy
  - header-to-block transition

This is the critical reliability phase.

### Phase 3. Split `sync_controller`

Current `sync_controller.c` is really multiple things:

- SQLite projection updates
- wallet sync
- witness management
- chain catchup

Split into:

- `chain_projection_service`
- `wallet_sync_service`
- `wallet_witness_service`
- `node_projection_repository` or keep as model helpers

### Phase 4. Runtime Context

- replace broad globals with `node_runtime`
- wire services from `boot_services.c`
- reduce cross-file extern coupling

### Phase 5. Async Projection Pipeline

- move expensive SQLite projection work behind async observers where safe
- keep consensus persistence canonical and immediate
- rebuild projections from events if needed

### Phase 6. Query Side Cleanup

- controllers become thin
- move explorer and health queries into services
- make views purely formatting

## Immediate Refactor Targets

These are the next concrete cuts, in order.

### 1. `msgprocessor.c`

Split into:

- `protocol_router`
- `headers_sync_handler`
- `block_download_handler`
- `snapshot_protocol_handler`

Owned by:

- `sync_service`
- `snapshot_service`

### 2. `sync_controller.c`

Rename conceptually away from “controller”.

Split into:

- `wallet_sync_service`
- `chain_projection_service`
- `sapling_witness_service`

### 3. `boot.c` and `boot_services.c`

Reduce to:

- context construction
- service initialization
- registration
- lifecycle

### 4. `api_controller.c`

Move all non-routing business logic into:

- `node_health_service`
- `explorer_query_service`
- `snapshot_service`

## Module Naming Convention

Use names that describe role, not transport.

- `*_controller.c`: request adapters only
- `*_service.c`: orchestration and workflows
- `*_repository.c` or model helpers: persistence/query code
- `*_view.c`: formatting and rendering
- `*_policy.c`: decision logic with minimal side effects

## Architectural Definition of Done

The refactor is successful when:

1. `msgprocessor.c` no longer owns sync policy.
2. `sync_controller.c` no longer acts as a controller.
3. `boot.c` is mostly composition.
4. health/status logic is not duplicated across endpoints.
5. SQLite projections can lag safely without risking consensus correctness.
6. controllers are thin enough to read in one pass.
7. events tell the full story of sync progress and failure.

## Summary

The clean design is:

- upstream-style full node core in `lib/`
- MVC for app delivery in `app/`
- service layer as the real application runtime
- ActiveRecord for projections and app data
- event-driven async updates for observability and performance
- composition-only startup in `config/`

That gives `zclassic23` the right shape:

- stable like `zclassicd`
- clean like Rails
- observable like an event-sourced system
- fast enough for a real power node
