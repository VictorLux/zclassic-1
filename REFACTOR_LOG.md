# Refactor Log

## 2026-04-02

### Model Lifecycle Hardening: ActiveRecord-First ZSLP And Save Validation

Completed in this slice:

- added `AR_VALIDATE_RECORD` to `app/models/include/models/activerecord.h` so
  model saves can consistently run `before_validate -> validate ->
  after_validate` before `before_save`
- moved the main model save paths onto that shared lifecycle macro:
  `block`, `utxo`, `file_service`, `tx_index`, `mempool_entry`, `peer`,
  `wallet_key`, `sapling_key`, `wallet_tx`, `wallet_utxo`, `sapling_note`,
  and the ZSLP models
- expanded `app/models/src/zslp.c` with a first-class `db_zslp_balance` model,
  validation, before-save token-key normalization, find/save/credit helpers,
  and an emitted model-saved event
- added schema migration `012` in `app/models/src/database.c` so
  `zslp_balances` is a model-owned table instead of an ad hoc service table
- rewired `app/services/src/zslp_service.c` to use `db_zslp_balance_find` and
  `db_zslp_balance_credit` through a transient `node_db` wrapper instead of
  raw balance SQL updates
- added `wallet_script` validation/callback coverage in
  `app/models/src/wallet_key.c` so redeem-script saves follow the same
  lifecycle pattern as the rest of the model layer
- expanded `lib/test/src/test_models.c` with direct model-boundary coverage for:
  wallet-script validation, ZSLP balance validation, token-key normalization,
  and ZSLP balance credit accumulation

Why:

- the app already had ActiveRecord-style callbacks and validations, but the
  save path was still inconsistent across models and some high-value flows
  were bypassing the model layer entirely
- ZSLP balances were the clearest gap: controller/service code cared about
  them, but the schema and persistence rules were not owned by a first-class
  model
- a shared validation lifecycle macro is more DRY, easier for LLM-assisted
  maintenance, and safer than repeating hand-written validate/log/save boilerplate
- before-save normalization on the balance model reduces token-key drift and
  makes mixed-case lookups less fragile

### App Models: Contacts And Onion Announcements

Completed in this slice:

- added `contact` and `onion_announcement` models in `app/models/` with
  validation and save helpers
- added schema migration `013` so `contacts` and `onion_announcements` are
  model-owned tables instead of controller-created tables
- rewired `wallet_view_helpers.c` contact saving to `db_contact_save`
- rewired `wallet_view_send.c` recent-contact loading to `db_contact_recent`
- rewired `blog_controller.c` onion auto-announce tracking to
  `db_onion_announcement_exists` and `db_onion_announcement_save`
- preserved the app behavior while making the persistence rules live in models
- added direct model tests for contacts and onion announcements
- expanded the blog feature test so onion announcements are verified to persist
  a non-empty script payload

Why:

- these were clearly app-level records, but the controllers still owned the
  table creation, validation assumptions, and SQL writes directly
- moving them into models broadens ActiveRecord coverage outside the ZSLP and
  core-node data paths
- it also keeps the Tor/onion-hosted app surface aligned with the same
  controller-thin / model-owned persistence direction as the rest of the app

### Store Models: Product And Order Writes

Completed in this slice:

- added `store` models in `app/models/include/models/store.h` and
  `app/models/src/store.c`
- added schema migration `014` so `products` and `orders` are part of the
  model-owned node DB lifecycle
- rewired `store_controller.c` product seeding from raw `INSERT` statements to
  `db_store_product_save`
- rewired order creation from raw `INSERT` statements to `db_store_order_save`
- rewired payment completion status updates to `db_store_order_mark_paid`
- switched the store controller and payment processor to `node_db_open`
  so model-backed store tables exist cleanly on fresh datadirs
- added direct model coverage for store product/order validation and persistence

Why:

- the store was still one of the clearest controller-owned write surfaces in
  the app layer, even though products and orders are exactly the kind of
  ActiveRecord-friendly app records the architecture is aiming for
