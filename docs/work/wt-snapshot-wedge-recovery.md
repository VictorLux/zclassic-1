# Worker Assignment — Snapshot recovery for a wedged tip (resilience escape hatch)

**Worktree:** any (wt2 / wt3)
**Branch:** PUSH DIRECT TO MAIN
**Moves benchmarks:** #6 kill-9 / wedge recovery (360s → <60s), #5 tip keep-up
**Vision tie-in:** "wedges unreachable by construction" — if the tip is wedged,
the node re-snapshots *past* the bad block instead of giving up.

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

## The fix: make snapshot a reachable recovery path

Wire snapshot re-sync as a **self-heal action for a wedged tip**, with two
sources of a snapshot (network OR local), so recovery does not depend on a
zclassic23 peer being connected.

### PR-1 — Wedge-detect Condition that arms snapshot recovery  ← CLAIM FIRST

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

**Owns:** the new Condition + its registration; the trigger call into
`snapshot_sync_service`. Wire a `zcl_state`/`zcl_conditions` field so the
recovery attempt is observable.

### PR-2 — Local snapshot source (no peer required)  (spec; gated on PR-1)

On THIS box the real always-available escape hatch is the local `zclassicd`
LevelDB (`~/.zclassic`), already proven by the `-cold-import` path
([[reference_zclassic23_cold_import_path]]: 145s cold / 33s warm). Today
network snapshot recovery needs a zclassic23 peer *offering* a snapshot; the
node is usually connected only to magicbean (C++) peers that don't serve them.

**Plan:** let `tip_wedged_resnapshot` fall back to a **local** snapshot built
from the legacy LDB (the immutable-data read proven in `tools/rebuild_recent.c`)
when no peer offers one. This makes wedge-recovery work offline / peerless.
Gated on PR-1 + a confirmed wedge reproduction.

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
