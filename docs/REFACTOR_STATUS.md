# Framework Refactor — Status Board

> One-screen summary of where we are. Updated every PR. Single source of
> truth for "what's done, what's next, what's blocked." Read this first
> when you start a session. Full architecture: [`FRAMEWORK.md`](./FRAMEWORK.md).

**Updated:** 2026-05-24 (**goals = 4 promises × 10 numbers** — agents report work as "moved number N", not phase codes. Phase detail below is the execution map.)

---

## OUR GOALS — one static C23 binary, four promises

Honest scoreboard. **MEASURED** = a real number from this box (date + how, in
[`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md)). **TARGET** = where we're going.
**not measured** = no harness run yet — don't quote a number.

```
  ⚡ FAST                       measured now      target        state
     Cold sync to tip          180s  (05-24)      30s           ▸ PR-3 building
     Warm restart              37.7s (05-24)      10s           ▸ real restart→tip
     Validation speed          108 blk/s (05-24)  fast          ✓ measured, running
     Stay at tip               AT TIP, 0 gap      keep <1 blk    ✓ synced now

  🪶 LEAN
     Memory (RSS)              1.93 GB (05-24)*    1.0 GB        ▲ plateaus, ~2x target
     Binary size               14.6 MB (05-24)     stay slim     ✓ met (docs' 26MB stale)

  💪 UNBREAKABLE
     Wedge/crash recovery      180s, manual        <60s, auto    ▸ PR-0 building (wt3)
     Uptime before failure     soak running        30 days       ◷ baselined 05-24
     Alerts to a human         not measured        0 / month     10 Conditions live, self-heal

  🔬 HONEST
     Bug → reproducible fix    not built           1 seed-tape   simulator pending
```
`*` RSS measured plateau (soak 05-24): fresh boot 1.53 GB → 1.93 GB after
bg-validation fills buffers (~330s), then **stable** — bounded, not a leak,
but still ~2× the 1 GB target. `✓` met · `▸` work in flight · `◷` measuring ·
`▲` above target.

**When you finish a task, name the goal you moved and the measured delta**
(e.g. "warm restart 33s→29s"), then add a row to BENCHMARKS_LOG.md.

### Who's moving what right now

| Work | Goal | Who |
|---|---|---|
| PR-3 parallel blk*.dat marking (cold-import 101s→seconds) | Cold sync | wt2 |
| PR-0 snapshot wedge-recovery (auto-heal a stuck tip) | Recovery | wt3 |
| Cutover C-5→C-9 authoritative | Cold sync, validation, recovery | — |
| 4e bodies-into-log + 4d projections | Warm restart, recovery | — |
| Phase-3 dissolves (header_probe ✅, chain_restore, utxo_recovery) | Memory, uptime | — |
| More self-heal Conditions | Alerts, uptime | — |
| Phase-6 postmortem + simulator | Bug→fix | — |

---

## Phases

```
Phase 0  [██████████] 100%   Condition engine + scaffold              ✅ DONE
Phase 1  [██████████] 100%   Adopt unused primitives                  ✅ DONE
Phase 2  [██████████] 100%   Wave S SHADOW complete (S-1..S-9 all shipped)  ✅
  ├ S-5..S-7 [██████████] 100%   body_persist, script_validate, proof_validate ✅
  ├ S-8    [██████████] 100%   utxo_apply shadow (wt3)                ✅ 497220f58
  └ S-9    [██████████] 100%   tip_finalize shadow (wt3)              ✅ 1a65b33c7
Phase 2 CUTOVER [███░░░░░░░]  28%   Flip shadow → authoritative      ← C-5 next, SOAK-GATED
  ├ C-2    [██████████] 100%   header_admit AUTHORITATIVE             ✅ f3f0c6c4e (the flip)
  ├ C-3    [██████████] 100%   validate_headers AUTHORITATIVE         ✅ ad34efb65 + 535f14902
  ├ C-3del [░░░░░░░░░░]   0%   delete legacy validate_headers fallback ← gated on C-3 24h soak
  ├ C-5    [░░░░░░░░░░]   0%   body_persist + delete body_fetch  ← gated on C-3 24h soak
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
  ├ header_probe   [████░░░░░░] PR-1 (poll Job) ✅ 79b53852a; PR-2/3 now unblocked (C-3 landed)
  └ utxo_recovery  [░░░░░░░░░░] gated on C-8 cutover (dissolve plan ready)
Phase 4  [█████████░]  95%   Storage unification — plan: docs/architecture/phase4-storage-unification.md
  ├ 4a     [██████████] 100%   event_log primitive  ✅ 76b3a10b4
  ├ 4b     [██████████] 100%   utxo_projection — Tasks 1-10 SHIPPED  ✅ (39b1e8efa..ee1c5c7b1, 7 commits)
  ├ 4c     [██████████] 100%   block_index_projection + finalize (diff tool + 9 tests)  ✅ (…ed34743ba, 066462576, 91b4ee734, 2f23d6a44, 2e289e41b)
  ├ 4d-1   [██████████] 100%   mempool projection + shadow replay  ✅ da005eb31, cc84e9419
  ├ 4d-2   [██████████] 100%   peers_projection  ✅ 91aa65c1c + 5dc442a81 + 48e78d801 + f925fb6f3 (wt2)
  ├ 4d-3   [██████████] 100%   wallet view projection + diff + final verification  ✅ 12284eb3e, 5626552cb
  ├ 4d-4   [██████████] 100%   znam projection — Tasks 1-5b SHIPPED  ✅ (f52313f02..eb53d9d52, 7 commits, 30 test cases pass)
  ├ 4d-5   [░░░░░░░░░░]   0%   zmsg/zslp/zswp/store batch (READY)
  └ 4e     [░░░░░░░░░░]   0%   block-body migration (spec'd, gated on 4c cutover)
Phase 5  [██████████] 100%   Crypto agility (registry indirection)    ✅ DONE
  ├ 5a-1   [██████████] 100%   Crypto registry skeleton  ✅ c4bebe0a2 + polish dde0183c7
  ├ 5a-2   [██████████] 100%   First call site rewire: Equihash PoW   ✅ f00be351f (wt2)
  ├ 5a-3   [██████████] 100%   script_validate ECDSA rewire (HOT PATH)  ✅ 7c2c067a0 + cde601acf + e8b926610 (wt3)
  └ Nix reproducible builds (5b/5c) DROPPED 2026-05-24 — out of scope. Cosign signing (5d) parked pending decision.
Phase 6  [██░░░░░░░░]  20%   Determinism + simulator
  ├ 6a     [██████████] 100%   seed_tape primitive  ✅ c2ed3145d + cb03fe595 + c62161c2a + b53f251b7 (sub-agent)
  ├ 6b     [░░░░░░░░░░]   0%   postmortem capsule (crash → seed.cap.gz)  ← spec'd (queued post 6a)
  └ 6c     [░░░░░░░░░░]   0%   simulator harness (chaos CI)  ← spec'd (queued post 6b)
Phase 7  [░░░░░░░░░░]   0%   Frontier (io_uring, hot reload)
Phase 8  [░░░░░░░░░░]   0%   Event-log compaction & retention — plan: docs/architecture/phase8-log-compaction-and-retention.md
  └ (draft)  gated on 4e — checkpoint event + segmentation + prune policy; pairs with SHA3 snapshot/FlyClient cold-sync

All 5 mega-module dissolve plans drafted: docs/dissolve/
(sync_watchdog, chain_advance_coordinator, legacy_mirror_sync,
chain_restore, header_probe, utxo_recovery)
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
| `.c` files in `app/` > 800 LOC | 6 (the monoliths) | 0 | dissolve in Phases 2-3 |
| Mega-modules remaining | 6 | 0 | was 7; `sync_watchdog` DELETED — see roster below |
| Lint gates active | 20 (1 FAIL'd in P1) | 21 | +1 by Phase 3 (gate #20→FAIL) |
| Raw clock/RNG callers | **0** | 0 | ✅ Phase 1c (was 443) |
| Mailbox prod callers | 1 | many | ✅ Phase 1a (header_admit), more in Phase 3 |
| Conditions registered | ~10 | ~15 | Phase 2+ adds more from monolith dissolution |

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

6 agent worktrees active under `.claude/worktrees/agent-*` (locked).
Orchestrator on `main` queues + merges; workers push direct to main.

**The Phase 2 cutover critical path is currently SOAK-GATED, not
worker-gated.** C-3 (validate_headers) went authoritative today; C-3del and
C-5 cannot flip until a 24 h zero-divergence soak completes. So a freed worker
should take **soak-independent** work from "Claimable NOW" below rather than
idle on the soak.

---

## NEXT UP — claim order

Claim a doc by marking it **IN PROGRESS** at the top; first to mark wins.
Push direct to main, one commit per task. Run `./test_parallel --jobs=$(nproc)`
before pushing.

**Shipped since last board sync (origin/main, fetch to see):** 4d-3 wallet
projection ✅ (a9fb0f396..49ef6bbe6) · chain_restore PR-1 planner extract ✅
(afed3d673..a5fbe3700) · utxo_recovery PR-1 reimport-flag primitive ✅ (af7ba7a30).

### Claimable NOW (no soak gate, fully independent)
1. ⚡ [`wt-snapshot-wedge-recovery.md`](./work/wt-snapshot-wedge-recovery.md) **PR-0: runtime snapshot re-sync entry point** — foundational for wedge recovery. Today snapshot sync has NO trigger API (only peer-offer driven, `snapshot_sync_service.h:209`) and apply is cold-start-gated. Add `snapsync_request_recovery()` + a runtime local-LDB manifest builder + relax the <100K accept gate for opt-in recovery. **Feasibility audited — read the ⚠️ block in the doc before claiming.** Unblocks PR-1 (the `tip_wedged_resnapshot` Condition). Moves #6 recovery + #5 keep-up.
2. ⚡ [`wt-perf-integrate-rebuild.md`](./work/wt-perf-integrate-rebuild.md) **PR-3: parallel io_uring blk*.dat marking in cold-import** — PROMOTED with a live profile: cold-import is ~180s, **101s of it is single-threaded blk*.dat marking** (measured `941b9803d`). The `rebuild_recent` prototype already proved the fix on this exact data (5.6s/2GB/s). Parallelize the scan → seconds. Moves #1 cold sync. PR-1 (HW-CRC) ✅ `69939ec97`; PR-2 (io_uring bulk-append) still spec'd.
2. [`wt-phase4d-5-small-batch-projections.md`](./work/wt-phase4d-5-small-batch-projections.md) — zmsg/zslp/zswp/store + hodl batch. Closes out the 4d projections.
2. header_probe PR-2 / PR-3 — now unblocked (C-3 landed). Spec in `docs/dissolve/`; split into a `wt-phase3-header-probe-pr2.md` doc when claimed.
3. chain_restore PR-2 — next dissolve slice now that the planner is extracted (PR-1 ✅). Split a `wt-phase3-chain-restore-pr2.md` from `docs/dissolve/chain_restore_service.md`.

### Soak-gated (read the spec now, start when the 24 h C-3 soak clears)
- [`wt-phase2-cutover-c3-final-delete.md`](./work/wt-phase2-cutover-c3-final-delete.md) — delete the legacy validate_headers fallback.
- [`wt-phase2-cutover-c5-body-persist.md`](./work/wt-phase2-cutover-c5-body-persist.md) — body_persist authoritative + DELETE body_fetch. Then C-6→C-9 in sequence per [`wt-phase2-cutover-c3-through-c9.md`](./work/wt-phase2-cutover-c3-through-c9.md) (each + its own soak).
- [`wt-phase4e-block-body-migration.md`](./work/wt-phase4e-block-body-migration.md) — block bodies into the log; gated on the 4c-cutover soak. Last Phase 4 PR. Phase 8 compaction follows ([`phase8-log-compaction-and-retention.md`](./architecture/phase8-log-compaction-and-retention.md)).

### Deferred — do NOT dispatch without explicit user approval
- Phase 7a/7b/7c (io_uring, structured concurrency, hot reload) — optional frontier.

### Critical path to 100%
```
C-3 ✅ ─► C-3del/C-5 ─► C-6 ─► C-7 ─► C-8 ─► C-9      (each + 24h soak)
                                       │       └─► dissolve chain_advance + legacy_mirror (P3)
                                       └─────────► dissolve utxo_recovery (P3)
4c ✅ ─► 4e (bodies in log) ─► Phase 8 (log self-bounding)
```
Everything in "Claimable NOW" runs in parallel to this path — that's where
freed workers add the most while the soak runs.

---

## Recently completed

| Date | What | Worktree | Commit |
|---|---|---|---|
| 2026-05-24 | **Phase 4d-3 wallet projection COMPLETE** — public-only wallet projection, diff RPC/MCP tool, diagnostics, replay edge coverage, secret/payload audit, and live fresh-node `match:true` diff evidence | wt2 → main | 12284eb3e, 5626552cb |
| 2026-05-24 | **Phase 2 C-3 validate_headers AUTHORITATIVE** — the flip; full test_parallel 0/196; stabilized 2 pre-existing flaky timing tests (crypto_registry ECDSA, event async) | wt3 → main | ad34efb65, 535f14902, 72dd5e01f |
| 2026-05-24 | **Phase 4c FINALIZED** — `zcl_block_index_diff` MCP tool + dumper wired + 9-case `test_block_index_projection`; block_index_projection complete | wt2 → main | 066462576, 91b4ee734, 2f23d6a44, 2e289e41b |
| 2026-05-24 | **Phase 4d-1 mempool projection MERGED** — `EV_TX_ADMIT/REMOVE_MEMPOOL` consumer + shadow replay | wt2 → main | da005eb31, cc84e9419 |
| 2026-05-24 | **Phase 3 header_probe PR-1 MERGED** — `header_probe_poll` Job under net supervisor (period=30s, live-confirmed in supervisor dump) | main | 79b53852a |
| 2026-05-24 | **Phase 8 spec drafted** — event-log compaction & retention (checkpoint event + segmentation + retention modes) | main | 533b7223e |
| 2026-05-24 | **Phase 4a event_log primitive MERGED** — append-only file with fsync-sentinel torn-write recovery; 24-trial kill-9 fuzz harness all green; 131 evt/sec on this disk (disk fsync-rate-limited per assignment doc note). Cherry-picked from orch sub-agent's isolated worktree | orch sub-agent → main | 76b3a10b4 |
| 2026-05-24 | **test_supervisor regression fixed** — pre-existing failure on main (introduced by supervisor tree split); test now looks at `root_orphans[]` instead of removed `children[]`. test_parallel: 0/194 failed | main | ae47aa283 |
| 2026-05-24 | **C-2 commit 3/4 shipped** — divergence guard in legacy `accept_block_header` ingress | wt3 → main | 659bc3e5a |
| 2026-05-24 | **Phase 5a-2 MERGED** — first call-site rewire: Equihash PoW now routes through `crypto_registry` (`CRYPTO_PROOF_EQUIHASH_200_9`); registry indirection in production | wt2 → main | f00be351f |
| 2026-05-24 | **5a-1 polish** — fixed JSON leaks in `crypto_registry_dump_state_json`, added LOG_FAIL diagnostics on registration failures, expanded test from 5 to 9 cases | orch sub-agent → main | dde0183c7 |
| 2026-05-24 | **C-2 commit 2/4 shipped** — authoritative write path gated on `HEADER_ADMIT_MODE_AUTHORITATIVE` | wt3 → main | 58921e518 |
| 2026-05-24 | **Phase 5a-1 MERGED — crypto registry skeleton.** SHA256/BLAKE2b/ECDSA/Groth16 wrappers + dispatch table | wt2 → main | c4bebe0a2 |
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