- converting the write path first gives us model validations and a cleaner
  persistence seam without forcing a risky rewrite of the read-side store views

### Store Routing: Resource-First REST Aliases

Completed in this slice:

- added resource-first route matching in `store_controller.c` for:
  `/store/products`, `/store/products/:id`, `/store/orders`,
  and `/store/orders/:id`
- kept `/store/product/:id`, `/store/buy/:id`, and `/store/order/:id` as
  compatibility aliases instead of breaking existing links and tests
- updated the store product detail form to post to `/store/orders` with an
  explicit `product_id` hidden field instead of relying only on an action path
  id segment
- tightened order creation parameter handling with explicit positive-integer
  parsing for `product_id`
- added a real order collection endpoint, `GET /store/orders`, so the store
  now exposes collection/member CRUD reads instead of only ad hoc singular pages
- updated canonical store HTML links to point at `/store/products` and
  `/store/orders` resource routes
- expanded store tests to cover the REST collection/member routes directly

Why:

- this keeps the controller moving toward resource-oriented CRUD structure
  without doing a risky top-to-bottom store rewrite
- resource-first routes make the controller easier to reason about and better
  aligned with the broader REST guidance in the architecture docs

### Store Models: Read Layer And Before-Save Normalization

Completed in this slice:

- expanded `app/models/src/store.c` and
  `app/models/include/models/store.h` with read-model APIs for:
  active product lookup/listing, recent order summaries, order detail views,
  and pending-payment projections
- added store model `before_save` normalization hooks so product names,
  descriptions, token ids, customer/payment addresses, and txids are trimmed
  consistently before persistence, with token ids normalized to uppercase
- rewired the main store CRUD resource handlers in
  `app/controllers/src/store_controller.c` to use the store model layer for:
  product collection/show, order collection/show, order create product lookup,
  and pending-payment scanning
- replaced permissive `atoll` resource id parsing in the store controller with
  fail-closed positive-integer parsing for product and order member routes
- expanded `lib/test/src/test_models.c` with store model normalization/read-path
  coverage and expanded `lib/test/src/test_store.c` with explicit `400` checks
  for malformed resource ids

Why:

- the store write path was already model-backed, but the read side was still
  controller-owned SQL, which kept the resource controller fatter than it
  needed to be
- before-save normalization reduces drift and duplicate formatting logic while
  keeping the model layer more ActiveRecord-like
- strict member-id parsing is part of the same REST boundary discipline as the
  earlier tx/address/search validation work: malformed resource identifiers
  should fail before they reach deeper DB or service code

### Store Access Route: Defensive Query Validation And DRY Payment Checks

Completed in this slice:

- tightened the token-gated `/store/access` route in
  `app/controllers/src/store_controller.c` so it now parses and validates
  `addr` and `token` defensively instead of trusting raw query-string bytes
- reused `zslp_service` token/address validators for the access path so the
  store gate follows the same token/address acceptance rules as the rest of the
  ZSLP surface
- changed malformed access requests to fail closed with `400 Bad Request`
  instead of flowing into access checks or HTML rendering
- extracted repeated store payment-processing helpers for:
  chain-tip height lookup and per-address received-payment lookup
- removed repeated `zslp_balance` calls from the gated-content renderer by
  computing the balance once per request
- expanded `lib/test/src/test_store.c` with explicit malformed access-query
  coverage for bad addresses and bad token ids

Why:

- token-gated routes are still part of the power-node app surface and should be
  held to the same defensive boundary standards as REST-style resource routes
- this keeps the store controller DRY while reducing the chance of malformed
  query params reaching deeper wallet/ZSLP logic

### ZSLP Models: Transfer Read Projection And Callback Cleanup

Completed in this slice:

- expanded `app/models/include/models/zslp.h` and
  `app/models/src/zslp.c` with a first-class `db_zslp_transfer_info` read
  projection and `db_zslp_transfer_list_by_token`
