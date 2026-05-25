# Cutover Safety Protocol — never silently wedge the chain again

**Why this exists:** the C-3 validate_headers cutover (`ad34efb65`, 2026-05-24)
flipped a stage to authoritative, the live chain froze, and **nothing noticed for
the entire session** — the tip never advanced one block, the self-heal lied 46×,
and the only "response" was a kill-9 restart loop that made it worse (it left the
torn coins state now tracked in `wt-bip30-stale-coins-unwedge.md`). A green test
suite said everything was fine.

Every remaining cutover (C-3 re-flip, C-5, C-6, C-7, C-8, C-9 — see
`wt-phase2-cutover-c3-through-c9.md`) MUST follow this protocol. It operationalizes
RESILIENCE DOCTRINE #1–#3 (`docs/REFACTOR_STATUS.md`) for the flip path.

## The protocol (per flip)

1. **Shadow must be 100% matching first.** Do not flip a stage authoritative
   while its shadow run logs ANY divergence at the tip. `SHADOW` mode exists to
   prove parity; a single unexplained mismatch is a STOP. (C-3 was flipped while
   parity at the tip was unproven on the live node — that's the original sin.)

2. **Pre-flip gate — live node healthy.** `tools/scoreboard.sh` must exit 0
   (tip advancing, gap ≤ 2) immediately before the flip. Never flip a node that
   is behind or unhealthy.

3. **Flip is a runtime toggle, reverted in one step.** The mode is already an
   atomic (`HEADER_ADMIT_MODE_*`, `VALIDATE_HEADERS_MODE_*`). Keep the
   compile-time default `SHADOW`; flip via runtime control, so revert is one
   atomic store, not a rebuild+redeploy.

4. **One-block canary.** After the flip, require the tip to connect **exactly one
   block authoritatively past the cutover height**, then compare that block's
   result to legacy/shadow. Only then continue.

5. **Auto-revert on no-forward-progress (THE missing guard).** A supervised
   Condition watches `sync_monitor_tip_advance_age()` after any flip. If the tip
   does not advance past the cutover height within `CUTOVER_WATCH_SECS`
   (start 180 s), it **reverts the stage to SHADOW automatically and pages**.
   This is doctrine #1 made mechanical: a flip that doesn't produce forward
   progress un-flips itself in minutes, not a silent multi-hour freeze.

6. **Soak AFTER the canary, not instead of it.** The 24 h zero-divergence soak
   gates the *next* flip and the legacy-path *deletion* — it is not a substitute
   for steps 4–5. A soak timer never moved a frozen tip.

## Claimable work — build the enforcement (the auto-revert Condition)

**Status: READY.** Independent. Moves UNBREAKABLE (Tip advancing, Alerts).

**Scope:** `app/conditions/src/cutover_no_forward_progress.c` (NEW), registered
under the chain supervisor; reuse existing infra:
- `sync_monitor_tip_advance_age()` (`lib/.../sync_monitor.c:98`) — detect.
- the stage mode setters (`header_admit_set_mode` / `validate_headers_set_mode`)
  — remedy = store `SHADOW`.
- `node_health_service` already pages on `tip_advance_age > 600`
  (`node_health_service.c:431`) — wire the cutover page there or via event.

**Condition shape (`lib/framework/condition.h`):**
- `detect`: any stage in AUTHORITATIVE mode AND `tip_advance_age >
  CUTOVER_WATCH_SECS` AND chain is behind a known peer.
- `remedy`: set that stage back to `SHADOW`; record which stage + height.
- `witness(target_at_detect)`: after revert, the tip advances past the height it
  was stuck at within `witness_window_secs`. Returns FAILED if still frozen —
  do NOT report `ok` while frozen (doctrine #2; this is the exact lie
  `47bdbc211` fixed for `peer_floor`).

**Acceptance:**
- Unit test: a stage stuck in AUTHORITATIVE with a frozen tip → Condition fires,
  reverts to SHADOW, witness confirms tip moved. RED before, GREEN after.
- `make test_parallel` clean; `make lint`.
- Live (gated on deploy): with this Condition live, a deliberately-bad flip
  reverts within `CUTOVER_WATCH_SECS` and `scoreboard.sh` returns to 0.

## Non-goals
- The at-tip kill-9 *ordering* hazard (coins.db commits before block_index fsync)
  — that's the prevention side; recovery is `wt-bip30-stale-coins-unwedge.md`.
  This protocol covers the *flip*, not the crash.
- Changing consensus. Reverting to SHADOW just stops trusting the new path; the
  legacy path (still present until the C-*del deletes) keeps the chain correct.

## References
- `docs/REFACTOR_STATUS.md` — RESILIENCE DOCTRINE, P0, cutover block.
- `wt-phase2-cutover-c3-through-c9.md` — the flip sequence this gates.
- `wt-bip30-stale-coins-unwedge.md` — the wedge the unguarded C-3 flip caused.
- Memory: `feedback_resilience_first_class_live_truth`.
