# Sync Hardening Plan

## Goal

Make `zclassic23` able to bring up a fresh peer node from an existing
`zclassic23` quickly, safely, and predictably on the same machine or LAN.

The target outcome is:

- bootstrap from a trusted `zclassic23` peer without manual recovery
- use SHA3-authenticated file transfer and MMB/FlyClient verification
- reach a usable chain state fast
- converge to `syncstate=at_tip`
- expose the whole path through explicit services, events, and operator checks

This is not just a bugfix list. It is a refactor program to align fast sync
with the architecture in `ARCHITECTURE.md`:

- controllers stay thin
- services own workflow/state-machine behavior
- models own persistence and cached read/write state
- `config/` composes runtime, not business logic
- events are the canonical observability path

## Refactor Shape

This work should be implemented with the same defaults as the broader
codebase refactor program:

- MVC boundaries stay explicit:
  - controllers adapt transport and expose operator-facing state
  - services own fast-sync orchestration and state-machine transitions
  - models own durable export/import metadata and diagnostic projections
- ActiveRecord remains the default persistence style for app-owned sync
  metadata and read models
- event emission is the default observability path for state transitions and
  rejection reasons
- `config/` should wire the fast-sync service graph, not contain sync policy
- helper macros are allowed only for stable boilerplate, not for hiding sync
  decisions, peer policy, or artifact ownership

## Concrete Target Architecture

### Controller Boundary

- `file_controller` exposes manifest/export status and explicit operator
  actions
- API/explorer surfaces consume read helpers or model-backed diagnostics
- controllers do not decide bootstrap policy, retry logic, or state-machine
  transitions

### Service Boundary

- a dedicated fast-sync service owns:
  - trusted bootstrap peer policy
  - manifest/export refresh orchestration
  - secure-offer acceptance/rejection logic
  - FlyClient verification progression
  - `zsnapreq` request/retry/fail progression
  - snapshot receive/verify/activate progression
- supporting services may own export build, manifest refresh, and projection
  updates if that keeps responsibilities narrow

### Model Boundary

- persisted metadata should cover:
  - exported artifact set freshness/version
  - last successful manifest build and coverage
  - last secure snapshot export
  - last secure snapshot import
  - last fast-sync attempt summary and failure reason
- read helpers should answer operator questions without forcing controllers to
  own SQL

### Event Boundary

- emit explicit events for:
  - export build start/complete/fail
  - manifest invalidated/refreshed/served
  - snapshot offer sent/accepted/rejected
  - FlyClient challenge/proof pass/fail
  - `zsnapreq` sent/retried/failed
  - snapshot verified/activated/fallback triggered

## Refactor Checklist

### Phase 1. Stabilize Export Ownership

- [ ] replace mutable live export assumptions with an explicit exported
  artifact set
- [ ] stop serving `block_index.bin` unless it is exported as a stable artifact
- [ ] make `consensus_snapshot.db` part of the explicit export contract
- [ ] make manifest generation consume only the exported artifact set

Acceptance gate:

- file service serves only stable exported artifacts, not hot runtime files

### Phase 2. Centralize Manifest Ownership

- [ ] move manifest rebuild/invalidation behind a dedicated service boundary
- [ ] keep controller RPC/API surfaces read-only over manifest state where
  possible
- [ ] make export-set coverage a hard validity rule
- [ ] expose manifest readiness and freshness through one read-model surface

Acceptance gate:

- manifest state has one authoritative owner and one operator-facing read path

### Phase 3. Build The Secure Sync State Machine

- [ ] define named fast-sync states and terminal reasons
- [ ] move secure progression logic behind one service-owned state machine
- [ ] separate file bootstrap, proof verification, snapshot request, snapshot
  receive, and activation phases
- [ ] make every transition queryable and event-emitting

Acceptance gate:

- secure fast sync can be reasoned about as state transitions, not log archaeology

### Phase 4. Persist Diagnostics And Projections

- [ ] add model-owned metadata for manifest/export freshness
- [ ] add model-owned metadata for last successful export/import
- [ ] add model-owned attempt/failure summaries for secure sync
- [ ] add read helpers for RPC/API/operator diagnostics

Acceptance gate:

- important fast-sync state survives restart and is queryable through models

### Phase 5. Harden Peer Policy And Recovery

- [ ] make trusted bootstrap peer selection explicit
- [ ] keep fallback policy behind service-owned decisions
- [ ] add idempotent recovery for half-complete secure sync
- [ ] make operator-visible reasons for trusted-peer loss or fallback

Acceptance gate:

