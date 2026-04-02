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
- [x] replace the main direct sync-state mutations with service-owned action/result structs
- [x] replace the main direct peer-state mutations with service-owned action/result structs
- [x] move the last header-path `activate_best_chain` execution decisions behind service results
- [ ] audit for any residual router-owned state transitions outside the established service boundaries

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
- [x] keep `snapshot_sync_service` as the snapshot state machine
- [x] leave `msgprocessor` depending on service interfaces, not mixed sync internals
- [x] keep event emission stable during the split

Exit criteria:

- header sync, block sync, and snapshot sync have separate responsibilities
- router code no longer couples unrelated sync phases together

### Phase 3: Make Runtime Composition Explicit

Objective:

- move from global-heavy startup to explicit runtime composition

Checklist:

- [x] introduce a clear `app_context` / runtime context for app-layer services
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
- [x] move major ad hoc globals behind runtime-owned or controller-owned state where practical
- [ ] finish the remaining non-wallet controller/runtime cleanup
- [ ] make `config/` responsible for composition only
- [x] document service lifecycle ownership and shutdown ordering
- [x] make service dependencies explicit in `boot_services`
- [x] remove the wallet-controller compatibility macro bridge

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
- [x] expose Tor/onion reachability in the health surface so hosted power-node apps are observable too

Exit criteria:

- the node exposes enough data to explain why it is or is not making progress
- regressions in sync behavior show up through tests or metrics quickly

### Phase 5: Add Stronger Validation And Soak Coverage

Objective:

- verify the refactor under realistic sync conditions

Checklist:

- [ ] add focused service tests for each remaining action/result type
- [x] expand model-layer validation/save lifecycle onto shared ActiveRecord macros
- [x] move ZSLP balances into a first-class model-backed table with tests
- [x] move ZSLP token metadata reads/writes onto model-backed paths with tests
- [x] move ZSLP transfer-history reads behind model/service-backed projections
- [x] expose ZSLP read resources through REST-style API endpoints with defensive validation
- [x] expose onion-announcement read resources through REST-style API endpoints with defensive validation
- [x] expose file-service read resources through REST-style API endpoints with defensive validation
- [x] expose peer read resources through REST-style API endpoints with defensive validation
- [x] move wallet contacts and onion announcements into first-class model-backed tables
- [x] move store product/order writes into first-class model-backed tables
- [x] move store product/order resource reads behind first-class model-backed queries
- [x] harden store resource and access-route validation with fail-closed id/query parsing
- [x] move wallet-view balance/tip projection queries behind model-backed wallet projection helpers
- [x] split oversized model tests into focused files by domain
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

- Phase 4

Current coding target:

- turn sync reliability into something observable and testable
- preserve the cleaner service/runtime boundaries while adding guardrails
- keep shrinking the remaining implicit controller/runtime coupling as side work

Current likely extraction order:

1. add sync invariants around chain tip, header tip, queue depth, and peer sync state
2. add structured sync metrics and event counters that explain forward progress and stalls
3. add regression coverage for stale-tip recovery, restart resume, and snapshot-to-header handoff
4. add an integration harness that compares `zclassic23` progress against legacy `zclassic`
5. audit remaining non-wallet controller/runtime state and finish the easy cleanup slices

## Immediate Checklist

- [ ] define the first sync invariant set and where each invariant is enforced
- [x] expose a compact sync metrics snapshot suitable for RPC/API/health output
- [ ] add regression tests for stall recovery and resume-from-disk block activation
- [ ] add a legacy-following integration test plan and fixture strategy
- [x] identify the remaining controllers with hidden runtime/composition state
- [x] document the routing rule: REST-style reads by default, command endpoints for mutating node operations

## Remaining Controller Cleanup Inventory

Higher-priority runtime/composition cleanup targets:

- `sync_controller.c`: still needs a larger service-oriented split, not just
  context cleanup
- `zslp_controller.c`: no longer uses a raw datadir global, but still mixes
  command-heavy token flows, wallet signing, and SQLite persistence in one
  large controller and should be split further toward dedicated service/view
  boundaries even after the new transfer-history read endpoint
- `store_controller.c`: main CRUD resource paths are model-backed now, but the
  remaining access-gate and product JSON bootstrap path still contain
  controller-owned persistence/query logic that should be reduced further
- `blog_controller.c`: onion announcement writes are model-backed, but the
  transaction-scan registry discovery path is still controller-heavy and can
  be split further toward services/read models
- `file_controller.c`: manifest/chunk APIs are established, but the manifest
  build/cache workflow is still controller-heavy and could be pushed further
  toward service/model ownership
- `network_controller.c`: runtime state is explicit now, but richer peer
  policy/read models still mostly live below the controller and could be
  surfaced further through dedicated services/resources
- `wallet_view_helpers.c`: balance/tip reads now use wallet model projections,
  but the large zclassicd-to-SQLite rebuild flow is still controller-owned and
  should be pushed behind wallet-facing services/models incrementally

Lower-risk cleanup completed after the audit:

- `network_controller.c`: moved from a raw file-local `g_cm` pointer to an
  explicit `network_context`
- `explorer_controller.c`: moved to explicit explorer context/backend/assets
  state with stricter read-side validation on tx/address/search/token routes
- `transaction_controller.c`: moved raw-tx RPCs to explicit `rawtx_context`
- `mining_controller.c`: moved mining RPCs to explicit `mining_context`
- `repair_controller.c`: moved repair RPCs to explicit `repair_context`
- `hodl_controller.c`: moved analytics RPCs to explicit `hodl_context` and now
  derives the coins DB from the injected cache instead of keeping a stale
  extra global pointer
- `misc_controller.c`: moved control/util RPCs to explicit `misc_context` and
  hardened `validateaddress` input validation
- `file_controller.c`: moved manifest/datadir state to explicit `file_context`
  and tightened manifest/chunk read failure handling
- `onion_service.c`: moved onion runtime state to explicit `onion_context` and
  brought hosted MVC apps/status forward on the landing and health surfaces
- `zslp_controller.c`: moved to explicit context/runtime helpers, centralized
  token/address validation, removed duplicated node.db open/close plumbing,
  moved token metadata to model-backed persistence, and added resource-style
  read RPCs for token show/list while keeping create/mint/send as commands;
  command request validation now lives in `zslp_service` instead of being
  duplicated across controller and RPC layers; token finalization and
  transfer-credit side effects now live in `zslp_command_service`; the shared
  wallet-side OP_RETURN patch/re-sign/commit flow also lives there now, along
  with the GENESIS and SEND base transaction assembly helpers; shielded
  payment-address generation and payment detection now live in
  `zslp_payment_service`; repeated ZSLP RPC request parsing and token JSON
  rendering are now centralized in controller helpers instead of duplicated
  across handlers

## Longer-Term Checklist

- [ ] introduce `wallet_sync_service` so wallet replay/indexing is no longer controller-shaped
- [ ] introduce `peer_policy_service` for legacy-peer preference and recovery selection
- [ ] introduce `event_projection_service` for asynchronous projection and metrics updates
- [ ] define operator-facing dashboards or RPCs for sync health and recovery state

## Progress Notes

- Keep detailed per-slice notes in `REFACTOR_LOG.md`
- Update this checklist when a phase item changes state
- Prefer small, verifiable slices over wide rewrites
