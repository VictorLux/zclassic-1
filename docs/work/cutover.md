# Cutover — collapse to ONE chain path

> **The single source for the cutover.** Replaces the former 5-doc spread
> (`cutover-revised-plan`, `cutover-safety-protocol`, `wt-c3-reflip`,
> `wt-phase2-cutover-*`, `wave-s-cutover`) and the per-module dissolve plans.
> Grounded in a 3-agent code review (2026-05-25).

## Goal

Two chain paths run today: the legacy `connect_tip` path (drives the live tip)
and the Wave-S **stage pipeline** (`header_admit → validate_headers → body_fetch
→ body_persist → script_validate → proof_validate → utxo_apply → tip_finalize`),
which currently runs in **shadow**. Robustness = **ONE path.** When the stages
are authoritative and the legacy modules are deleted (3 modules = 4,407 LOC + comparison scaffolding), every step
must "advance the durable cursor or name a typed blocker" — a silent halt becomes
unreachable by construction.

**This is a single personal node, not a fleet.** No multi-week, per-stage
soak ceremony, no C-2…C-9 bureaucracy. Three steps:
**prove offline → flip once → delete everything.**

## 1. PROVE (offline, on a complete read-only archive)

The shadow stages are fed only by blocks arriving live at the tip, so today's
"proof" inherits every defect of the live node's state (peer floor, torn
datadir, boot ordering). Prove correctness **off** the live node instead.

- **Reorg is the keystone, not the linear replay.** The new path CANNOT roll back
  a reorg yet — `tip_finalize_stage.c:305` logs `reorg_detected` and skips;
  all rollback lives only in legacy `disconnect_tip` ← `activate_best_chain`.
  A forward-only proof + deleting the only rollback path = a node that halts on
  the first mainnet reorg. **First:** give `tip_finalize` a disconnect/unwind
  step (port `disconnect_tip` UTXO unwind + `BLOCK_FAILED` propagation), and the
  proof MUST include a **reorg corpus** (branch A, heavier branch B; assert
  disconnect+reconnect+UTXO-unwind byte-matches legacy).
  - Progress: `tip_finalize` successful rows now persist the finalized tip hash,
    detect a coherent active-chain fork on restart/step, rewind the durable cursor
    to the last matching finalized row, and replay from the fork boundary. The
    remaining hard part is still the actual disconnect/UTXO unwind parity proof.
- **Tiered replay driver** (~500 LOC over existing primitives: `block_log_legacy_open`
  + `iter_from` → `shadow_feeder_observe_block` → the three divergence counters +
  `diff_with_legacy_shadow`):
  - *Tier 1 (CI, minutes):* Equihash from persisted `nSolution` over full 0→tip +
    `utxo_apply` delta + `tip_finalize` count diffs + byte-range diff. Assert
    `blocks_fed == blocks_diffed` so a backpressure drop can't fake convergence.
  - *Tier 2 (deep, ~8h nightly/pre-flip):* full `script_validate` + `proof_validate`
    (Groth16), or FlyClient-sampled N at the existing ≥150-bit model.
  - NOT `make chaos` — its consensus is stubbed; it can't prove block equivalence.
  - Progress: `shadow_replay_proof` now provides the offline byte-replay proof
    skeleton: primary block log → shadow block log → byte diff, with
    `blocks_fed == blocks_diffed` required for a passing proof. The CLI can run
    it against a read-only legacy zclassicd datadir and an empty shadow log.
- Emits one artifact: *"0 divergences (incl. reorg corpus) across N blocks, commit
  <sha>, datadir <fingerprint>."* Independent of peer count and live state.

## 2. FLIP (once, behind the guard)

- **Decouple header validation from bodies** (DONE, `84de4465d`): `validate_headers`
  verifies Equihash from persisted `nSolution`, not block bodies — mandatory for
  the 30s FlyClient/SHA3 cold-sync too (a snapshot-synced node has no bodies).
- **`BLOCK_HAVE_DATA` = read-verified** (DONE): `block_index_set_have_data_verified()`
  + the `have_data_unreadable` self-heal Condition. **Remaining:** fence cold-import
  (`legacy_bootstrap_copy_block_index` still bulk-copies the flag unverified) — or
  retire it; canonical bootstrap is FlyClient + SHA3 snapshot (hash-verified).
- **Atomic flip** (DONE): header_admit + validate_headers now share one
  `cutover_modes` atomic bitfield, and `cutovermode all` / the no-progress
  auto-revert use one combined pipeline store instead of two sequential mode
  writes.
- **Real canary** (PARTIAL): `cutovermode all authoritative` records
  `tip_height + 1`, and `cutover_canary_complete` auto-reverts both stages to
  SHADOW after that target block connects. **Remaining:** tie the one-block
  pass to explicit authoritative-path evidence/divergence checks before the
  final guarded flip. The `cutover_no_forward_progress` guard (DONE,
  `230d9b896`) still reverts both stages to SHADOW on 180s no-progress and fires
  `EV_OPERATOR_NEEDED` → alert sinks + `zcl_status` DEGRADED + sd_notify. Worst
  case: 3-min stall, auto-revert, you get paged.

## 3. DELETE (extract-then-delete — it is NOT a clean delete)

Three legacy modules carry unique behavior the new path lacks. Re-home first,
then delete the bulk. Net: the 3 modules = 4,407 LOC measured (some logic
re-homes), plus the shadow-vs-legacy comparison scaffolding.

1. **Never delete the live-mirror heartbeat.** `legacy_mirror_sync` IS the
   always-on live-sync-from-`zclassicd` heartbeat + lag-SLO monitor. Extract that
   to a lean monitor; delete only its block-application *coordination*.
2. **Extract source-selection policy** (`chain_advance_coordinator` scores
   P2P/mirror/snapshot/import) into `header_probe` or a small `block_source_policy`.
3. **Re-home recovery primitives** (`utxo_recovery`: `clean_above_tip` orphan-UTXO
   heal — what the boot-integrity FATAL needs — `restore_chain_tip`, `import_ldb`);
   make `clean_above_tip` a Condition before deleting.
4. **Then delete the bulk** + the shadow-vs-legacy comparison scaffolding
   (`diff_with_legacy_shadow`, `shadow_feeder`, the `*_projection_diff` tools, the
   cutover mode/preflight/canary plumbing). Once there is one path, the entire
   comparison apparatus is dead.

## Net

Reorg-capability is the real prerequisite; the offline proof banks correctness off
the live node; the flip is one guarded act; the delete removes the legacy path AND
the comparison apparatus that only exists because two paths coexist. Connectivity
(peer floor) gates only the final live flip.
