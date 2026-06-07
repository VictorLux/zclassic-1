# Chain-Tip-Durability Collapse — the wedge-class kill

Status: **GO** (adversarially verified, 9-agent workflow 2026-06-07). Supersedes
the prior task#2 "convergent" and "layered" designs and the rejected
"collapse-onto-utxo_apply-cursor" variant.

## The disease (every tip-wedge for weeks)

The served chain tip / "coins applied through N" was recorded **redundantly** in
multiple stores with **no atomic cross-commit**, so any crash/recovery drifts
them apart and boot *guesses* which is right. Live 4-way split:
`coins_best_block HASH`=3134313 / `cec.coins_best_block_height` INT=3134559 /
`utxo_apply` cursor=3134559 / `utxos MAX`=3132687. The guessing is the bug factory.

## The single source of truth (verified)

**`tip_finalize`'s contiguous finalized frontier** — `g_last_advance_height`
(RAM) backed by `tip_finalize_log` (the row + the stage cursor commit in ONE
`stage_run_once` `BEGIN IMMEDIATE` txn, `stage.c:316-356`), reconciled at boot to
the contiguous `ok=1` prefix. `tip_finalize` is NOT a redundant durability stage:
it is the ONLY place the tip is published, gated FIRST by reorg detection
(`tip_finalize_stage.c:324`), the must-never-fork chainwork-greater check (`:367`),
and UTXO conservation (`:389`), and it runs MMB/FlyClient + wallet/Sapling/mempool
side-effects in lockstep (`:412,428`).

Rejected alternatives (both NO-GO, code-confirmed): `coins_best_block` HASH is
**dead-legacy** on the live path (`process_block` has zero live callers) and is
NOT atomic with the live coins (they fold via `event_log_append` with its own
fsync, separate file); the `utxo_apply` cursor `U` advances PAST failed blocks
(`utxo_apply_stage.c:213-216,296-297`) and is not atomic with the coins either.
Collapsing onto either formalizes a torn-write / tip-onto-rejected-block window.

## RIP-OUT (pure subtraction)

- DELETE state key `cec.coins_best_block_height` + all writers:
  `chain_evidence_authority_service.c:635`, `chain_evidence_reconstruct.c:200`,
  `utxo_recovery_service.c:192`.
- DELETE Guard A `chain_evidence_clamp_coins_height_to_frontier`
  (`chain_evidence_authority_service.c`) — it guards a store that no longer
  exists as an authority. (Shipped 7550868df as a stepping stone; the collapse
  removes the store instead.)
- KEEP (load-bearing, do NOT delete): `tip_finalize` / `tip_finalize_log` /
  `g_last_advance_height` / `tip_finalize_run_post_finalize`; the anchor
  hash-convention carve-out; `utxo_apply` advance-on-failure (deadlock guard —
  `tip_finalize` is the gate that refuses to publish onto those failures).

## REWIRE (minimal)

- Boot reconcile clamp floor (`boot.c:3330-3348`): change from the poisoned
  `cec` int to the **genuine coins frontier = height(coins_best_block HASH)**
  (resolver `coins_view_sqlite_get_best_block` + `block_map_find`, already used
  at `boot.c:3155-3164`). `stage_reconcile_clamp_tip_finalize_to_floor` is
  UNCHANGED (deletes no `tip_finalize_log` rows). Under-rewind is SAFE (forces
  more forward re-finalization, never publishes ahead).
- Repoint surviving `cec` diagnostic readers to a value derived from the
  finalized frontier, or drop the field: `event_controller.c:53`,
  `diagnostics_registry.c:346`, `chain_evidence_snapshot.c:96`, the header field;
  fix tests `test_syncdiag_rpc.c:707`, `test_node_health_service.c:401`.

## Ordered steps (each independently gateable + copy-provable)

1. Anchor write-guard + read-back exclusion (`tip_finalize_stage.c:82-125`) —
   ship FIRST; makes the contiguous-prefix frontier sound. + unit test. No
   live-datadir change.
2. Re-floor the boot clamp onto height(coins_best_block HASH) — the minimal
   live-wedge fix. Copy-proof on the torn datadir copy.
3. Repoint `cec` diagnostic readers + fix the 2 tests (must precede step 4).
4. Delete `cec` writers + Guard A. Pure subtraction.
5. Demote the dead-legacy coins store to recovery-only + add a boot invariant
   assert (`served_tip == tip_finalize contiguous frontier`).

## Migration

**Live node: clean cold-import from the oracle** (`-cold-import=$HOME/.zclassic`,
~60s), owner-gated `make deploy`. The torn datadir is 4-way poisoned; cold-import
produces finalized-frontier == coins == height by construction, zero residual
poison. **Self-heal (steps 1+2) is the COPY regression proof, not the live
remedy.**

## Copy-proof (on a copy, never live)

A. Wedge-clear (torn copy): boot patched binary → log shows clamp to the
   HASH-resolved genuine frontier (not `cec`) → `finalized_total` climbs,
   `reorg_detected` does not → `served_tip == contiguous ok=1 frontier`, no
   `tip_finalize_log` row deleted.
B. SHA3 UTXO-commitment EXACT match vs zclassicd oracle `gettxoutsetinfo`
   (RPC 8232, `~/.zclassic`) at H_node (from the HASH, not `cec`) +
   `getblockhash H_node == node coins_best hash`.
C. Kill-9 mid-climb, restart, re-run A+B; tip must not drop; ADVANCE_DEADLINE ≥300s.

## Deferred (off the critical path, owner-gated)

- The `utxo_apply`/event-log non-atomicity (pre-existing torn window) — this plan
  deliberately does NOT collapse onto it. Future: fold the projection inside the
  `stage_run_once` txn (one DB / ATTACH) so the coin mutation is transactional
  with its cursor.
- Eventual deletion of the dead `process_block` / `coins_view_sqlite` legacy
  connect engine once the stale-anchor heal is re-rooted off it.