- moved token-key normalization behind a dedicated ZSLP token
  `before_save` callback helper instead of hand-inlining the uppercase step in
  the main save path
- replaced the ad hoc one-off balance callback registration with an explicit
  callback-ready helper, keeping the model lifecycle style more uniform and DRY
- added `zslp_service_list_transfers` in
  `app/services/src/zslp_service.c` so transfer reads come through the service
  layer instead of bypassing it
- added `zslp_listtransfers` in
  `app/controllers/src/zslp_controller.c` as a resource-style read endpoint for
  token transfer history
- expanded `lib/test/src/test_models.c` with direct coverage for token-scoped
  transfer projection reads

Why:

- token metadata and balances were already model-backed, but transfer history
  still lacked a clean read projection that controllers/services could depend on
- adding a transfer read model keeps ZSLP moving toward the same CRUD/read-model
  pattern as the store and other app subsystems
- the callback cleanup keeps ZSLP model saves more consistent with the broader
  ActiveRecord-style lifecycle work

### REST API: ZSLP Resource Reads

Completed in this slice:

- added resource-style ZSLP API reads to `app/controllers/src/api_controller.c`:
  - `GET /api/zslp/tokens`
  - `GET /api/zslp/tokens/:id`
  - `GET /api/zslp/tokens/:id/transfers`
- kept the controller thin by routing those reads through model/service-backed
  helpers instead of embedding one-off SQL in the router body
- added defensive validation for token ids and `limit` query params so malformed
  ZSLP API requests fail closed before they hit deeper DB logic
- expanded `lib/test/src/test_api.c` with direct coverage for the new collection,
  member, and transfer-subresource endpoints, including invalid token and limit
  cases

Why:

- ZSLP reads should not be trapped behind RPC-only handlers when the node
  already has a REST API layer for resource-oriented explorer/app data
- this keeps the “controllers more REST” direction concrete and gives the power
  node a cleaner API surface for token-aware MVC apps, onion-hosted or not

### Planning Refresh: Reliability-Focused Phase

Completed in this slice:

- updated `REFACTOR_CHECKLIST.md` to reflect that the service split and major
  runtime/global cleanup are largely complete
- moved the active execution focus from runtime extraction to sync reliability
  hardening and validation
- added an immediate checklist for invariants, metrics, regression tests, and
  legacy-following integration coverage
- updated `ARCHITECTURE.md` so the current state and next hardening targets are
  explicit instead of still reading like an early extraction plan
- audited `app/controllers/src` for remaining hidden runtime/composition state
- identified the highest-signal remaining controller cleanup targets:
  `explorer_controller.c`, `transaction_controller.c`, `mining_controller.c`,
  `hodl_controller.c`, `repair_controller.c`, and the broader
  `sync_controller.c` split
- moved `network_controller.c` off a raw file-local `g_cm` pointer and behind
  an explicit `network_context`
- documented the controller routing rule in `ARCHITECTURE.md`: REST-style
  resource reads by default, explicit command endpoints for mutating node
  workflows
- grouped `explorer_controller.c` runtime, RPC-backend, and asset/template
  state behind explicit structs, with a temporary local alias bridge so the
  large file can be migrated incrementally
- removed the temporary alias usage from the low-risk `explorer_controller.c`
  helper path: disk-cache helpers, stats build input, CSS serving, and favicon
  asset path handling now use explicit explorer context/assets access
- moved the `explorer_controller.c` dashboard and block resource handlers onto
  explicit `explorer_context` access instead of the temporary runtime aliases
- moved the `explorer_controller.c` tx/address/search resource handlers onto
  explicit `explorer_context` access
- tightened explorer read-route validation so txids, addresses, and search
  inputs are rejected early when malformed, overlong, non-hex, or non-printable
  instead of flowing into deeper lookup logic