- a fresh node can prefer a trusted local peer intentionally and recover cleanly

### Phase 6. Automate Acceptance

- [ ] add deterministic two-node/three-node secure-sync harnesses
- [ ] verify manifest/export coverage in automation
- [ ] verify offer -> proof -> request -> activate progression in automation
- [ ] verify fallback behavior when export artifacts are incomplete or stale

Acceptance gate:

- secure fast sync is reproducible and regression-tested

## Findings From Live Probe

Fresh-node probe against local source node:

- source node: `/home/rhett/.zclassic-c23`
- probe node: `/home/rhett/.zclassic-c23-syncprobe`
- source P2P/RPC/file service: `8033` / `18232` / `18034`
- probe P2P/RPC: `18035` / `18235`

Observed behavior:

- local file service is fast when it works: about `6.1 GB` in `10s`
- SHA3-secure file-service handshake is active
- source node offered a snapshot and served FlyClient proofs
- receiver did not complete the secure fast path end-to-end

Concrete failures observed:

1. local file-service transfer failed on the last `9` chunks
2. source log reported `SHA3 mismatch` while serving `file_index=253`
3. cached manifest currently contains `block_index.bin` chunks but no
   `consensus_snapshot.db` chunks
4. probe node fell back to `waiting for P2P snapshot`
5. receiver did not visibly advance from FlyClient proof receipt to
   `zsnapreq`/snapshot receive
6. local addnode session was not stable enough to trust as the only sync path

## Design Rules

- keep transport parsing in `lib/net` and controllers only
- move sync policy and multi-step decisions into `app/services`
- keep manifest/snapshot metadata in model-owned or service-owned state, not
  scattered across boot and message handlers
- represent fast-sync phases as explicit state transitions, not log-only
  conventions
- emit events for every material transition and rejection
- avoid serving mutable artifacts directly from live runtime paths when a
  stable export artifact is more correct
- prefer small explicit helpers over large macros in sync code

## Workstreams

### 1. Stable Export Artifacts

Problem:

- file service is serving artifacts whose contents can drift under a cached
  manifest

Checklist:

- [ ] define which artifacts are allowed to be served live
- [ ] stop treating live `block_index.bin` as an implicitly stable export
- [ ] create an explicit exported fast-sync artifact set owned by one service
- [ ] make `consensus_snapshot.db` generation part of that exported artifact set
- [ ] include freshness/version metadata for every exported artifact
- [ ] make manifest generation consume only those exported artifacts

Exit criteria:

- file service serves a stable export set, not mutable runtime files

### 2. Manifest Ownership And Invalidation

Problem:

- manifest cache can remain valid enough to load while omitting newer required
  artifacts such as `consensus_snapshot.db`

Checklist:

- [ ] move manifest ownership behind a dedicated snapshot/file-sync service API
- [ ] invalidate cached manifest when export set changes
- [ ] invalidate cached manifest when snapshot file appears, disappears, or is
  replaced
- [ ] verify required artifact coverage, not just first/last chunk freshness
- [ ] expose manifest summary by service/model API instead of hidden globals
- [ ] add an operator-visible RPC/API for manifest status and freshness

Exit criteria:

- a cached manifest cannot be reused if required secure-fast-sync artifacts are
  missing or stale

### 3. Secure Fast-Sync State Machine

Problem:

- the secure path exists, but its receiver-side transitions are not explicit
  enough to diagnose or guarantee progression

Checklist:

- [ ] formalize the fast-sync phases as one service-owned state machine
- [ ] make offer accept/reject reasons explicit and queryable
- [ ] make FlyClient challenge/proof verification outcomes explicit and queryable
- [ ] make `zsnapreq` send/retry/fail outcomes explicit and queryable
- [ ] make snapshot chunk receive/verify/activate phases explicit and queryable
- [ ] separate file-service bootstrap state from P2P snapshot state

Exit criteria:

- every secure fast-sync phase is represented by an explicit state and reason,
  not only by ad hoc logs

### 4. Receiver Progression And Recovery

Problem:

- receiver got through offer/proof exchange but did not complete snapshot
  receive in a way that was easy to observe or recover

Checklist:

- [ ] log and emit event when FlyClient verification passes
- [ ] log and emit event when `zsnapreq` is sent
- [ ] log and emit event when `zsnapreq` is rejected, dropped, or times out
- [ ] add bounded retry policy for receiver-side secure snapshot request flow
- [ ] keep receiver progression decisions in `snapshot_sync_service`
- [ ] make recovery from half-complete secure sync idempotent

Exit criteria:

