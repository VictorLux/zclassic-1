# Revised cutover plan v2 — "make the new path reorg-capable, prove it offline+on a reorg corpus, extract-then-delete"

> **v2 (2026-05-25):** grounded in a 3-agent code investigation. Three findings
> change the plan materially:
> 1. **The new pipeline CANNOT handle a reorg today** — `tip_finalize` logs+skips
>    them; all rollback lives only in the legacy path. Deleting legacy ⟹ the node
>    halts/corrupts on the first mainnet reorg. **This, not the offline driver, is
>    the keystone.**
> 2. The 8-hour full replay is **not a per-commit CI gate** — tier it (cheap
>    structural full-range = CI-able; ~8h Groth16 = a deep/pre-flip gate).
> 3. "Delete 12.5K LOC" is really **"extract 3 live behaviors, then delete the
>    bulk."** One of them is the sacred always-on live-mirror heartbeat.
>
> v1 (below the line in git history) correctly diagnosed the *entanglement* but
> mis-ranked the work and under-counted what deletion would destroy.

## The flaw (unchanged from v1) — correctness entangled with live state

The cutover entangles "is the new consensus path correct?" with "is this one live
node well-connected and is its hand-patched datadir intact?" The shadow stages are
fed ONLY by blocks arriving live at the tip (`shadow_feeder` is called only from
`msg_blocks.c:1166`); the historical chain 0→tip-1 is never replayed through them.
So the correctness proof is a *side effect of live operation* and inherits every
defect of the live node's state (halt ordering, peer floor, torn cold-import
`disk-read-failed`). Meanwhile the path is already bit-clean over 3.1M *headers*
(`validate_headers_log`). The fix is to prove correctness off the live node — but
**headers being clean is not the whole proof**, see Finding 1.

## What the code actually says (the research, with evidence)

| Piece | Status | Evidence |
|---|---|---|
| validate_headers from persisted `nSolution` (M2) | **DONE** | `validate_headers_stage.c:174-263` (`header_from_persisted_block_index`); body-read fallback removed; `block_index_db.c:314` reads nSolution |
| `HAVE_DATA` read-verify helper (M3a) | **DONE** | `disk_block_io.c` `block_index_set_have_data_verified()` reads-back + hash-checks before setting the flag; used by `accept_block.c` |
| `have_data_unreadable` self-heal Condition (M3b) | **DONE** | `app/conditions/src/have_data_unreadable.c` detect/remedy/witness, registered |
| cold-import fenced (M3c) | **GAP** | `legacy_bootstrap_importer.c` `legacy_bootstrap_copy_block_index()` still bulk-copies `BLOCK_HAVE_DATA` with **no** read-verify |
| `cutover_no_forward_progress` guard (M4a) | **DONE** | `app/conditions/src/cutover_no_forward_progress.c` — 180s no-progress ⟹ reverts both stages to SHADOW + pages |
| paired flip atomic (M4b) | **GAP** | `diagnostics_controller.c:855-859` — two sequential `atomic_store`s (header_admit, then validate_headers); tear window |
| one-block canary (M4b) | **MISSING** | described in v1; **no code implements it** |
| **reorg / disconnect in new path** | **MISSING — CRITICAL** | `tip_finalize_stage.c:305-316` logs `reorg_detected` and `return STAGE_ADVANCED` without rollback; real rollback only in `disconnect_tip.c` ← `activate_best_chain.c` (legacy) |
| offline-driver primitives | **ALL EXIST** | `block_log_legacy_open()`+`iter_from()`, `shadow_feeder_observe_block()` (`shadow_feeder.c:64`), `diff_with_legacy_shadow()` byte-exact (`:69`), the 3 counters, injectable `validate_headers_stage_set_validator()` |
| offline replay cost | **~8h** | 107 blk/s (BENCHMARKS_LOG) × 3.1M ⟹ ~29,000s; Groth16 proof-validate dominates. Queue can drop on backpressure ⟹ silent false-converge |

## The revised moves — ordered by what actually blocks a safe delete

### M1 (keystone) — make the new path reorg-capable, and prove reorgs offline
A forward-only 0→tip proof can never prove reorg correctness, and the live shadow
at the tip rarely sees deep reorgs — so this is the gap that surfaces only on
mainnet, after legacy (the only rollback path) is gone. **Before any flip:**
- Give the new path rollback: `tip_finalize` must invoke a disconnect/unwind step
  (port the `disconnect_tip` UTXO unwind + `BLOCK_FAILED` propagation into a stage,
  or keep `activate_best_chain`'s reorg handler reachable as the new path's
  rollback) instead of logging+skipping at `tip_finalize_stage.c:305`.
- The offline proof (M2) MUST include a **reorg corpus**: build branch A to height
  h, then a heavier branch B from h-k; assert the new path disconnects A, connects
  B, unwinds UTXOs, and the resulting state byte-matches legacy. No reorg corpus ⟹
  no proof, regardless of how clean the linear replay is.

### M2 — offline replay-proof, TIERED, paced, drop-proof
Build the thin driver (~500 LOC, all primitives exist): iterate 0→tip from a
read-only archive via `block_log_legacy_open()`+`iter_from()` → `block_deserialize`
→ `shadow_feeder_observe_block()`; then assert `diff_with_legacy_shadow == CONVERGED`
and the three counters `== 0`.
- **Tier 1 (CI-able, minutes):** structural — Equihash from `nSolution` over full
  0→tip + `utxo_apply` delta + `tip_finalize` count diffs + byte-range diff. These
  are cheap and order-independent. **Proof-integrity guard: assert
  `blocks_fed == blocks_diffed`** so a backpressure drop can't masquerade as
  convergence. Pace the feeder to queue depth (never saturate the mutator queue).
