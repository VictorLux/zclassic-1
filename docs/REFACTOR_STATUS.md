# Framework Refactor — Status Board

> One-screen summary of where we are. Updated every PR. Single source of
> truth for "what's done, what's next, what's blocked." Read this first
> when you start a session. Full architecture: [`FRAMEWORK.md`](./FRAMEWORK.md).

**Updated:** 2026-05-24 (Wave S SHADOW 100%; Phase 5a-1 crypto registry MERGED; C-2 cutover IN PROGRESS by wt3; 5a-2 READY)

---

## Phases

```
Phase 0  [██████████] 100%   Condition engine + scaffold              ✅ DONE
Phase 1  [██████████] 100%   Adopt unused primitives                  ✅ DONE
Phase 2  [██████████] 100%   Wave S SHADOW complete (S-1..S-9 all shipped)  ✅
  ├ S-5..S-7 [██████████] 100%   body_persist, script_validate, proof_validate ✅
  ├ S-8    [██████████] 100%   utxo_apply shadow (wt3)                ✅ 497220f58
  └ S-9    [██████████] 100%   tip_finalize shadow (wt3)              ✅ 1a65b33c7
Phase 2 CUTOVER [░░░░░░░░░░]   0%   Flip shadow → authoritative      ← C-2 READY (S-9 merged)
  ├ C-2    [░░░░░░░░░░]   0%   header_admit authoritative             ← READY  (standalone spec)
  ├ C-3    [░░░░░░░░░░]   0%   validate_headers authoritative          ← spec'd (queued, post C-2)
  ├ C-5    [░░░░░░░░░░]   0%   body_persist + delete body_fetch (batch spec, post C-3)
  ├ C-6    [░░░░░░░░░░]   0%   script_validate authoritative (batch spec, post C-5)
  ├ C-7    [░░░░░░░░░░]   0%   proof_validate authoritative (batch spec, post C-6)
  ├ C-8    [░░░░░░░░░░]   0%   utxo_apply authoritative (batch spec, post C-7 — gates utxo_recovery dissolve)
  └ C-9    [░░░░░░░░░░]   0%   tip_finalize authoritative (batch spec, post C-8 — gates chain_advance dissolve)
Phase 3  [██████░░░░]  60%   Dissolve mega-modules                    ← partial
  ├ watchdog [██████████] 100%   sync_watchdog_service.c DELETED      ✅ 611631541
  ├ supervisor tree split [██████████] 100%   7 domain supervisors    ✅ dae31dee9
  ├ chain_advance  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ legacy_mirror  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ chain_restore  [░░░░░░░░░░] independent — plan ready, awaiting per-PR assignment
  ├ header_probe   [░░░░░░░░░░] independent — plan ready, awaiting per-PR assignment
  └ utxo_recovery  [░░░░░░░░░░] gated on C-8 cutover (dissolve plan ready)
Phase 4  [░░░░░░░░░░]   0%   Storage unification — plan: docs/architecture/phase4-storage-unification.md
  ├ 4a     [░░░░░░░░░░]   0%   event_log primitive  ← READY (primitive sits idle, no callers wired)
  ├ 4b     [░░░░░░░░░░]   0%   utxo_projection — first event-log consumer (queued, post-4a)
  └ 4c     [░░░░░░░░░░]   0%   block_index_projection — kills LevelDB (queued, post-4a)
Phase 5  [██░░░░░░░░]  14%   Crypto agility + reproducible builds — plan: docs/architecture/phase5-crypto-agility-and-releases.md
  ├ 5a-1   [██████████] 100%   Crypto registry skeleton  ✅ c4bebe0a2 (SHA256/BLAKE2b/ECDSA/Groth16 wrappers, no call sites rewired)
  ├ 5a-2   [░░░░░░░░░░]   0%   First call site rewire: Equihash PoW  ← READY (5a-1 merged)
  └ 5b-1   [░░░░░░░░░░]   0%   flake.nix reproducible build skeleton  ← READY (independent of everything; needs Nix installed locally)
Phase 6  [░░░░░░░░░░]   0%   Determinism + simulator
Phase 7  [░░░░░░░░░░]   0%   Frontier (io_uring, hot reload)

All 5 mega-module dissolve plans drafted: docs/dissolve/
(sync_watchdog, chain_advance_coordinator, legacy_mirror_sync,
chain_restore, header_probe, utxo_recovery)
```

---

## Conformance metrics (updated each PR)