- moved the remaining factoid/token/HODL DB-backed explorer views onto explicit
  `explorer_context` access and removed the last `g_ms`/`g_ndb`/`g_datadir`
  runtime alias usage from `explorer_controller.c`
- tightened token-detail validation so token IDs must be printable 64-char hex
  before SQLite lookup
- moved `transaction_controller.c` off its file-local runtime globals and onto
  an explicit `rawtx_context` carrying main state, mempool, coins tip,
  datadir, keystore, and connman access
- moved `mining_controller.c` off its file-local runtime globals and onto an
  explicit `mining_context` carrying main state, mempool, coins tip, and
  datadir access
- moved `repair_controller.c` off its file-local runtime globals and onto an
  explicit `repair_context` carrying main state, coins tip, and node DB access
- started Phase 4 reliability hardening by expanding `node_health_service` and
  `/api/health` with compact sync-health metrics:
  header height, peer-best height, tip lag, stale-tip state, download queue
  pressure, and a machine-readable degraded reason
- aligned `healthcheck` RPC with the richer health snapshot and expanded
  `test_node_health_service.c` to cover the new degraded-reason behavior

Why:

- the main architectural excavation is already done enough that the largest
  remaining operational risk is sync reliability, not layer naming
- the plan should track the real bottleneck: proving that `zclassic23` stays in
  sync with legacy `zclassic` under stalls, restarts, and peer churn
- the remaining controller/runtime cleanup is now small enough to track as an
  explicit inventory instead of a vague “more globals remain” note
- routing style should reinforce the service architecture instead of mixing
  resource reads and operational commands under one accidental pattern
- the explorer controller is too large for one safe rewrite, so the setup and
  asset-serving path is being moved first before the heavier block/tx/address
  handlers
- dashboard and block are the highest-signal read resources in the explorer
  surface, so moving them first aligns the cleanup with the intended REST-style
  organization
- REST-style read routes need defensive validation at the boundary, not after
  they have already started touching chain, mempool, or SQLite state
- `explorer_controller.c` now follows the same explicit controller-context
  pattern as the other refactored controllers instead of hiding runtime
  dependencies behind file-local aliases
- `transaction_controller.c` was a compact controller with clustered runtime
  dependencies, so it was a good next target for the same explicit-context
  pattern without needing a transitional alias layer
- `mining_controller.c` is another compact controller with a tight dependency
  set, so it benefits from the same cleanup without expanding the change scope
- `repair_controller.c` follows the same pattern: compact dependency surface,
  operationally important behavior, and a straightforward move to explicit
  controller-owned context
- the next operational bottleneck is understanding why the node is degraded or
  behind without stitching together separate endpoints and logs by hand

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

Recent cleanup and hardening:

