# Framework Refactor — Status Board

> One-screen summary of where we are. Updated every PR. Single source of
> truth for "what's done, what's next, what's blocked." Read this first
> when you start a session. Full architecture: [`FRAMEWORK.md`](./FRAMEWORK.md).

**Updated:** 2026-05-25 (**goals = 4 promises × 10 numbers** — agents report work as "moved number N", not phase codes. Phase detail below is the execution map.)

---

## OUR GOALS — one static C23 binary, four promises

Honest scoreboard. **MEASURED** = a real number from this box (date + how, in
[`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md)). **TARGET** = where we're going.
**not measured** = no harness run yet — don't quote a number.

> Read live from the SERVICE (canonical C, not a shell snapshot): `zcl_status`
> (tip, sync_state, `tip_advance_age_seconds`) + `zcl_state subsystem=blocker`
> (the typed blocker registry = "what's blocking"). No-MCP fallback: `tools/zcl-rpc`.
> **Halt cured 2026-05-25** — the live node was rebuilt from the local zclassicd
> and is HEALTHY (advancing, 0 restarts). The scoreboard numbers below are a
> 2026-05-25 07:08Z snapshot from BEFORE the cure and the cutover; re-measure
> after the cutover lands rather than quoting them.

```
  ⚡ FAST                       live now           target        state
     Cold sync to tip          180s  (05-24)       30s           ▸ PR-3 building
     Warm restart              37.7s (05-24)       10s           ▸ real restart→tip
     Validation speed          108 blk/s (05-24)   fast          ◷ measured; tip frozen now
     Stay at tip               advancing (05-25)   keep <1 blk    ◑ live healthy (connect_block fix + rebuild); structural fix = cutover

  🪶 LEAN
     Memory (RSS)              1.97 GB (05-25)*    1.0 GB        ▲ climbs w/ bg-verify
     Binary size               15.4 MB (05-24)     stay slim     ✓ met (docs' 26MB stale)

  💪 UNBREAKABLE  ← resilience is a PROMISE, measured by truth not by "result=ok"
     Tip advancing             YES (05-25, 0 restart) always      ◑ live healthy; can't-halt-by-construction still needs the cutover
     Self-heal tells the truth gated + alert loop    0 false-ok    ✓ witness gating (47bdbc211) + EV_OPERATOR_NEEDED routed
     Halt/crash recovery       rebuild ~secs (05-25)  <60s, auto   ◑ connect_block fix shipped + rebuild-from-zclassicd proven; auto-heal pending
     Uptime before failure     rebuilt, advancing     30 days      ◷ re-measuring on the healthy node (was halted ~5h pre-fix)
     Alerts to a human         loop CLOSED (05-25)    0 / month    ✓ EV_OPERATOR_NEEDED → sinks + zcl_status DEGRADED + sd_notify

  🔬 HONEST
     Live truth from service   zcl_status (C)      always true    ◑ build-commit + dominant-blocker → surface in zcl_status
     Bug → reproducible fix    built (postmortem+chaos) 1 tape    ✓ Phase 6 done (6fb76f2b0)
```
`*` RSS soak (05-24): fresh boot 1.53 GB → **stair-steps up with bg-validation
depth** → ~2.4 GB by 17min (only 6.6% validated), still creeping. (Node rebuilt
2026-05-25 — RSS to be re-measured on the healthy node.) NOT bounded at a low plateau — tracks how much chain
bg-verify has buffered. >2× the 1 GB target; real ceiling needs the full ~8h
run. `✓` met · `▸` in flight · `◷` measuring · `◑` fixed-in-code-not-deployed ·
`▲` above target · `✗` BROKEN/regressed.

### Halt cured 2026-05-25 — focus is the one-path cutover + DRY

Live node healthy and advancing (0 restarts). Root cause was the `connect_block`
BIP30 self-write + write-ordering hazard, now fixed (see git history);
silent-halt escalation is closed (`EV_OPERATOR_NEEDED` → alert sinks + `zcl_status`
DEGRADED + sd_notify). The **structural** cure is the cutover below — collapse to
ONE path, delete the legacy (3 modules = 4,407 LOC + comparison scaffolding). Full root-cause + resilience doctrine
live in git history + memory ([[project_silent_halt_architecture_diagnosis_2026-05-25]],
[[feedback_resilience_first_class_live_truth]]).

**Working rule:** when you finish a task, name the goal you moved and the measured
delta (e.g. "warm restart 33s→29s") and add a row to BENCHMARKS_LOG.md.

### Owner mandate (2026-05-25)

**NO whack-a-mole. Collapse to ONE path, DELETE the legacy.** The node stays
robust by *finishing* the refactor, not by adding conditions — default to
SUBTRACTION. While two chain paths coexist a silent halt remains *possible*; the
cutover ([`work/cutover.md`](./work/cutover.md)) is the structural cure. Current
work is in NEXT below.

---

## Phases

```
Phase 0  [██████████] 100%   Condition engine + scaffold              ✅ DONE
Phase 1  [██████████] 100%   Adopt unused primitives                  ✅ DONE
Phase 2  [██████████] 100%   Wave S SHADOW complete (S-1..S-9 all shipped)  ✅
  ├ S-5..S-7 [██████████] 100%   body_persist, script_validate, proof_validate ✅
  ├ S-8    [██████████] 100%   utxo_apply shadow (wt3)                ✅ 497220f58
  └ S-9    [██████████] 100%   tip_finalize shadow (wt3)              ✅ 1a65b33c7
Phase 2 CUTOVER [██░░░░░░░░] ~15%   Flip shadow → authoritative — plan: work/cutover.md
  Shadow stages all run + match. The cutover = trusting them, then deleting legacy.
  Simplified to: prove offline (incl reorg) → flip once → delete. NO per-stage soak
  ceremony (single personal node). Prereqs done: halt cured, guard + alert loop,
  header validation decoupled from bodies, HAVE_DATA read-verified.
  Next real work: reorg-capability in tip_finalize + the offline replay-proof.
Phase 3  [███████░░░]  70%   Dissolve mega-modules                    ← partial
  ├ watchdog [██████████] 100%   sync_watchdog_service.c DELETED      ✅ 611631541
  ├ supervisor tree split [██████████] 100%   7 domain supervisors    ✅ dae31dee9
  ├ chain_advance  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ legacy_mirror  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ chain_restore  [██████████] 100%   service/header deleted; focused modules own restore ✅
  ├ header_probe   [██████████] 100%   core renamed; old service file deleted ✅ d17eb5ca0
  └ utxo_recovery  [░░░░░░░░░░] gated on C-8 cutover (dissolve plan ready)
Phase 4  [█████████░]  95%   Storage unification (event log + projections)
  ├ 4a     [██████████] 100%   event_log primitive  ✅ 76b3a10b4
  ├ 4b     [██████████] 100%   utxo_projection — Tasks 1-10 SHIPPED  ✅ (39b1e8efa..ee1c5c7b1, 7 commits)
  ├ 4c     [██████████] 100%   block_index_projection + finalize (diff tool + 9 tests)  ✅ (…ed34743ba, 066462576, 91b4ee734, 2f23d6a44, 2e289e41b)
  ├ 4d-1   [██████████] 100%   mempool projection + shadow replay  ✅ da005eb31, cc84e9419
  ├ 4d-2   [██████████] 100%   peers_projection  ✅ 91aa65c1c + 5dc442a81 + 48e78d801 + f925fb6f3 (wt2)
  ├ 4d-3   [██████████] 100%   wallet view projection + diff + final verification  ✅ 12284eb3e, 5626552cb
  ├ 4d-4   [██████████] 100%   znam projection — Tasks 1-5b SHIPPED  ✅ (f52313f02..eb53d9d52, 7 commits, 30 test cases pass)
  ├ 4d-5   [██████████] 100%   small projections: contacts/onion/hodl ✅ 2f23d8352
  └ 4e     [░░░░░░░░░░]   0%   block-body migration (spec'd, gated on 4c cutover)
Phase 5  [██████████] 100%   Crypto agility (registry indirection)    ✅ DONE
  ├ 5a-1   [██████████] 100%   Crypto registry skeleton  ✅ c4bebe0a2 + polish dde0183c7
  ├ 5a-2   [██████████] 100%   First call site rewire: Equihash PoW   ✅ f00be351f (wt2)
  ├ 5a-3   [██████████] 100%   script_validate ECDSA rewire (HOT PATH)  ✅ 7c2c067a0 + cde601acf + e8b926610 (wt3)
  └ Nix reproducible builds (5b/5c) DROPPED 2026-05-24 — out of scope. Cosign signing (5d) parked pending decision.
Phase 6  [██████████] 100%   Determinism + simulator                 ✅ DONE
  ├ 6a     [██████████] 100%   seed_tape primitive  ✅ c2ed3145d + cb03fe595 + c62161c2a + b53f251b7 (sub-agent)
  ├ 6b     [██████████] 100%   postmortem capsule (crash → seed.cap.gz) ✅ 89fabc360
  └ 6c     [██████████] 100%   simulator harness (`make chaos`) ✅ 6fb76f2b0
Phase 7  [░░░░░░░░░░]   0%   Frontier (io_uring, hot reload)
Phase 8  [░░░░░░░░░░]   0%   Event-log compaction & retention (future)
  └ (draft)  gated on 4e — checkpoint event + segmentation + prune policy; pairs with SHA3 snapshot/FlyClient cold-sync

Mega-modules still to dissolve (chain_advance_coordinator, legacy_mirror_sync,
utxo_recovery) are cutover-gated; extract-then-delete plan in work/cutover.md.
```

---

## Conformance metrics (updated each PR)

This table tracks **architecture conformance** (is the code the new shape?).
The **user-facing numbers** (cold/warm/MTBF/RSS/kill-9/pages) live in ONE
place — [`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md), the append-only measured
ledger — and are surfaced on the scoreboard at the top of this file. Don't
re-quote them here; they rot. Add a row to the ledger instead.

| Conformance metric | Today | Target | Delta |
|---|---|---|---|
| Files conforming to shape | scaffold | 342 / 342 | scaffold lint not yet run |
| `.c` files in `app/` > 800 LOC | **31** | 0 | mostly controllers — large DRY/split + dead-code surface (cruft hunt 05-25) |
| Mega-modules remaining | **3** | 0 | chain_advance_coordinator · legacy_mirror_sync · utxo_recovery (all cutover-gated); chain_evidence/restore/watchdog/header_probe DISSOLVED |
| Lint gates active | 20 (1 FAIL'd in P1) | 21 | +1 by Phase 3 (gate #20→FAIL) |
| Raw clock/RNG callers | **0** | 0 | ✅ Phase 1c (was 443) |
| Mailbox prod callers | 1 | many | ✅ Phase 1a (header_admit), more in Phase 3 |
| Conditions registered | 19 | ~15 | `chain_stalled_with_data` retired; activation-no-progress routes through `legacy_mirror_stuck` |

> User-facing numbers (MTBF/RSS/cold/warm/kill-9/pages) intentionally NOT
> tabled here — they rot. Live values: scoreboard at top + `BENCHMARKS_LOG.md`.

---

## Mega-module roster (Phase 2-3 deletion targets)

| File | LOC | Dissolves into | Phase |
|---|---|---|---|
| `chain_advance_coordinator.c` | 1,716 | `services/sync/source_scorer.c` + `jobs/tip_finalize.c` + 1 condition | 2 (S-9) |
| `chain_restore_service.{c,h}` | DELETED | `chain_restore_{planner,executor,repair,integrity,boot_activation,boot_snapshot}.{c,h}` | 3 |
| `sync_watchdog_service.c` | DELETED | replaced by 8 supervised conditions | 3 (PR-3) |
| `legacy_mirror_sync_service.c` | 1,487 | `services/sync/legacy_bridge.c` + `jobs/legacy_poll.c` + 1 condition | 2 (S-12) |
| `header_probe_service.c` | DELETED | `header_probe.c` + `legacy_header_client.c` + mailbox use | 3 (PR-3) |
| `utxo_recovery_service.c` | 1,204 | `conditions/utxo_drift.c` + `jobs/utxo_repair.c` | 3 |
| `chain_evidence_controller.c` | DELETED | split into chain-evidence storage helpers ✅ | 3 |

---

## In flight

**Workers stopped 2026-05-25.** wt2/wt3 worktrees reset to origin/main; work
continues on `main`. Everything below the cutover is either done or cutover-gated.

## NEXT

1. **The cutover — collapse to ONE path.** Full plan:
   [`work/cutover.md`](./work/cutover.md). Prereqs done (halt cured, guard +
   alert loop, header validation decoupled from bodies, HAVE_DATA read-verified).
   Real next work: reorg-capability in `tip_finalize` + the offline replay-proof,
   then a single guarded flip, then delete the legacy path + the shadow-vs-legacy
   comparison apparatus (4,407 LOC across the 3 modules + the diff/shadow scaffolding).
2. **DRY / cruft** —
   [`work/wt-consolidate-import-paths.md`](./work/wt-consolidate-import-paths.md)
   (3 importers → 1). Independent of the cutover.

Recent history lives in `git log` — the per-PR refactor changelog used to sit
here and was removed as journey archaeology.

## Decision log (binding)

- **2026-05-23** Framework adopted: Rails-style MVC + Phoenix-style supervisors + hexagonal cut + Conditions. Replaces "constitution / invariants" framing of the 50-year-architecture plan.
- **2026-05-23** Execution model: strangler (per-mega-module PRs), not big-bang single-commit. Commitment is total — no parallel architecture plans.
- **2026-05-23** Lint gates: ratcheting — start as WARN in Phase 0, tighten to FAIL as violations are fixed.
- **2026-05-23** Worktree workflow: separate clones at `~/github/zclassic23-{2,3}`, identified by pwd suffix, each on its own branch, pushed to origin for orchestrator merge.

---

## How this file gets updated

- **Orchestrator** (main worktree) updates Phases, Conformance, In flight, Recently completed, Decision log.
- **Worker agents** (wt2, wt3, ...) DO NOT edit this file directly — they append a Completion section to their own assignment doc under `docs/work/wtN-*.md`. Orchestrator aggregates.
- **Frequency:** every PR merge updates Recently completed; every conformance number recomputed by `tools/lint/framework_shape_check.sh`.

> Phase 0–1 acceptance evidence lived here; removed 2026-05-25 as done-phase
> archaeology (git history + the Phases section retain it). This board stays a
> one-screen current-state view.