The five user-facing numbers (cold-start, warm-start, MTBF, RSS,
kill-9 recovery) are spec'd in [`USER_BENCHMARKS.md`](./USER_BENCHMARKS.md)
along with QoL numbers and the **operator paging rate target: 0/month**
clause. The table below is the dashboard.

| Metric | Today | Target | Delta |
|---|---|---|---|
| Files conforming to shape | scaffold | 342 / 342 | scaffold lint not yet run |
| `.c` files in `app/` > 800 LOC | 13 | 0 | dissolve in Phases 2-3 |
| Mega-modules remaining | 6 | 0 | see roster below |
| Lint gates active | 20 (1 FAIL'd in P1) | 21 | +1 by Phase 3 (gate #20→FAIL) |
| Raw clock/RNG callers | **0** | 0 | ✅ Phase 1c (was 443) |
| Mailbox prod callers | 1 | many | ✅ Phase 1a (header_admit), more in Phase 3 |
| Conditions registered | 3 | ~15 | Phase 2+ adds ~6 from sync_watchdog dissolution |
| MTBF (live node) | 5.5 d | 30 d | Phase 0 + Phase 2 |
| RSS steady-state | 2.2 GB | 1 GB | Phase 3 |
| Cold-start | 145 s | 60 s | Phase 2 |
| Warm-start | 33 s | 10 s | Phase 2 |
| Kill-9 recovery | 60-360 s | 60 s | Phase 2 |
| Operator pages | n/a | 0/month | Phase 0+ (condition engine) |

---

## Mega-module roster (Phase 2-3 deletion targets)

| File | LOC | Dissolves into | Phase |
|---|---|---|---|
| `chain_advance_coordinator.c` | 1,715 | `services/sync/source_scorer.c` + `jobs/tip_finalize.c` + 1 condition | 2 (S-9) |
| `chain_restore_service.c` | 1,673 | `jobs/reorg_*.c` + `services/chain/restore_planner.c` | 3 |
| `sync_watchdog_service.c` | DELETED | replaced by 8 supervised conditions | 3 (PR-3) |
| `legacy_mirror_sync_service.c` | 1,410 | `services/sync/legacy_bridge.c` + `jobs/legacy_poll.c` + 1 condition | 2 (S-12) |
| `header_probe_service.c` | 1,264 | `services/network/header_probe.c` (smaller) + mailbox use | 3 |
| `utxo_recovery_service.c` | 1,241 | `conditions/utxo_drift.c` + `jobs/utxo_repair.c` | 3 |
| `chain_evidence_controller.c` | 1,083 | `services/chain/evidence.c` + 1 condition | 2 (S-9) |

---

## In flight (worktrees)

| Worktree | Branch | Assignment | Status | Last update |
|---|---|---|---|---|
| `~/github/zclassic23` (main) | `main` | Orchestrator: queue + plan + merge | ✅ 5a-1 merged via sub-agent; 3 more queue items drafted (C-3, 4c, 5a-2) | 2026-05-24 |
| `~/github/zclassic23-2` (wt2) | `main` (direct push) | (idle — pick next from READY queue below) | 🕐 READY for next task | 2026-05-24 |
| `~/github/zclassic23-3` (wt3) | `main` (direct push) | [`docs/work/wt-phase2-cutover-c2-header-admit.md`](./work/wt-phase2-cutover-c2-header-admit.md) | 🚧 IN PROGRESS (started 97fc3b02b) | 2026-05-24 |

**READY queue** (any worker can pick on restart; first to mark IN PROGRESS wins):
- [`docs/work/wt-phase4a-event-log-primitive.md`](./work/wt-phase4a-event-log-primitive.md) — append-only event log with kill-9 fuzz harness; primitive sits idle, no callers wired.
- [`docs/work/wt-phase5a2-first-call-site-rewire.md`](./work/wt-phase5a2-first-call-site-rewire.md) — route Equihash PoW through the registry; 5-task PR with indirection-cost benchmark gate.
- [`docs/work/wt-phase5b1-flake-nix-skeleton.md`](./work/wt-phase5b1-flake-nix-skeleton.md) — `flake.nix` reproducible build skeleton; **needs Nix installed locally** (`sh <(curl -L https://nixos.org/nix/install) --daemon`).

**QUEUED (gated on a specific predecessor)** — workers can read the spec but should not start until the gate clears:
- C-3 validate_headers cutover (after C-2 + soak)
- Phase 4b utxo_projection (after 4a)
- Phase 4c block_index_projection (after 4a) — kills LevelDB
- Phase 2 cutovers C-5..C-9 (each gated on its predecessor + soak)
- All Phase 3 dissolves besides watchdog (gated on cutover progress per their respective `docs/dissolve/` plans).

---

## Recently completed

| Date | What | Worktree | Commit |
|---|---|---|---|
| 2026-05-24 | **Phase 5a-1 MERGED — crypto registry skeleton.** SHA256/BLAKE2b/ECDSA/Groth16 wrappers + dispatch table. No consensus call sites rewired yet (5a-2 does the first rewire) | main | c4bebe0a2 |
| 2026-05-24 | **Plans:** standalone cutover C-3 spec; Phase 4c block_index_projection (kills LevelDB); Phase 5a-2 first call-site rewire (Equihash PoW); orchestrator launched sub-agent for Phase 5a-1 implementation | main | e41fb92ba |
| 2026-05-24 | **Phase 3 supervisor tree split MERGED** — flat supervisor → 7 domain supervisors (chain, net, mempool, wallet, feature, onion, op) + self_heal | wt3 → main | dae31dee9 |
| 2026-05-24 | **Phase 3 watchdog dissolve COMPLETE** — PR-2 (4 kick conditions) + PR-3 (DELETED `sync_watchdog_service.c` — 1,448 LOC gone) | wt2 → main | 611631541 |
| 2026-05-24 | **Phase 2 S-9 MERGED — Wave S SHADOW COMPLETE** — tip_finalize shadow stage; all 9 stages ship; cutover C-2 unblocked | wt3 → main | 1a65b33c7 |
| 2026-05-24 | **Plans:** Phase 4b utxo_projection assignment (10 tasks, first event-log consumer) + Phase 5a-1 crypto registry skeleton assignment (parallel-safe, additive indirection) | main | 7b3832c62 |
| 2026-05-24 | **Workflow:** direct-push-to-main; agent-protocol.md rewritten; 22 stale remote branches deleted; only `origin/main` remains | main | bb5bcc7f1 |
| 2026-05-24 | **Phase 2 S-8 MERGED** — utxo_apply shadow stage: per-block UTXO delta computation (added/spent), shadow-vs-live diff (`g_delta_diverged_total` gate for C-8) | main | 497220f58 |
| 2026-05-24 | Cherry-picked wt2 mailbox drain hardening (bounded drains prevent reentrant-publish starvation) + wallet view projection move (lint gate #20 debt) | main | 12d9c8e73 |
| 2026-05-23 | **Phase 2 S-7 MERGED** — zero-knowledge proof verification shadow stage: Sapling spends + outputs (Groth16), Sprout JoinSplits (Groth16/PHGR13), binding sigs; per-proof-type counters | main | b6138327f |
| 2026-05-23 | Plans: Wave S cutover playbook (9 PRs, 4-commit structure) + Phase 5 crypto agility + 3 mega-module dissolve plans | main | 0a6fd9108 |
| 2026-05-23 | **Phase 2 S-6 MERGED** — script_validate shadow stage: per-input script_verify across every tx; per-height log of (verified \| script_invalid \| internal_error \| upstream_failed) | main | (S-6 merge) |
| 2026-05-23 | **Phase 3 PR-1 MERGED** — first 2 watchdog conditions extracted: `block_failed_mask_at_tip` predicate extended; NEW `utxo_activation_paused` | main | 19ae6d8b1 |
| 2026-05-23 | Plans: 2 more dissolve plans drafted (`chain_advance_coordinator.md`, `legacy_mirror_sync_service.md`) + Phase 4 storage unification architecture | main | 123c00b13 |
| 2026-05-23 | **Phase 1b MERGED** — projection adoption: `zcl_getblockcount` reads MVCC snapshot; new `chain_projection_*`, framework projection wrapper, MVCC-under-load stress test (4 readers + 1 writer × 1000) | main | a96856925 |
| 2026-05-23 | **Phase 2 S-5 MERGED** — body_persist shadow stage: per-height read + header + Merkle verification, no consensus mutation | main | 218b79bb4 |
| 2026-05-23 | Plan: dissolve `sync_watchdog_service.c` (1,448 LOC) → 8 Conditions over 3 PRs (~850 LOC net deletion) | main | b7257f225 |
| 2026-05-23 | **Phase 1c MERGED** — platform.clock + platform.rng rewired in 167 files; gate #19 ratcheted WARN→FAIL with 0 violations | main | be9e05022 |
| 2026-05-23 | **Phase 1a MERGED** — first production mailbox adopter: `header_probe_service` → `header_admit_inbox` → `header_admit_stage` drain | main | (prior merge before be9e05022) |
| 2026-05-23 | **Phase 0 MERGED** — condition engine + 3 conditions (wt2) + lint gates #18-#20 WARN (wt3) + test fixture fixes | main | 4e0ea3382 |
| 2026-05-23 | wt3 Phase 0b — `lib/framework/condition.h` stub, `tools/lint/framework_shape_check.sh` + 2 prep gates, `DEFENSIVE_CODING.md` updates | wt3/phase0-framework-shape-lint | 32b17449c |
| 2026-05-23 | wt2 Phase 0a — `lib/framework/condition.{c,h}`, `self_heal` supervisor, 3 conditions (block_failed_mask_at_tip, contradiction_frozen, chain_stalled_with_data), `zcl_conditions` MCP tool, 6 unit tests passing | wt2/phase0-condition-engine | 94e5fa31f |
| 2026-05-23 | Scaffold: FRAMEWORK.md + REFACTOR_STATUS.md + folder scaffold + work/ docs | main | 786ec92cc |
| 2026-05-23 | `chain_tip_watchdog` — single-purpose tip-stuck overlord (PRE-framework, will dissolve into a Condition in a future phase) | main | fb32df981 |
| 2026-05-22 | Wave S S-4b — legacy-attach one-shot import | main | (multiple) |
| 2026-05-22 | Wave S S-4 — body_fetch shadow stage | main | 95abed36d |

## Phase 0 acceptance evidence

- ✅ condition engine primitive shipped: `lib/framework/condition.{c,h}` (284 LOC impl, 73 LOC header)
- ✅ 3 conditions registered: `block_failed_mask_at_tip` (CRITICAL, wires Wave M's process_block_revalidate), `contradiction_frozen` (CRITICAL, detect-only Phase 0), `chain_stalled_with_data` (WARN, force_mirror remedy)
- ✅ `self_heal.engine` supervisor child registered (5s period)
- ✅ MCP tool `zcl_conditions` + `zcl_state subsystem=condition_engine`
- ✅ 6 unit tests passing
- ✅ Lint gates #18 (framework_shape, WARN, baseline=0), #19 (raw_clock, WARN, baseline=443), #20 (raw_sqlite_in_controllers, WARN, baseline=121)
- ✅ `make lint` PASS
- ✅ `make` builds clean
- ✅ Live unwedge verification deferred (current live node is past the wedge); condition lifecycle verified via unit tests instead

---

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

---

## Phase 1 acceptance evidence (1a + 1c)

- ✅ Mailbox primitive has its first production caller: `header_probe_service`
  publishes accepted headers into `header_admit_inbox`, drained by
  `header_admit_stage` before its existing cursor scan.
- ✅ New `lib/framework/include/framework/mailbox.h` (typed inbox macros over
  `util/mailbox.h`) — pattern other actors can copy in Phase 3.
- ✅ `platform.clock` adopted everywhere: 443 → 0 raw clock callers outside
  `lib/platform/`. Deterministic-simulator prerequisite met.
- ✅ `platform.rng` adopted everywhere: zero `getrandom(` calls outside
  `lib/platform/` and tagged exceptions.
- ✅ New sugar header `platform/time_compat.h` makes incremental rewires
  mechanical (`platform_time_wall_unix()`, `platform_time_wall_time_t()`, etc).
- ✅ Lint gate #19 ratcheted WARN → FAIL — new raw clock calls now break CI.
- ✅ `make lint` PASS; `make -j$(nproc)` PASS.
- ✅ `make test_parallel` 177/179 groups PASS (2 pre-existing failures unrelated
  to Phase 1: `test_mcp_e2e` count_substring off-by-one — a tool description
  contains a `"name":"zcl_..."` substring — and `file_controller consensus
  snapshot export`. Both pre-date Phase 1 and are tracked separately.)
- ⏳ Phase 1b (projection adoption) deferred to next sub-wave.

---

## Next decision points (orchestrator)

1. When wt2 + wt3 both report DONE on their assignments → orchestrator merges, runs full test suite, updates Phase 0 to 100%, opens Phase 1 assignments.
2. Phase 0 acceptance: live node unwedges, condition engine reports condition #1 active+remedy+witness flow, lint gate counts violations without failing build.
3. Phase 1 trigger: Phase 0 has been live for ≥ 3 days with no operator pages.
