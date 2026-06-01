# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.

State at handoff: active framework-refactor main worktree. Verify with
`git status --short --branch` before editing.

---

## Current truth

The reducer/staged path is now the authoritative chain-advance architecture.
The old comparison/cutover RPC surface has been removed from the public tool
set, and the legacy block-connect engine files are gone. The remaining refactor
work is cleanup and hardening:

1. Keep stale shadow/cutover scaffolding out of production; current production
   C/H searches are clean, and `docs/work` now contains only the
   parallel-worktree protocol. The small-projection legacy table comparison
   helpers now live only in tests, not the production storage API.
2. Move every remaining oversized or mixed-purpose app file into one framework
   shape.
3. Shrink the ratchet baselines to zero.
4. Keep docs honest with the reducer-as-authority architecture.
5. Prove the result with clean build/lint/tests and a live node soak.

`docs/FRAMEWORK.md` is the architecture. `docs/REFACTOR_STATUS.md` is the live
debt board.

---

## Do Not

1. Do not weaken a lint gate or grow a baseline.
2. Do not restore deleted cutover/projection-diff/public shadow tooling.
3. Do not stop `zclassicd-rhett`; manage long-running services through
   `systemctl --user`.
4. Do not treat green unit tests as a live-node proof. The final bar includes
   forward progress on the running node.
5. Do not move the local `zclassic23` P2P listener back to `8033`; the active
   dev node is on `8023` to avoid a `zcashd` port conflict.

---

## First 5 Minutes

```bash
git status --short --branch
make lint
touch lib/test/src/test_parallel.c && make test_parallel && ./test_parallel
./tools/zcl-rpc getblockcount
```

If the node is not running, record that explicitly before claiming live proof.

---

## Current High-Value Targets

- E1 oversized files:
  `tools/scripts/file_size_ceiling_baseline.txt` is empty. Keep it empty; do
  not add new grandfathered oversized app files.
- E2 service-result files:
  `tools/scripts/one_result_type_baseline.txt` is empty. Keep it empty; migrate
  legacy bool compatibility call sites to `struct zcl_result` as adjacent files
  are split or touched.
- E6 write-path debt:
  controller/admin `coins_view_cache_flush`, coins.db batch writers,
  process-block flush-policy write paths, and the grandfathered
  `active_chain_set_tip()` compatibility wrapper. Current E6 baseline:
  24 write surfaces.
- Lib-layering debt:
  `tools/scripts/lib_layering_baseline.txt` is down to 32 grandfathered
  lib-to-app includes after moving file manifest protocol declarations into
  `lib/net/include/net/file_manifest.h`, moving generic node DB path building
  into `lib/util`, moving UTXO script classification into `lib/script`,
  replacing a net internal service include with a forward declaration, moving
  schema migration into the Model shape, and moving file-offer SQLite
  persistence into the FileOffer model, and moving ZMSG SQLite persistence
  into the Zmsg model. ZNAM at-rest records and SQLite persistence now live in
  the Znam model instead of the lib protocol parser, and swap-contract
  persistence now lives in the SwapContract model instead of the HTLC script
  builder/parser. Addrman sidecar integrity now lives in `lib/net`, backed by
  the generic `lib/storage` SHA3 sidecar helper, so `connman.c` no longer
  includes an app service for peers.dat integrity. Mining found-block
  submission is now caller-owned through a `gen_context` callback, so
  `lib/mining` no longer includes the app activation service. Connman onion
  peer discovery is now callback-injected from boot, so `lib/net/src/connman.c`
  no longer includes the blog controller. Onion service blog serving and peer
  discovery are now callback-injected app handlers registered by boot, so
  `lib/net/src/onion_service.c` no longer includes the blog controller.
  Compact-block reducer submission is now callback-injected from boot, so
  `lib/net/src/msg_compact.c` no longer includes the activation service.
  Handshake peer persistence is now callback-injected from boot, so
  `lib/net/src/msg_version.c` no longer includes the Peer model/database
  headers. Metrics service/model gauges and connman known-ZCL23 peer
  selection are now callback-injected from boot, so `lib/metrics/src/metrics.c`
  and `lib/net/src/connman.c` no longer include those app-layer headers. Tx
  wallet persistence and the snapshot-active check are now callback-injected
  from boot, so `lib/net/src/msg_tx.c` no longer includes app
  controller/model/service headers. P2P block reducer submission is now
  callback-injected from boot, and `lib/net/src/msg_blocks.c` uses the
  injected snapshot-active check, so that file no longer includes app
  controller/model/activation/snapshot headers. Block-connected tip observers
  are now callback-injected from boot too, so `msg_blocks.c` no longer includes
  the sync monitor service. Block-sync planning for invalid-block retries and
  valid-block acceptance is now hidden behind net-internal helpers, so
  `msg_blocks.c` has no remaining app-service includes. Stale unused
  app-layer includes were removed from `msg_headers.c`, `msgprocessor.c`, and
  `msgprocessor_snapshot.c`; FlyClient proof building is now callback-injected
  from boot, so the net snapshot handler no longer includes the blockchain
  controller or MMB leaf-store model. Keep shrinking it; do not add new
  entries.
- Controller raw-SQL debt:
  `tools/lint/no_raw_sqlite_in_controllers_baseline.txt` is empty after
  routing wallet scan / legacy import exec helpers,
  snapshot controller exec helpers, wallet shielded height fallback, and
  repair-height UTXO queries through models / `node_db_exec()`, plus moving
  `repairutxos` transaction control to `node_db_*()` and Sapling tree block
  writes to the Block model, and moving sync-import UTXO cardinality validation
  to the UTXO model. Wallet key readback/rollback now routes through
  `wallet_sqlite`, MMR/MMB state persistence routes through `node_db_state_*`,
  and consensus snapshot export moved to
  `consensus_snapshot_export_service`. `importchainstate` derived wallet /
  address cache rebuilds and imported-value reporting now live on the UTXO /
  wallet models. The tx-index block-position scan and additive-build PRAGMAs
  now live on Block / TxIndex model helpers, and the diagnostic SQL primitive
  prepares statements through `node_db_prepare_readonly_query()`. Keep this
  baseline empty.
- Raw allocation debt:
  `tools/scripts/raw_malloc_allowlist.txt` has no active entries. Keep
  production allocations on `zcl_malloc` / `zcl_calloc` / `zcl_realloc` unless
  a local raw-alloc exception is explicitly justified.
- Supervisor debt:
  `tools/scripts/supervisor_baseline.txt` is empty; keep it that way.
- Typed-blocker debt:
  `tools/scripts/typed_blocker_baseline.txt` is empty; keep it that way.

Default to subtraction. A file that exists only to preserve the old cutover or
shadow comparison world should be deleted or moved into tests.