- **Tier 2 (deep gate, ~8h, nightly + pre-flip):** full `script_validate` +
  `proof_validate` (Groth16) over the full range, OR FlyClient-style sampled N at
  the existing ≥150-bit model (the codebase already has this security primitive).
- Emits one artifact: *"0 divergences (incl. reorg corpus) across N blocks, commit
  <sha>, datadir <fingerprint>."* Independent of peer count and torn live state.
- **Correction kept from v1:** do NOT use Phase 6 (`make chaos` / seed_tape) —
  `tools/sim/chaos.c` uses STUBBED consensus (counter bumps); it cannot prove block
  equivalence.

### M3 — finish the HAVE_DATA invariant: fence cold-import
The helper + self-heal Condition are done. Remaining: route
`legacy_bootstrap_copy_block_index()` through `block_index_set_have_data_verified()`,
**or fence it** — don't set `HAVE_DATA` on import; let `body_fetch` re-pull + verify
on demand. Fencing is consistent with the standing rule that cold-import is the
wrong recovery tool (it stops `zclassicd` and left a torn datadir). Strategic
direction unchanged: canonical bootstrap is FlyClient + SHA3 snapshot
(hash-verifies every chunk before activation); retire cold-import.

### M4 — flip atomically behind a REAL canary, then extract-then-delete
**Flip mechanics (fix the two gaps first):**
- Make the paired flip atomic: a single combined-mode store (or seqlock) for
  `header_admit`+`validate_headers`, closing the tear window at
  `diagnostics_controller.c:855`.
- **Build the canary** (it does not exist): flip ⟹ watch exactly one block connect
  through the new path authoritative ⟹ auto-revert on any divergence or no-progress.
  The `cutover_no_forward_progress` guard already reverts on 180s-stall; add the
  explicit single-block success gate on top.
- Demote the 24h-per-stage soaks to a **post-flip** confidence/deletion gate.

**Extract-then-delete (the deletion is NOT a clean delete):** three modules carry
unique behavior the new path lacks. Order, and what must survive each:
1. **Never delete the live-mirror heartbeat.** `legacy_mirror_sync` IS the
   always-on live-sync-from-`zclassicd` heartbeat + lag-SLO monitor
   (`boot_services.c:1049`). Extract that to a lean monitor; delete only its
   block-application *coordination*, never the heartbeat. (Owner rule:
   [[feedback_services_linger_live_sync]].)
2. **Extract source-selection policy.** `chain_advance_coordinator` scores
   P2P/mirror/snapshot/import; the new stages assume pre-selected blocks. Move the
   policy into `header_probe` or a small `block_source_policy` before deleting.
3. **Re-home recovery primitives, especially `clean_above_tip`.**
   `utxo_recovery`'s `clean_above_tip` (orphan-UTXO heal after a dirty reorg),
   `restore_chain_tip`, and `import_ldb`. `clean_above_tip` is **exactly what the
   recurring boot-integrity FATAL needs** — turn it into a Condition (the framework
   shape) before deleting `utxo_recovery`. (boot-only callers: `boot.c:2438/2561/
   2752/3274`.)
4. **THEN delete the bulk**, in order: `utxo_recovery` → `chain_advance_coordinator`
   → `legacy_mirror_sync` (coordination only). **Net deletion is < 12.5K LOC** —
   some logic re-homes. Report the honest post-extraction count, not the gross.

## Net (revised)

The real keystone is **reorg-capability**, not the offline driver: a forward-only
proof plus a delete of the only rollback path = a node that halts on the first
reorg. So: (M1) give the new path rollback and prove it on a reorg corpus; (M2)
prove the linear path offline over the full chain, tiered so the cheap structural
proof is CI-able and the 8h crypto proof is a deep/pre-flip gate, with a
`blocks_fed == blocks_diffed` guard so drops can't fake convergence; (M3) fence
cold-import to finish the HAVE_DATA invariant; (M4) make the paired flip truly
atomic, build the canary that's only described, and **extract-then-delete** —
never touching the always-on live-mirror heartbeat. Connectivity (peer floor)
still gates only the final live flip, which is correct.

## Critical files (with evidence file:lines)
- new driver → `adapters/inbound/src/shadow_feeder.c:64`,
  `adapters/outbound/persistence/.../block_log_legacy.c` (`open`/`iter_from`),
  `application/operations/src/diff_with_legacy_shadow.c:15`,
  `tools/bench_fresh_sync.c` (launch model)
- **reorg (M1)** → `app/services/src/tip_finalize_stage.c:305-316` (logs+skips —
  must gain rollback), `lib/validation/src/disconnect_tip.c`,
  `lib/validation/src/activate_best_chain.c`
- M3 fence → `app/services/src/legacy_bootstrap_importer.c`
  (`legacy_bootstrap_copy_block_index`), `lib/storage/src/disk_block_io.c`
  (`block_index_set_have_data_verified`)
- M4 atomic flip → `app/controllers/src/diagnostics_controller.c:855-859`;
  guard `app/conditions/src/cutover_no_forward_progress.c`
- extract-then-delete → `app/services/src/legacy_mirror_sync*.c` (heartbeat — keep),
  `app/services/src/chain_advance_coordinator.c` (policy — extract),
  `app/services/src/utxo_recovery*.c` (`clean_above_tip`/`restore_chain_tip`/
  `import_ldb` — re-home)
