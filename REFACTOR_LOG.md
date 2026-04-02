# Refactor Log

## 2026-04-01

### Slice 1: Health Service Extraction

Completed:

- created `app/services`
- added `node_health_service`
- moved health snapshot assembly out of controllers
- unified `/api/health` and `healthcheck` around one service

Why:

- remove duplicated status logic
- establish `services` as a first-class layer

### Slice 2: Sync Service Extraction Start

Completed in this slice:

- added `sync_service`
- moved sync kickoff policy out of `msgprocessor`
- moved header re-request timing policy out of `msgprocessor`
- moved IBD/tip progress snapshot assembly out of `msgprocessor`
- moved stall detection scan and recovery-plan building out of `msgprocessor`
- moved stall recovery execution for queue/reset mutations out of `msgprocessor`
- moved recovery header-anchor selection out of `msgprocessor`

Still inside `msgprocessor` for now:

- actual wire sends
- outbound `getheaders` wire action after recovery
- snapshot protocol handling

Next target:

- extract snapshot sync orchestration into a service boundary
- keep converting `msgprocessor` into a protocol adapter that delegates sync policy

### Slice 3: Snapshot Service Namespace Cleanup

Completed in this slice:

- moved `snapshot_sync_service` from controller namespace to service namespace
- updated `api_controller` and `msgprocessor` to include the service header
- aligned code layout with the architecture plan
- moved snapshot handshake follow-up policy behind service helpers
- moved snapshot offer / FlyClient parse-write helpers behind service helpers
- moved FlyClient proof building / serialization behind service helpers
- moved verified snapshot-tip activation behind a service helper
- moved snapshot serve-step chunk framing / progress advancement behind a service helper
- moved getheaders log policy and stale-tip warning throttle behind sync service
- moved peer block-assignment sizing policy behind sync service
- moved peer block-assignment execution behind sync service
- moved block `getdata` payload serialization into protocol helper
- moved `getheaders` payload serialization into protocol helper
- moved valid-block sync transition and header-chain eligibility behind sync service
- moved accepted-header block collection / queue preparation behind sync service
- moved accepted-headers log throttle behind sync service
- moved header-batch warning / follow-up decisions behind sync service
- moved one-shot post-header block-file scan trigger behind sync service
- moved accepted-header download planning behind a single sync-service step
- moved header-processing follow-up behind a composite sync-service plan
- aligned header continuation requests with upstream `zclassic` full-batch behavior
- moved `getheaders` locator construction behind sync service
- moved periodic and stale-tip `getheaders` resend decisions behind sync service
- moved stall-recovery `getheaders` resend decision behind sync service
- unified `getheaders` execution call sites behind one router helper
- moved invalid-block header retry decision behind sync service
- moved header-processing chain-activation decisions behind sync service
- moved snapshot offer/end transition results behind snapshot sync service
- moved snapshot serving-start transition result behind snapshot sync service
- moved snapshot offer follow-up action selection behind snapshot sync service
- added unit coverage for snapshot follow-up and PoW request building
- added unit coverage for snapshot stream parse/write helpers
- added unit coverage for FlyClient response roundtrip and tip activation
- added unit coverage for snapshot serve-step progression
- added unit coverage for sync header-log and stale-tip warning policy
- added unit coverage for block-assignment planning policy
- added unit coverage for block-assignment execution
- added unit coverage for block `getdata` serialization
- added unit coverage for `getheaders` serialization
- added unit coverage for valid-block transition and header-chain policy
- added unit coverage for accepted-header block collection and activation fallback
- added unit coverage for accepted-headers log throttling
- added unit coverage for header-batch follow-up decisions
- added unit coverage for post-header block-file scan trigger
- added unit coverage for composite header-download planning
- added unit coverage for composite header-processing planning
- updated header-batch tests for upstream-style continuation policy
- added unit coverage for `getheaders` locator construction
- added unit coverage for periodic and stale-tip `getheaders` actions
- added unit coverage for stall-recovery `getheaders` action planning
- added unit coverage for invalid-block `getheaders` retry planning
- added unit coverage for header-processing activation policy
- added unit coverage for snapshot transition results
- expanded snapshot transition-result coverage for serving start
- expanded snapshot transition-result coverage for offer follow-up selection
- moved more header-processing execution decisions behind sync service
- expanded snapshot transition-result coverage for FlyClient verify follow-up
- moved valid-block catch-up transition planning behind sync-service result structs
- moved snapshot verified-end and serve-complete peer/sync transition planning behind snapshot-service result structs
- expanded transition-result coverage for snapshot serve completion
- moved header-path chain activation behind explicit sync-service activation results
- moved snapshot offer-acceptance and serve-start application behind explicit snapshot-service result fields
- introduced `header_sync_service`
- moved header locator building, header batch planning, and header-path activation into `header_sync_service`
- introduced `block_sync_service`
- moved block assignment, progress snapshots, stale-tip watchdog logic, stall recovery, and catch-up transitions into `block_sync_service`
- switched `msgprocessor` to direct `header_sync_service` and `block_sync_service` dependencies
- reduced `sync_service` to a compatibility umbrella instead of an active logic owner
- introduced `app_runtime_context` for `msgprocessor`
- removed `g_active_node_db` dependency from `msgprocessor` by threading runtime-owned node-db access through boot
- removed `g_active_wallet` dependency from `msgprocessor` by using the injected runtime wallet
- extended runtime-owned dependency access into `boot_services`
- moved startup, RPC wiring, snapshot prebuild, SQLite catchup, and shutdown persistence paths in `boot_services` from raw `g_active_node_db` reads to runtime-owned accessors
- made `boot_services` the visible owner of runtime wiring for node DB, mempool, and wallet compatibility handoff
- added a shared runtime registry in `config/runtime.c`
- set and cleared the shared runtime registry from `boot_services`
- moved `process_block` wallet/node-db/mempool access onto the shared runtime context
- moved `connman` ZCL23 peer-preference DB access onto the shared runtime context
- moved `api_controller` health and wallet endpoints off direct node-db globals
- moved `zslp_controller` wallet/mempool signing and broadcast paths off direct globals
- replaced `boot.c` internal node-db alias reads with direct boot-owned state
- reduced remaining `boot.c` `g_active_*` usage to compatibility alias assignment only
- isolated `boot_services` compatibility alias publish/clear behind explicit helpers
- documented runtime registry lifecycle and composition-owned runtime state in `config/` headers
- removed dead `g_active_wallet` and `g_active_mempool` compatibility aliases
- moved the last controller-side sapling reset call off `g_active_node_db`
- removed the final live `g_active_node_db` compatibility alias
- split `app_shutdown_svc` into explicit shutdown phases in `boot_services`
- documented shutdown phase order in `boot_internal.h`
- updated architecture notes to make startup/shutdown ownership explicit in `config/`
- centralized wallet controller shared state into `wallet_rpc_context`
- moved wallet controller setters to populate the shared wallet context instead of separate globals
- added typed `wallet_rpc_context` accessors in `wallet_helpers.h`
- moved `wallet_controller.c` off legacy wallet compatibility macros and onto direct `wallet_rpc_context` access
- moved `wallet_helpers.c` off legacy wallet compatibility macros and onto typed wallet-runtime accessors
- moved `wallet_shielded_controller.c` off legacy wallet compatibility macros and onto direct `wallet_rpc_context` access
- moved `wallet_rescan_controller.c` off legacy wallet compatibility macros and onto direct `wallet_rpc_context` access
- moved the low-risk scan/reindex/import/db-info entry points in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- moved `getwalletaccounting` and `removestalletxs` in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- moved `walletaudit`, `getchaincoins`, `traceutxo`, and `listwalletkeys` in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- moved `listwallettxdetail` and `getbalanceflow` in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- moved `reconcilewalletutxos` and `purgephantomutxos` in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- moved `diagnoseutxos` and `walletledger` in `wallet_diagnostic_controller.c` onto direct `wallet_rpc_context` access
- kept the compatibility macros in `wallet_helpers.h` for the remaining wallet view/controller surfaces only
- removed the legacy wallet-helper compatibility macros from `wallet_helpers.h`
- switched `misc_controller.c` to the typed wallet context accessor that the removed macro had been masking
- removed the stale macro-undef workaround from `transaction_controller.c`
- replaced the file-local composition globals in `chain_inspect_controller.c` with an explicit `chain_inspect_context`
- centralized `blockchain_controller.c` state behind a single `blockchain_context` instead of separate file-local service/datadir globals
- centralized `api_controller.c` state behind explicit runtime and RPC-backend structs instead of separate file-local globals
- tightened `blockchain_controller.c` further so the normal chain RPC and chainstate-rebuild paths use explicit `blockchain_context` access directly
- removed the final internal node-db alias from `blockchain_controller.c`; the legacy bulk import/index pipeline now also uses explicit `blockchain_context` access

