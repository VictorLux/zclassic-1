# Build Checklist — the road to the beautiful node

> The single tracker for **everything that needs to be created.** Architecture
> and the laws: [`FRAMEWORK.md`](./FRAMEWORK.md). This file is the *work*: every
> item is a checkbox, grouped by workstream, ordered by dependency. Check it off
> when it's shipped + proven. Read this first when you start a session.
>
> **Updated:** 2026-05-26. Goal = the Prime Directive: *log-as-truth + pure
> projections + single reducer; advance-cursor-or-name-blocker; health = the gap.*
> Five workstreams: **B** flips authority (the north star), **A** makes the canon
> true, **C** dissolves the legacy, **D** restores beauty, **E** enforces it.

---

## Scoreboard — four promises (live truth, not green tests)

Read live from the SERVICE: `zcl_status` (tip, `tip_advance_age_seconds`) +
`zcl_state subsystem=blocker`. Measured numbers live in
[`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md) (append-only); don't re-quote them here.

```
  ⚡ FAST          cold-sync 180s→30s ▸ building   warm 37.7s→10s ▸   stay-at-tip ◑ (cutover)
  🪶 LEAN          binary 15.4MB ✓ slim            RSS ~2GB→1GB ▲ climbs w/ bg-verify
  💪 UNBREAKABLE   alert loop ✓ closed             can't-halt-by-construction ◑ needs cutover (B)
  🔬 HONEST        zcl_status live truth ✓          bug→repro ✓ (chaos/postmortem)
  ✓ done · ▸ building · ◑ fixed-in-code-not-structural · ▲ above target
```

**Owner mandate (standing):** NO whack-a-mole. The node becomes unbreakable by
*finishing* the refactor — collapse to ONE path, DELETE the legacy — not by
adding conditions. Default to SUBTRACTION. While two chain paths coexist a silent
halt remains *possible*; workstream **B** is the structural cure.

---

## ✅ Already shipped (the foundation)

The primitives the north star needs all exist and are tested — they just run in
**shadow**. This is why the work ahead is *flipping authority*, not building.

- [x] Condition engine + 22 conditions (the model-citizen shape) · Phase 0
- [x] Kernel primitives adopted: mailbox, projection, platform.clock/rng (0 raw clock/RNG callers) · Phase 1
- [x] Wave-S stage pipeline, all 8 stages, SHADOW complete (header_admit → tip_finalize) · Phase 2
- [x] event_log — durable, fsync'd, CRC32C, torn-write recovery, SHA3 fingerprint · Phase 4a
- [x] 8 pure-fold projections: utxo, block_index, mempool, peers, wallet, znam, contacts/onion/hodl · Phase 4b–d
- [x] Crypto-agility registry + hot-path rewires (Equihash, ECDSA) · Phase 5
- [x] Determinism: seed_tape + postmortem capsule + `make chaos` · Phase 6
- [x] Supervisor tree split into 7 domain supervisors · Phase 3
- [x] Dissolved already: chain_restore_service, sync_watchdog_service, header_probe_service, chain_evidence_controller
- [x] **Reorg keystone (2026-05-26):** disconnect emits inverse UTXO deltas (`bfa379bc8`) + byte-exact convergence proof (`1e65f81a0`)
- [x] Silent-halt escalation closed: `EV_OPERATOR_NEEDED` → sinks + `zcl_status` DEGRADED + sd_notify
- [x] PROVE Tier-1: offline PoW/integrity sweep `zcl_replay_verify` (`63a9a5de4`) + shadow_replay_proof
- [x] Test harness: `test_parallel --only=SUBSTR` (1s iteration vs 110s full)

---

## B — THE CUTOVER: flip authority from coins.db to the log  ← the north star

Dependency-ordered. This is the structural cure; everything in **C** is gated on
it. Plan detail: [`work/cutover.md`](./work/cutover.md).

- [x] **B1** Reorg-capability in `tip_finalize` — disconnect emits inverse deltas; parity proven byte-exact.
- [ ] **B2** Emit block bodies to the log. `EV_BLOCK_BODY` (type 2) has **zero emitters** today; bodies live in `blocks/*.dat`. Without bodies in the log, the log can't be replayed to rebuild UTXO from scratch — only trail. *(Phase 4e)*
- [ ] **B3** Invert the UTXO emitter. Make `utxo_apply_stage` the **author** of `EV_UTXO_ADD/SPEND` and the **sole writer** of the utxo projection — instead of `update_coins.c` shadow-emitting *after* legacy already wrote. *Acceptance:* utxo projection driven by the stage, not by legacy.
- [ ] **B4** Point `connect_block` input lookups at the utxo **projection** instead of `coins_view_cache`. Closes the validation feedback loop (Prime Directive). Keep a RAM read-cache for speed; authority is the projection; its write lands in the same txn as the cursor advance.
- [ ] **B5** Make `log_head` / the `tip_finalize` cursor the **definitional tip**. Demote `chain_active` to a derived in-RAM index rebuilt from `block_index_projection`. *Acceptance:* `health = network_tip − log_head` is one real number.
- [x] **B6** Offline PROVE harness complete: Tier-1 full-0→tip driver emits `shadow_replay_proof: 0 divergences across N blocks, commit <sha>`; `--deep`/`--tier2` runs full PoW/script/Groth16 sweep via `replay_verify_run`; reorg corpus (`test_reorg_projection_parity`) byte-exact; full-driver CI test. `e7c5c4b74`.
- [ ] **B7** Flip once, behind the guard. `cutovermode all authoritative` + `cutover_no_forward_progress` auto-revert (180s no-progress → revert to SHADOW + page). Real canary: one block connects through the authoritative path, auto-revert on any divergence.
- [ ] **B8** Extract-then-delete (see **C**) — the legacy path + the entire shadow-vs-legacy comparison apparatus (`diff_with_legacy_shadow`, `shadow_feeder`, the `*_projection_diff` MCP tools, cutover mode/preflight/canary plumbing). Once there's one path, the comparison apparatus is dead.

---

## A — Make the canon true (the missing shape primitives)

`FRAMEWORK.md` blesses **struct-registration**, not block-macro DSLs (Law 3).
So this is NOT about building fictional `MODEL(){…}` macros — it's giving the
three unreal shapes a real, debuggable form.

- [ ] **A1** `lib/framework/job.h` — one uniform Job contract: a step fn returning `JOB_{ADVANCED,BLOCKED,IDLE,FATAL}` + a durable cursor. Then **relocate the 8 `*_stage.c`** from `app/services/` into `app/jobs/` and rename `stage_result_t` → `JOB_*`. *Acceptance:* `app/jobs/` holds the reducer; the empty-scaffold folder becomes real.
- [x] **A2** Event-shape decision (orchestrator, 2026-05-26): the Event shape STAYS; its implementation is lib-resident today (`lib/event/` in-mem observability ring + `lib/storage/event_log` durable log). `app/events/` is reserved for app-level event definitions + subscriber wiring and gets populated by **B2** (block-body emit) as the log becomes authoritative — not deleted. FRAMEWORK §3 Event row states this.
- [x] **A3** Supervisor declarations extracted into `app/supervisors/src/{net,chain,staged_sync}_supervisor.c` (8 Wave-S children in pipeline order); `boot_services.c` 3,885 → 3,270 LOC (−615); boot ordering preserved. `fa9e8d0ec`.
- [ ] **A4** `zcl_result` adoption for services (Law 2). Migrate services off bare `bool`/`int`. *(ratchet — see E2)*
- [ ] **A5** *(optional)* `tools/` codegen that emits shape skeletons (model, condition, job, controller) — the easy path is the correct path. Generates readable, steppable, committed source. Not metaprogramming.

---

## C — Dissolve the mega-modules (extract-then-delete)

3 modules = 4,407 LOC. Inventory + caller maps verified 2026-05-25. All gated on
**B** (they own behavior the new path lacks until the flip).

- [ ] **C1** `chain_advance_coordinator.c` (1,716) → re-home `score_source()` source-selection into `header_probe`/`block_source_policy`; DELETE the mirror force-promotion window. *Gated on B.*
- [ ] **C2** `legacy_mirror_sync_service.c` (1,487) → extract the live-sync **heartbeat + lag-SLO monitor** to a lean monitor (PRESERVE it); delete only the block-application coordination. *Gated on B.*
- [ ] **C3** `utxo_recovery_service.c` (1,204) → re-home `clean_above_tip` (orphan-UTXO heal) as a **Condition** first; `restore_chain_tip` / `import_ldb` to a recovery path. *Gated on B.*
- [ ] **C4** Collapse the **4 importers** (5,519 LOC: `legacy_bootstrap_importer` + `legacy_mirror_sync` + `legacy_import` + `sync_controller_import`) → one `legacy_bridge` + one `legacy_poll` job. [`work/wt-consolidate-import-paths.md`](./work/wt-consolidate-import-paths.md). *Independent of B.*

---

## D — Restore beauty (shape conformance — independent, high-leverage)

From the beauty audit; each is "principle violated → where → the elegant form."

- [x] **D1** Dissolve `diagnostics_controller.c` — 2,550 → 51 LOC, split into 6 single-concern files (diagnostics_registry + cutover/projection_diff/nodelog/dbquery/probe controllers). `da9a1fe5a`.
- [x] **D2** Controllers must not build views — explorer_factoids/stats/pages assembly moved into `app/views/` (controllers now skinny). `dd041e3b8`.
- [x] **D3** `header_admit_log` now a Model (`app/models/src/header_admit_log.c`, validates_* + before/after_save + `AR_ADHOC_SAVE`); raw SQL removed from the stage's write path. `aa9b6aaa7`.
- [ ] **D4** One log call. Collapse the 486 raw `fprintf(stderr)` + `// obs-ok:` sites to `LOG_*`/`log_json` so `zcl_node_log` is structured.
- [ ] **D5** The 31 `app/` files > 800 LOC → under the cap (mega-module split + dead-code removal). Tracks toward E1.

---

## E — Enforce it (every law gets a gate; "beauty by the build")

Hygiene is well-gated (21 live gates). **Architecture is not.** Each gate lands
with the work it guards (design in `FRAMEWORK.md` §5).

- [x] **E1** `file-size-ceiling` — RATCHET, baseline = 29 grandfathered files (can only shrink). `5daf21742`.
- [x] **E2** `check-one-result-type` — RATCHET (67 service files baselined; new ones must use `zcl_result`). `f331f6e0d`.
- [x] **E3** `check-shape-includes-header` — HARD; every condition/model/supervisor file includes its shape header. `f331f6e0d`.
- [x] **E4** `check-projections-pure` — HARD; every `*_projection.c` is a pure fold (no app includes, no AR model saves). `f331f6e0d`.
- [ ] **E5** `stage-advances-or-blocks` — every Job references a cursor and calls `blocker_set()` on non-progress. *(hard for the Job shape)*
- [ ] **E6** `one-write-path` — exactly one writer to chain state. *(ratchet → hard once B7 lands)*
- [ ] **E7** `no-authoritative-RAM-state` — consensus state in log/projections/cursors, not mutable globals. *(ratchet)*
- [ ] **E8** `health-is-the-gap` — one `tip_not_advancing` Condition is the sole liveness authority; others don't emit `EV_OPERATOR_NEEDED` for liveness. *(ratchet)*
- [x] **E9** `operator-needed-has-a-sink` — HARD; `EV_OPERATOR_NEEDED` emit paired with the `alerts.c` subscriber (tree already satisfies). `5daf21742`.
- [x] **E10** `framework-shape` + `controller-SQL` graduated WARN → RATCHET (baselines captured; new violators fail). `5daf21742`.
- [x] **E11** `doc-accuracy` — HARD; `<!--LINT-GATES-->` block in DEFENSIVE_CODING.md must match Makefile `lint:` deps. 24 gates, agree. `5daf21742`.

---

## Decision log (binding)

- **2026-05-26** Canon made honest: `FRAMEWORK.md` rewritten to bless struct-registration over fictional block-DSLs; the Ten Laws + Prime Directive + validation-feedback honesty added; every law mapped to a gate. `VISION.md` + `ARCHITECTURE.md` (old L1–L7 layer cake) merged in and deleted.
- **2026-05-23** Framework adopted: Rails-style MVC + Phoenix-style supervisors + hexagonal cut + Conditions. Strangler execution (per-module PRs), not big-bang. Lint gates ratchet WARN→FAIL.
- **2026-05-23** Worktree workflow: separate clones at `~/github/zclassic23-{2,3}`, identified by pwd suffix, pushed to origin for orchestrator merge.

## How this file gets updated

- Check an item `[x]` only when it's shipped **and** proven (forward-progress / test / measured delta). Add a row to `BENCHMARKS_LOG.md` for any number moved.
- Orchestrator (main) edits this file; workers append a Completion section to their own `docs/work/wtN-*.md`. Recent history lives in `git log`, not here.
