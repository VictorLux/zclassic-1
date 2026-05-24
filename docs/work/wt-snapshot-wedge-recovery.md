# Worker Assignment — Snapshot recovery for a wedged tip (resilience escape hatch)

**Worktree:** any (wt2 / wt3)
**Branch:** PUSH DIRECT TO MAIN
**Moves benchmarks:** #6 kill-9 / wedge recovery (360s → <60s), #5 tip keep-up
**Vision tie-in:** "wedges unreachable by construction" — if the tip is wedged,
the node re-snapshots *past* the bad block instead of giving up.
**Status: IN PROGRESS (wt3)** — PR-0 entry API slice implemented;
runtime local-snapshot manifest builder still pending.

---

## The problem (observed live 2026-05-24, height 3,121,684, ~1,990 behind tip)

Fast-sync (FlyClient + SHA3 UTXO snapshot) today is **cold-start only**. When a
node that already holds the chain gets **wedged at the tip** — a single block
fails to connect — self-heal exhausts its attempts and the node just sits there.
Observed condition state on the wedged node:

- `block_failed_mask_at_tip` — **critical**, 5/5 attempts, all `failed`
- `contradiction_frozen` — **critical**
- `local_import` source: `state=next-child-missing`, `selection_blocker=no_forward_progress`
- `snapshot` source: `state=idle`, `reason="state=idle peer=0 staged=0 offered=0"`

The snapshot source has the machinery to fix this (jump to a fresh verified
height past the wedge) but **nothing triggers it from a near-tip stall** — it
only fires at cold start, and only if a peer is *offering* a snapshot.

## ⚠️ Feasibility checked (2026-05-24) — read before claiming

A read-only audit + spot-check of the snapshot subsystem found the original
plan (a Condition that "triggers a re-sync") is **NOT buildable as-is**. Two
hard facts, both verified at the source:

- **No trigger API.** The only public entry to snapshot sync is
  `snapsync_handle_offer()` (`snapshot_sync_service.h:209`) — it is *peer-offer
  driven*. There is no `initiate` / `request_resync` / `start`. A Condition has
  nothing to call.
- **Apply is replace-all, cold-start-gated.** `snapshot_apply.c:52` does
  `DELETE FROM utxos` then promotes the staged set — it assumes *snapshot = new
  tip*. That replace-all is actually exactly what wedge-recovery wants (discard
  the wedged set, adopt a verified snapshot past the wedge), BUT acceptance is
  gated to a near-empty UTXO set (<100K, `snapshot_offer.c`), so it refuses to
  run over a populated 3.12M-height chain.
- **No runtime local-snapshot builder.** The LDB→snapshot path exists only at
  boot (`-cold-import` / `-snapshot`), not as a runtime-callable function.

So this needs a **foundational PR-0** before the Condition can be written.

## The fix: make snapshot a reachable recovery path

Wire snapshot re-sync as a **self-heal action for a wedged tip**, with two
sources of a snapshot (network OR local), so recovery does not depend on a
zclassic23 peer being connected.

### PR-0 — Runtime re-sync entry point + recovery gate  ← CLAIM FIRST (foundational)

Make snapshot sync reachable without a peer offer, over a populated chain:
- Add a PUBLIC function the recovery Condition can call, e.g.
  `bool snapsync_request_recovery(int32_t target_height, const struct snapshot_offer_params *manifest)`.
  It takes a manifest as a parameter (does NOT invent one) and drives the
  existing IDLE→NEGOTIATING→… lifecycle.
- Add a runtime **local-snapshot manifest builder** that wraps the proven
  boot-time LDB read (`ldb_snapshot.c` / `utxo_snapshot_loader.c`, and the
  immutable-LDB read in `tools/rebuild_recent.c`) into `snapshot_offer_params`
  (height, UTXO root, MMB root, chainwork). This is the peerless source.
- Parameterize the cold-start accept gate so a **recovery** resync is permitted
  over a populated UTXO set (today's <100K gate refuses it). Recovery must be
  explicitly opt-in (a flag/reason), never the default offer path.
- Reuse `snapshot_apply` replace-all unchanged — for recovery, discarding the
  wedged set and adopting the verified snapshot is the intended semantics.

**MUST:** keep FlyClient + SHA3 verification on the recovery path. No trust
shortcut — a recovery snapshot is verified exactly like a cold-start one.

### PR-1 — Wedge-detect Condition that calls PR-0  (gated on PR-0)

Add (or extend) a self-heal Condition `tip_wedged_resnapshot`:
- **Detect:** `block_failed_mask_at_tip` exhausted (attempts == max, last_outcome
  `failed`) **OR** `local_import` reports `next-child-missing` past N retries,
  **AND** we are behind the best header by < a "near-tip" threshold (this is a
  wedge, not a cold start). Reference the live source/condition fields in
  `app/controllers/src/diagnostics_controller.c` dumpers + the condition engine.
- **Act:** request a snapshot re-sync to a verified height **past** the wedged
  block, then promote + resume normal advance. Reuse the existing
  `snapshot_sync_service` lifecycle (IDLE→NEGOTIATING→RECEIVING→VERIFYING→COMPLETE)
  and `snapshot_apply.c` tip activation — do NOT fork a second apply path.
- **MUST:** keep FlyClient + SHA3 verification on the recovery path (no trust
  shortcut — a wedge-recovery snapshot is verified exactly like a cold-start one).

**Owns:** the new Condition + its registration; the call into the PR-0 entry
point. Wire a `zcl_state`/`zcl_conditions` field so the recovery attempt is
observable. (The peerless local source is delivered by PR-0's manifest builder.)

### First-line recovery is cheaper than re-snapshot — respect the ordering

Re-snapshot is the **last-resort** hammer. The cheap first line already exists:
`block_failed_mask_at_tip`'s remedy (`process_block_revalidate`) clears a
`BLOCK_FAILED_VALID` flag once the quorum oracle (peers + zclassicd mirror)
agrees on the block hash. `find_most_work_chain` (`process_block_core.c:364`)
skips any fork with a failed ancestor, so one stale failed-flag wedges the tip;
clearing it is enough. That remedy is failing **only because the quorum can't be
reached** (no zclassic23 peers / mirror disagreement). `tip_wedged_resnapshot`
must fire **after** the quorum-clear path is exhausted, not instead of it.

---

## Acceptance
- [ ] Repro a wedged tip (or simulate the exhausted-`block_failed_mask_at_tip`
      condition), confirm `tip_wedged_resnapshot` fires and the node resumes
      advancing — not a manual restart.
- [ ] Snapshot recovery still runs full FlyClient + SHA3 verification.
- [ ] `./test_parallel --jobs=$(nproc)` PASS; new test in `lib/test/`.
- [ ] `zcl_conditions` shows the recovery attempt + outcome.

## NOT in scope (do not regress)
- Cold-start fast-sync behavior is unchanged.
- No trust shortcut: recovery snapshots are verified like any other.

<!-- Worker: append a Completion section with the "Benchmark moved" line (#6). -->