Why:

- snapshot sync is a long-lived workflow and state machine, not a controller
- this makes the service layer explicit before deeper router extraction
- runtime composition is now visible in `config/` instead of being inferred from globals in the message processor and service bootstrap path
- core runtime consumers can now use injected app dependencies without each subsystem inventing its own global handoff
- controller-facing app features are now using explicit controller/runtime state instead of reaching directly into legacy globals
- boot-time composition decisions now use the state they own directly instead of routing through compatibility aliases
- the remaining compatibility layer is now visibly transitional instead of being mixed into normal startup/shutdown flow
- runtime access is now explicit in code instead of flowing through legacy `g_active_*` aliases
- shutdown ownership is now encoded in named phases instead of hidden inside one monolithic teardown function
- wallet-controller composition is now one object instead of a pack of unrelated globals, which makes the next migration steps narrower and safer
- the main wallet RPC controller now reads explicit controller context directly, which reduces hidden coupling before tackling the shielded and view controllers
- the shared wallet helper layer now reads typed context accessors directly, which narrows the remaining macro bridge to the still-unmigrated wallet controllers
- the shielded RPC controller now uses explicit wallet runtime context too, which leaves the wallet view layer as the main remaining macro bridge
- the rescan/import/witness RPC surface now uses explicit wallet runtime context too, which leaves diagnostics and any residual view wiring as the main remaining compatibility-heavy surfaces
- the diagnostic controller is too large for a single safe rewrite, so its low-risk operational entry points are now migrated first and the deeper audit/accounting RPCs remain for subsequent slices
- the diagnostic controller macro count is now reduced incrementally in bounded RPC groups instead of one risky rewrite
- the diagnostic controller macro count is now down from 81 to 51 after migrating the main audit/inspection read paths
- the diagnostic controller macro count is now down from 51 to 38 after migrating the next transaction-history and balance-flow read paths
- the diagnostic controller macro count is now down from 38 to 13 after migrating the main mutating repair/reconcile paths
- `wallet_diagnostic_controller.c` is now fully off the legacy wallet-helper macro bridge
- the wallet-controller surface is now fully off the legacy wallet-helper macro bridge, so `wallet_helpers.h` only exposes typed context accessors and helpers
- the same explicit controller-context pattern is now applied to `chain_inspect_controller.c`, reducing another non-wallet controller’s hidden composition state
- `blockchain_controller.c` now has one backing composition object as well, which narrows the remaining non-wallet controller cleanup to explicit context access rather than disconnected globals
- `api_controller.c` now follows the same composition direction, with request-time node/datadir dependencies and RPC backend settings grouped into explicit controller-owned state
- `blockchain_controller.c` still keeps one temporary node-db alias for the bulk legacy indexer/import pipeline, but the ordinary RPC and reindex paths are now off the internal alias macros
- `blockchain_controller.c` is now fully off its internal composition aliases, so the entire controller hangs off one explicit backing context

Planning update:

- added `REFACTOR_CHECKLIST.md`
- documented phased execution plan for MVC + services + sync hardening
- added a living checklist for progress tracking

Next target:

- make runtime composition and lifecycle even more explicit in `config/`
- keep reducing implicit composition patterns that are not globals anymore but are still scattered
- migrate the remaining wallet view/controller surfaces off compatibility macros onto direct context access incrementally
