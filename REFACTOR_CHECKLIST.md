# ZClassic23 Refactor Checklist

## Goal

Turn `zclassic23` into a strong, modern, high-performance full node with:

- legacy `zclassic` behavioral compatibility on the sync path
- thin MVC transport layers
- explicit service orchestration
- ActiveRecord-backed app projections
- event-driven observability
- runtime guardrails that keep the power node at tip

This checklist is execution-oriented. `ARCHITECTURE.md` describes the target
shape; this file tracks the concrete work needed to get there.

## Reference Behavior

Primary reference:

- upstream `zclassic` sync behavior in `src/main.cpp`
  https://raw.githubusercontent.com/zclassiccommunity/zclassic/master/src/main.cpp

Non-negotiable behavior rules:

- header-first sync must remain compatible with legacy `zclassic`
- header continuation should follow upstream full-batch behavior
- stalled sync recovery must prefer correctness over aggressiveness
- snapshot sync must resume normal header/block sync deterministically

## Phases

### Phase 1: Stabilize The Service Boundary

Objective:

- make `lib/net/src/msgprocessor.c` a transport adapter instead of a workflow owner

Checklist:

- [x] extract node health snapshot logic into `node_health_service`
- [x] create `sync_service`
- [x] create `snapshot_sync_service`
- [x] move header resend, stall recovery, and progress policy into services
- [x] move snapshot handshake and verification follow-up into services
- [x] move `getheaders` and block `getdata` serialization out of `msgprocessor`
- [x] move composite header-processing planning into `sync_service`
- [x] move more header-processing execution decisions into `sync_service`
- [x] move valid-block and snapshot-end transition planning to service-owned result structs
- [x] move header activation and snapshot-offer/serve-start application decisions behind service-owned results
- [ ] replace remaining direct sync-state mutations with service-owned action/result structs
- [ ] replace remaining direct peer-state mutations with service-owned action/result structs
- [ ] move the last `activate_best_chain` execution decisions behind service results

Exit criteria:

- `msgprocessor` mostly parses, validates wire input, and executes planned actions
- sync policy is unit-testable without message-router branching

### Phase 2: Split Sync Into Explicit Domain Services

Objective:

- reduce `sync_service` from a broad utility layer into clear bounded services

Checklist:

- [x] introduce `header_sync_service`
- [x] move locator building, header continuation, and header-batch planning there
- [x] introduce `block_sync_service`
- [x] move queue planning, block assignment, stall recovery, and catch-up transitions there
- [ ] keep `snapshot_sync_service` as the snapshot state machine
- [x] leave `msgprocessor` depending on service interfaces, not mixed sync internals
- [ ] keep event emission stable during the split

Exit criteria:

- header sync, block sync, and snapshot sync have separate responsibilities
- router code no longer couples unrelated sync phases together

### Phase 3: Make Runtime Composition Explicit

Objective:

- move from global-heavy startup to explicit runtime composition

Checklist:

- [ ] introduce a clear `app_context` / runtime context for app-layer services
- [x] introduce an initial runtime context for `msgprocessor` app-layer dependencies
- [x] extend runtime-owned dependency access into `boot_services`
- [x] add a shared runtime registry for core runtime consumers
- [x] move controller-level health and wallet token flows off active globals
- [x] narrow `boot.c` global reads to compatibility-only alias assignments
- [x] isolate `boot_services` compatibility alias publish/clear into explicit helpers
- [x] remove dead wallet and mempool compatibility aliases
- [x] remove the last live node-db compatibility alias
- [x] centralize wallet controller composition in one context object
- [x] move `wallet_controller.c` to direct `wallet_rpc_context` access
- [x] move `wallet_shielded_controller.c` to direct `wallet_rpc_context` access
- [x] move `wallet_rescan_controller.c` to direct `wallet_rpc_context` access
- [ ] move ad hoc globals behind runtime-owned state where practical
- [ ] make `config/` responsible for composition only
- [x] document service lifecycle ownership and shutdown ordering
- [x] make service dependencies explicit in `boot_services`

Exit criteria:

- service wiring is visible in one place
- app features do not discover dependencies through globals by default

### Phase 4: Harden Sync Reliability

Objective:

- make “stay synced to legacy tip” measurable and defendable

Checklist:

- [ ] add sync invariants around chain tip, header tip, queue state, and peer state
- [ ] add structured sync metrics for headers, blocks, queue depth, retries, and stalls
- [ ] add event counters for snapshot negotiation, verification, resume, and fallback
- [ ] add explicit watchdog coverage for stale tip, repeated header rejection, and block churn
- [ ] audit timeout and retry thresholds against observed mainnet behavior
- [ ] document operator-visible failure states and recovery paths

Exit criteria:

- the node exposes enough data to explain why it is or is not making progress
- regressions in sync behavior show up through tests or metrics quickly

### Phase 5: Add Stronger Validation And Soak Coverage

Objective:

- verify the refactor under realistic sync conditions

Checklist:

- [ ] add focused service tests for each remaining action/result type
- [ ] add integration coverage for legacy-node-following behavior
- [ ] add restart coverage for “headers ahead, blocks already on disk”
- [ ] add stall-recovery regression tests
- [ ] add snapshot-to-header resume tests
- [ ] add long-running soak coverage under peer churn

Exit criteria:

- service boundaries are backed by deterministic tests
- sync regressions are reproducible without manual node babysitting

## Current Focus

Current phase:

- Phase 3

Current coding target:

- keep runtime ownership visible through `app_runtime_context`
- document and tighten composition/lifecycle boundaries in `config/`

Current likely extraction order:

1. move validation/network/controller access onto explicit runtime or controller state
2. narrow boot-time logic to direct owned state instead of compatibility aliases
3. centralize controller-local shared state behind explicit context objects
4. make runtime ownership visible in more boot/config call sites
5. document lifecycle and shutdown ordering around runtime-owned services
6. retire wallet view/controller compatibility macros one surface at a time
7. keep shrinking remaining implicit composition patterns

## Progress Notes

- Keep detailed per-slice notes in `REFACTOR_LOG.md`
- Update this checklist when a phase item changes state
- Prefer small, verifiable slices over wide rewrites