- moved `hodl_controller.c` off file-local runtime globals and behind an explicit `hodl_context`
- removed the stale extra `g_coins_db` pointer from `hodl_controller.c` and now derive the backing `coins_view_db` from the injected `coins_tip`
- tightened `gethodlwavetimeline` input validation so `granularity` must be `"day"` or `"month"`
- moved `misc_controller.c` off file-local runtime globals and behind an explicit `misc_context`
- tightened `validateaddress` so it rejects empty, overlong, and non-printable address strings before decode
- moved `file_controller.c` off raw datadir/manifest globals and behind an explicit `file_context`
- tightened file RPC error handling so missing manifest state fails closed instead of returning a bare false
- moved `onion_service.c` off raw onion/datadir/start-time globals and behind an explicit `onion_context`
- expanded the onion landing page to present explorer, store, blog, directory, and status as first-class power-node apps
- exposed Tor/onion readiness in `node_health_service`, `/api/health`, `healthcheck`, and the onion `/status` endpoint
- tightened `zslp_controller.c` around the old ZSLP SDK-style operation split by centralizing token/address validation and shared DB access helpers
- removed the raw ZSLP datadir global in favor of explicit controller context/runtime helpers
- made the ZSLP RPC boundary fail closed on missing runtime context, malformed token ids, malformed addresses, and non-positive amounts
- moved ZSLP token metadata writes off controller/service SQL shortcuts and onto model-backed persistence via `db_zslp_token_save_key`
- added model-backed ZSLP token lookup/list paths, plus direct tests for token normalization, lookup, and listing
- added read-oriented ZSLP RPCs `zslp_gettoken` and `zslp_listtokens`, keeping `zslp_createtoken`, `zslp_mint`, and `zslp_send` as explicit command endpoints
- tightened the ZSLP controller around shared token/address/runtime DB validation helpers so the read side is more resource-like and the command side stays fail-closed
- moved ZSLP create/mint/send request validation into `zslp_service` request validators so the controller and RPC entry points no longer hand-roll the same policy
- added direct tests for the new ZSLP service request validators in the store/ZSLP test surface
- introduced `zslp_command_service` so GENESIS finalization and transfer-balance credits are no longer controller-owned side effects
- moved ZSLP token-finalization persistence and mint/send balance-credit side effects behind `zslp_command_service`
- added direct tests for the new ZSLP command-service seam in the store/ZSLP test surface
- moved the duplicated OP_RETURN prepend + re-sign + commit wallet flow out of `zslp_controller.c` and into `zslp_command_service`
- both GENESIS and SEND now reuse the same wallet-side commit helper instead of maintaining two separate patch/re-sign loops
- moved GENESIS base-tx assembly and SEND base-tx assembly into `zslp_command_service`, so the controller no longer builds those wallet transaction skeletons inline
- introduced `zslp_payment_service` so shielded payment-address generation and payment detection are no longer controller-owned workflow
- moved `zslp_generate_payment_address` and `zslp_check_payment` behind the dedicated payment service boundary
- extracted reusable ZSLP RPC request/response helpers so create/mint/send parsing and token JSON rendering are no longer hand-expanded in each RPC handler
- expanded the Phase 3 cleanup inventory to reflect that `explorer`, `transaction`, `mining`, `repair`, and `hodl` controller context cleanup is now done
- moved wallet-view balance and effective-tip reads behind `wallet_tx` model projection helpers instead of repeating raw SQL in `wallet_view_helpers.c`
- added `db_wallet_projection_summary`, `db_wallet_utxo_balance_with_count`, `db_sapling_note_balance_with_count`, and chain/effective tip helpers so wallet read-side metrics have one model-owned query surface
- added a `before_save` normalization hook to the contact model so address-book writes trim whitespace consistently before validation/persistence
- split the oversized `lib/test/src/test_models.c` file into focused test files:
  `test_models_core.c`, `test_models_zslp.c`, `test_models_app.c`, and `test_models_wallet_projection.c`
- kept `test_models()` as a stable aggregator entrypoint so the harness stays simple while the test code gets smaller and easier to extend
- added `before_save` normalization to the onion-announcement model so onion addresses and script hex are trimmed and lowercased before validation/persistence
- added `db_onion_announcement_recent` so onion registry reads have a model-backed collection path instead of ad hoc controller SQL
- exposed `GET /api/onion/announcements` as a REST-style read resource with bounded `limit` validation and added model/API tests for the new onion read path
- added `before_save` defaults to the file-service model so `p2p_port` falls back to `port` and `last_seen` is timestamped consistently
- tightened `file_service` lifecycle to use the shared ActiveRecord validation macro path instead of hand-rolled validation logging
- exposed `GET /api/file-services` as a REST-style read resource with bounded `limit` validation and added model/API tests for recent file-service reads
- added `before_save` normalization/defaulting to the peer model so `last_seen`, `last_try`, `attempts`, and empty source state are cleaned up consistently before persistence
- tightened peer validation to cover `last_seen`, `last_try`, and `bandwidth_score` ranges through the shared ActiveRecord macro path
- exposed `GET /api/peers` as a REST-style read resource with bounded `limit` validation and added model/API tests for recent peer reads
