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
  controller/admin/repair/shutdown `coins_view_cache_flush`, the remaining
  `coins_view_sqlite_batch_write_ex()` SQLite writer entry point,
  and process-block flush-policy write paths. Current E6 baseline:
  13 write surfaces.
- Process-block split debt:
  `lib/validation/src/process_block_core.c` is smaller after moving runtime
  hook dispatch, failed-child propagation, and block-index disk
  placement/hydration, tip-publication evidence/commit mechanics, and
  active-tip child discovery/disk verification into purpose-named validation
  files. It now carries chain selection and contextual-header skip logic.
- Lib-layering debt:
  `tools/scripts/lib_layering_baseline.txt` is empty. The final baseline entry
  was removed by moving the snapshot-sync router contract to
  `lib/net/include/net/snapshot_sync_contract.h` and leaving
  `app/services/include/services/snapshot_sync_service.h` as a compatibility
  wrapper for app callers. Keep this baseline empty; do not add new upward
  includes from `lib/` to `app/`.
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