- receiver can either complete secure snapshot receive or report a precise,
  actionable failure reason

### 5. Peer Selection And Trust Policy

Problem:

- a fresh node may drift toward seed fallback or unstable peers before it fully
  exercises the trusted local fast path

Checklist:

- [ ] define explicit preferred bootstrap peer policy for same-host / same-LAN
- [ ] separate trusted bootstrap peer choice from generic peer discovery
- [ ] make `-fileservice=` and bootstrap-peer policy first-class runtime inputs
- [ ] ensure `connect-only`/bootstrap-only semantics are explicit and tested
- [ ] keep file-service fallback policy in a service, not scattered through boot
- [ ] expose which peer is currently trusted for bootstrap and why

Exit criteria:

- operators can intentionally pin a trusted bootstrap peer and understand when
  fallback happens

### 6. Runtime Composition Cleanup

Problem:

- bootstrap, manifest build, snapshot export, and message handling are split
  across boot code and protocol code in a way that hides ownership

Checklist:

- [ ] move fast-sync composition behind a dedicated service registry entry
- [ ] reduce boot-time direct ownership of snapshot-offer building
- [ ] keep `config/` focused on wiring service dependencies and startup order
- [ ] move policy decisions out of `boot_services.c` into service helpers
- [ ] remove hidden coupling between manifest globals and message router logic
- [ ] define one runtime context for fast-sync dependencies

Exit criteria:

- fast sync has explicit runtime ownership and composition boundaries

### 7. Persistence And ActiveRecord Boundaries

Problem:

- fast-sync runtime metadata is still mostly implicit or global instead of
  persisted/queryable in app-owned state

Checklist:

- [ ] define model-owned metadata for export-set freshness
- [ ] define model-owned metadata for last successful secure snapshot build
- [ ] define model-owned metadata for last successful secure snapshot import
- [ ] keep durable sync diagnostics in SQLite where they help operator tooling
- [ ] avoid direct schema ownership in controllers/services
- [ ] add narrow read helpers for sync diagnostics instead of controller SQL

Exit criteria:

- important fast-sync metadata survives restart and is available through
  model/read-helper boundaries

### 8. Events And Observability

Problem:

- critical secure-sync decisions are observable mainly through raw logs

Checklist:

- [ ] emit events for manifest build start/complete/invalidated
- [ ] emit events for snapshot offer sent/accepted/rejected
- [ ] emit events for FlyClient challenge/proof pass/fail
- [ ] emit events for `zsnapreq` sent/retried/failed
- [ ] emit events for snapshot verified/activated
- [ ] expose those events in operator-facing RPC/API summaries

Exit criteria:

- operators can diagnose fast-sync behavior from events and RPC/API state, not
  by tailing logs alone

### 9. Test Harness And Acceptance

Problem:

- the behavior is too complex to rely on ad hoc manual testing

Checklist:

- [ ] add a deterministic local two-node or three-node sync harness
- [ ] add acceptance coverage for manifest contents and invalidation
- [ ] add acceptance coverage for local trusted bootstrap path
- [ ] add acceptance coverage for secure snapshot offer -> FlyClient -> request
- [ ] add acceptance coverage for fallback behavior when export set is incomplete
- [ ] add operator validation script for fresh-node secure sync

Exit criteria:

- the secure fast path is reproducible in automation, not only by manual probes

## Recommended Execution Order

1. stable export artifacts
2. manifest ownership and invalidation
3. secure fast-sync state machine visibility
4. receiver progression and recovery
5. peer selection and trust policy
6. runtime composition cleanup
7. persistence and diagnostic read-models
8. event observability and acceptance harnesses
8. acceptance harness and operator tooling

## Immediate Next Slice

- [ ] document live findings in the refactor log
- [ ] add this sync-hardening plan to the repo
- [ ] audit manifest/export ownership around `file_controller.c`,
  `file_service.c`, `boot_services.c`, and `snapshot_sync_service.c`
- [ ] make manifest validity require explicit coverage of
  `consensus_snapshot.db` when it exists
- [ ] stop serving a stale cached manifest that omits required secure-sync
  artifacts

## Acceptance Target

A fresh node started against an existing trusted `zclassic23` should:

1. select the trusted bootstrap peer intentionally
2. fetch a stable export set over SHA3-authenticated file service
3. receive a snapshot offer bound to the chain by MMB/FlyClient
4. verify the offer and request the snapshot
5. import and verify the snapshot
6. replay only the delta
7. reach `syncstate=at_tip`
8. match source-node `getutxocommitment` and `getmmrroot`
