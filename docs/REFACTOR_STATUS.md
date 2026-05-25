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
     Stay at tip               HALTED ~18 behind   keep <1 blk    ✗ BIP30 self-write recurs at tip; real fix queued

  🪶 LEAN
     Memory (RSS)              2.13 GB (live)*     1.0 GB        ▲ climbs w/ bg-verify
     Binary size               15.4 MB (05-24)     stay slim     ✓ met (docs' 26MB stale)

  💪 UNBREAKABLE  ← resilience is a PROMISE, measured by truth not by "result=ok"
     Tip advancing             NO — re-halts/blk  always         ✗ connect_block rejects own coinbase (BIP30 self-write)
     Self-heal tells the truth code: gated (47bdbc211) 0 false-ok ◑ fixed in tree · live still lies
     Halt/crash recovery      180s, manual        <60s, auto    ◑ resnapshot path landed (8e25887b0); BIP30 fix + live timing pending
     Uptime before failure     halted ~5h, 12 restarts 30 days    ✗ restart loop went quiet — now silently stuck
     Alerts to a human         code: 19 Conditions  0 / month     ◑ 19 self-heal Conditions in tree · live node still pages nobody

  🔬 HONEST
     Live truth from service   zcl_status (C)      always true    ◑ build-commit + dominant-blocker → surface in zcl_status
     Bug → reproducible fix    built (postmortem+chaos) 1 tape    ✓ Phase 6 done (6fb76f2b0)
```
`*` RSS soak (05-24): fresh boot 1.53 GB → **stair-steps up with bg-validation
depth** → ~2.4 GB by 17min (only 6.6% validated), still creeping. Live now reads
2.13 GB at ~5h halted. NOT bounded at a low plateau — tracks how much chain
bg-verify has buffered. >2× the 1 GB target; real ceiling needs the full ~8h
run. `✓` met · `▸` in flight · `◷` measuring · `◑` fixed-in-code-not-deployed ·
`▲` above target · `✗` BROKEN/regressed.

### ✅ RESOLVED 2026-05-25 — halt cured; focus = one-path cutover + DRY  (history kept below for context)

**The live node no longer halts** — verified HEALTHY 2026-05-25 (advancing past
the old halt block, 0 restarts), via the `connect_block` self-write + write-order
fix (shipped, `work/done/wt-connect-bip30-selfwrite.md`) + a clean rebuild from
the local `zclassicd` when on-disk state was already torn. **Silent-halt
escalation is now closed:** `EV_OPERATOR_NEEDED` → alert sinks + `zcl_status`
DEGRADED + sd_notify — a halt can no longer page nobody. **The structural cure is
the cutover: collapse to ONE path and delete the legacy (~12.5K LOC)** — a node
where every step must "advance the cursor or name a typed blocker" can't halt
silently. See [[project_silent_halt_architecture_diagnosis_2026-05-25]] (memory).
Condition consolidation is owned by wt3. Historical root-cause analysis follows:

**Earlier diagnosis was WRONG and is corrected here.** The halt is *not* a
cutover consensus divergence. The cutover (C-3, `ad34efb65`) was only the
**trigger**: it flapped the chain, `chain_tip_watchdog` kill-9'd the node 12×
(`NRestarts=12`), and one of those kills hit the at-tip ordering hazard
([[feedback_at_tip_kill9_ordering_invariant]] — coins.db commits before the
block_index fsync). That left a **torn coins state** that is now the active,
standalone halt:

**Root cause — PROVEN, deeper than first thought (2026-05-25):** the node freezes
~1 block below the tip with `connect_block FAILED: bad-txns-BIP30`, **and it
recurs at every tip advance.** A cold-import from the good local `zclassicd`
closed the 536-block gap but the halt just **moved** 3,123,689 → 3,124,225.
Live `node.db`:
```
chain tip          = 3,124,224
utxos MAX(height)  = 3,124,225   (one row: txid 98963472…, vout 0, is_coinbase=1)
→ block 3,124,225's OWN coinbase is in the UTXO set while the tip is 3,124,224.
  connect_block runs BIP30, sees the block's own coinbase already present, and
  rejects the block as a duplicate-overwrite.
```
So the UTXO set sits **one block ahead of the block-index tip**, and BIP30 treats
the node's own coinbase as a consensus violation. **Post-BIP34 (coinbase txids
are height-unique), BIP30 can NEVER legitimately fire at these heights — so this
is ALWAYS a false positive on stale local data, never a real duplicate.**

**The symptom-chasers only move the halt:**
| Attempt | Commit | Result |
|---|---|---|
| boot single-block rewind | `dbf4845a1` | clears one row on boot; tip re-halts at the next block |
| cold-import from zclassicd | (manual) | closed 536-blk gap, halt moved 3,123,689 → 3,124,225 |
| cutover→shadow / restart-cap / self-heal-witness | `6e0f6a82c` `82ec4e11f` `47bdbc211` | fix the trigger/loop/lie; do NOT cure the BIP30 false-positive |

**The cure (new P0 assignment):**
[`work/wt-connect-bip30-selfwrite.md`](./work/wt-connect-bip30-selfwrite.md) —
(1) `connect_block` must not reject a block's OWN same-height coinbase (it's a
stale self-write, impossible to be a real duplicate post-BIP34); (2) fix the write
ordering so the UTXO set never commits ahead of the block-index tip. Acceptance is
**sustained LIVE forward progress at the tip**, not a unit test.

**RESILIENCE DOCTRINE (new, load-bearing):**
1. **A green test suite is not a healthy node.** No cutover is "done" until the
   *live* node advances past the cutover height. Forward-progress is the gate.
2. **A remedy that returns `ok` must resolve the symptom.** A Condition that
   reports success while its symptom persists is worse than none — it hides the
   failure. Verify by symptom delta, not by "the remedy ran."
3. **The scoreboard must read live truth, not a cached snapshot.** "At tip" =
   `tip_advance_age < threshold` AND `gap == 0`, sampled now.
These three are now first-class promises under UNBREAKABLE/HONEST above.

**When you finish a task, name the goal you moved and the measured delta**
(e.g. "warm restart 33s→29s"), then add a row to BENCHMARKS_LOG.md.

### Who's moving what right now

**OWNER MANDATE (2026-05-25):** NO whack-a-mole. The node still halts because we
maintain TWO chain paths — the robust stage pipeline AND the legacy
connect_tip/chain_advance_coordinator/legacy_mirror mega-modules that run the
live tip. **Robustness = finishing the refactor: collapse to ONE path, DELETE the
legacy.** Focus = refactor + DRY + cruft removal + high-performance. SUBTRACTION,
not more conditions. ~17K LOC of cruft to remove (inventory 2026-05-25).

| Work | Goal | Who |
|---|---|---|
| **Master lever:** safe-flip guard + cutover C-3→C-9, DELETE legacy path | One path; ~12.5K LOC out; never-silent | **wt2/wt3** (unblocked: healthy node) |
| Recovery consolidation (DRY) | Lean, uptime | ✅ wt3 (`f7a643442`) |
| Import-path DRY: 3 importers → 1 (`wt-consolidate-import-paths.md`) | Lean; ~1.7K LOC out | **unclaimed** |
| Never-silent alert loop (`EV_OPERATOR_NEEDED` → sinks) | Alerts to a human | ✅ orchestrator |
| Explorer controller cruft removal | Lean; −118 LOC | orchestrator (integrating) |
| chain_advance + legacy_mirror + snapshot sprawl dissolve | ~9K LOC out | gated on cutover (C-8/C-9) |

---

## Phases

```
Phase 0  [██████████] 100%   Condition engine + scaffold              ✅ DONE
Phase 1  [██████████] 100%   Adopt unused primitives                  ✅ DONE
Phase 2  [██████████] 100%   Wave S SHADOW complete (S-1..S-9 all shipped)  ✅
  ├ S-5..S-7 [██████████] 100%   body_persist, script_validate, proof_validate ✅
  ├ S-8    [██████████] 100%   utxo_apply shadow (wt3)                ✅ 497220f58
  └ S-9    [██████████] 100%   tip_finalize shadow (wt3)              ✅ 1a65b33c7
Phase 2 CUTOVER [█░░░░░░░░░]  ~8%   Flip shadow → authoritative   ⚠ REVERTED — 0/7 stages authoritative in prod
  ├ UNHALT FIRST [░░░░░░░░░░] 0%  live node halted on BIP30 stale-coins (see P0) — fix that before ANY re-flip
  ├ SAFE-FLIP GUARD [░░░░░░░░] 0%  build the auto-revert-on-no-forward-progress Condition + follow the
  │                                protocol BEFORE re-flipping — work/cutover-safety-protocol.md. Without it,
  │                                the next flip can silently halt the chain again (exactly what C-3 did).
  ├ C-2    [███████░░░] flipped, then REVERTED   header_admit: flip f3f0c6c4e → set back to SHADOW 6e0f6a82c
  ├ C-3    [███████░░░] flipped, HALTED, REVERTED validate_headers: flip ad34efb65 → froze chain → SHADOW 6e0f6a82c
  ├ C-3del [░░░░░░░░░░]   0%   delete legacy validate_headers fallback ← gated on root-cause + clean re-flip
  ├ C-5    [░░░░░░░░░░]   0%   body_persist + delete body_fetch  ← gated on root-cause + clean re-flip
  ├ C-6    [░░░░░░░░░░]   0%   script_validate authoritative (batch spec, post C-5)
  ├ C-7    [░░░░░░░░░░]   0%   proof_validate authoritative (batch spec, post C-6)
  ├ C-8    [░░░░░░░░░░]   0%   utxo_apply authoritative (batch spec, post C-7 — gates utxo_recovery dissolve)
  └ C-9    [░░░░░░░░░░]   0%   tip_finalize authoritative (batch spec, post C-8 — gates chain_advance dissolve)
  NOTE: shadow stages (Phase 2 SHADOW, 100%) all run + match in shadow. The CUTOVER is the act of
        trusting them as authoritative. C-2/C-3 proved the flip mechanism works, but the live node is
        halted on a SEPARATE bug (BIP30 stale coins, see P0) — unhalt first, then re-flip on a clean node.
Phase 3  [███████░░░]  70%   Dissolve mega-modules                    ← partial
  ├ watchdog [██████████] 100%   sync_watchdog_service.c DELETED      ✅ 611631541
  ├ supervisor tree split [██████████] 100%   7 domain supervisors    ✅ dae31dee9
  ├ chain_advance  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ legacy_mirror  [░░░░░░░░░░] gated on C-9 cutover (dissolve plan ready)
  ├ chain_restore  [██████████] 100%   service/header deleted; focused modules own restore ✅
  ├ header_probe   [██████████] 100%   core renamed; old service file deleted ✅ d17eb5ca0
  └ utxo_recovery  [░░░░░░░░░░] gated on C-8 cutover (dissolve plan ready)
Phase 4  [█████████░]  95%   Storage unification — plan: docs/architecture/phase4-storage-unification.md
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
Phase 8  [░░░░░░░░░░]   0%   Event-log compaction & retention — plan: docs/architecture/phase8-log-compaction-and-retention.md
  └ (draft)  gated on 4e — checkpoint event + segmentation + prune policy; pairs with SHA3 snapshot/FlyClient cold-sync

Remaining dissolve plans: docs/dissolve/ (chain_advance_coordinator,
legacy_mirror_sync, utxo_recovery). The sync_watchdog / chain_restore /
header_probe plans were deleted 2026-05-25 — those mega-modules are dissolved.
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
| Conditions registered | 19 | ~15 | `chain_stalled_with_data` retired; activation-no-progress routes through `legacy_mirror_stuck` |

> User-facing numbers (MTBF/RSS/cold/warm/kill-9/pages) intentionally NOT
> tabled here — they rot. Live values: scoreboard at top + `BENCHMARKS_LOG.md`.

---

## Mega-module roster (Phase 2-3 deletion targets)

| File | LOC | Dissolves into | Phase |
|---|---|---|---|
| `chain_advance_coordinator.c` | 1,715 | `services/sync/source_scorer.c` + `jobs/tip_finalize.c` + 1 condition | 2 (S-9) |
| `chain_restore_service.{c,h}` | DELETED | `chain_restore_{planner,executor,repair,integrity,boot_activation,boot_snapshot}.{c,h}` | 3 |
| `sync_watchdog_service.c` | DELETED | replaced by 8 supervised conditions | 3 (PR-3) |
| `legacy_mirror_sync_service.c` | 1,410 | `services/sync/legacy_bridge.c` + `jobs/legacy_poll.c` + 1 condition | 2 (S-12) |
| `header_probe_service.c` | DELETED | `header_probe.c` + `legacy_header_client.c` + mailbox use | 3 (PR-3) |
| `utxo_recovery_service.c` | 1,241 | `conditions/utxo_drift.c` + `jobs/utxo_repair.c` | 3 |
| `chain_evidence_controller.c` | 1,083 | `services/chain/evidence.c` + 1 condition | 2 (S-9) |

---

## In flight (worktrees)

**Workers LIVE (2026-05-25):** wt2 → BIP30 stale-coins unhalt (`ed799dd4a`),
wt3 → cutover safety guard (`f711ac7b0`) — the two critical-path items.
Orchestrator on `main` queues + curates; workers push direct to main.
(Housekeeping: 6 orphaned dead-session sub-agent worktrees pruned; 3 live ones
remain under `.claude/worktrees/agent-*`.)

**The Phase 2 cutover critical path is UNHALT-BLOCKED, not soak-gated.** C-2/C-3
went authoritative, the chain halted, and `6e0f6a82c` reverted both stages to
shadow. The live node is now frozen on a SEPARATE bug — BIP30 stale coins (see
P0), not a header divergence. No further flip (C-3del, C-5..C-9) should proceed
until (1) the live node is unhalted, and (2) a clean re-flip confirms the
authoritative header path actually matches legacy past the cutover height. A
freed worker should take the BIP30 unhalt or pull **independent** work from
"Claimable NOW" below.

---

## NEXT UP — claim order

Claim a doc by marking it **IN PROGRESS** at the top; first to mark wins.
Push direct to main, one commit per task. Run `./test_parallel --jobs=$(nproc)`
before pushing.

**Shipped since last board sync (origin/main, fetch to see):** 4d-3 wallet
projection ✅ (a9fb0f396..49ef6bbe6) · chain_restore PR-1 planner extract ✅
(afed3d673..a5fbe3700) · chain_restore PR-1b boot snapshot extract ✅
(462be5e5a) · chain_restore PR-2a executor extract ✅ (6ec178eb6) ·
chain_restore PR-2b repair extract ✅ (5042fde7b) ·
chain_restore service implementation delete ✅ (89892c441) ·
chain_restore compatibility header delete ✅ (8658ef0d2) ·
utxo_recovery PR-1 reimport-flag primitive ✅ (af7ba7a30) · header_probe
dissolve ✅ (981ad4897..1b0847820) · snapshot halt recovery ✅
(4d7f7adee..8e25887b0) · small projections 4d-5 ✅
(0f10cd5f4..2f23d8352) · postmortem capsules ✅
(720906bf4..89fabc360) · chaos simulator harness ✅
(ca74cb4c2..6fb76f2b0).

### Claimable NOW (no soak gate, fully independent)
0. 🔴 **[`wt-connect-bip30-selfwrite.md`](./work/wt-connect-bip30-selfwrite.md) — P0, THE disease: BIP30 self-write halt.** PROVEN root cause of "always stuck": the UTXO set ends up 1 block ahead of the tip, and `connect_block` rejects the block's OWN coinbase as a BIP30 duplicate (impossible post-BIP34 → always a false positive). Recurs at every tip advance; boot-rewind + cold-import only move it. Fix: (1) connect_block tolerates a same-height self-coinbase; (2) write-ordering so coins never commits ahead of the block-index tip. Acceptance = sustained LIVE forward progress. Deploy gated on Rhett.
1. 🆕 **[`wt-bench-harness.md`](./work/wt-bench-harness.md) — READY for wt3, fully independent of P0.** `make bench`: the 5 primaries + `bench-history.csv` + a >20% regression gate. We've quoted perf numbers from ad-hoc runs with no harness — this is the "high-performance" foundation (can't optimize what you can't measure). Isolated datadir/ports, touches no C source wt2 edits. Build now; full to-tip baselines follow the P0 fix.
   - (perf track [`wt-perf-integrate-rebuild.md`](./work/wt-perf-integrate-rebuild.md) is CLOSED: PR-1 HW-CRC ✅, PR-3 parallel scan ✅ but cold-import bypasses it; PR-2 io_uring deferred. Don't re-claim.)
2. 🛡️ [`cutover-safety-protocol.md`](./work/cutover-safety-protocol.md) — auto-revert-on-no-forward-progress Condition (wt3 already shipped the core, `230d9b896`). REQUIRED before any C-* re-flip.
3. More self-heal Conditions — chain_restore/header_probe dissolved (✅); remaining mega-module plans in [`docs/dissolve/`](./dissolve/).

> **Sequencing:** #0 (stop halting) is the gate on everything — a high-performance
> node that's always stuck is worthless. #1 (perf) runs in parallel; it touches
> the cold-import scan, not the connect path, so no conflict with #0.

**QUEUED behind the root fix (DRY / purge — do NOT start while Agent 1 is in
connect/coins/boot):** [`wt-consolidate-recovery-paths.md`](./work/wt-consolidate-recovery-paths.md)
— research-backed cleanup of the whack-a-mole sprawl: the coins-rewind SQL is
copy-pasted in 3 files (→ 1 helper); 17 tip/stall conditions (→ ~10 after the root
fix makes the BIP30 band-aids dead); LEGACY_LIFECYCLE doc-vs-reality drift. Net
LOC down. Gated so it can't collide with the live root-cause work.

### Re-flip-gated (read the spec now; start AFTER unhalt + safe-flip guard + a clean C-3 re-flip — there is no soak running, C-3 is reverted)
- [`wt-phase2-cutover-c3-final-delete.md`](./work/wt-phase2-cutover-c3-final-delete.md) — delete the legacy validate_headers fallback.
- [`wt-phase2-cutover-c5-body-persist.md`](./work/wt-phase2-cutover-c5-body-persist.md) — body_persist authoritative + DELETE body_fetch. Then C-6→C-9 in sequence per [`wt-phase2-cutover-c3-through-c9.md`](./work/wt-phase2-cutover-c3-through-c9.md) (each + its own soak).
- [`wt-phase4e-block-body-migration.md`](./work/wt-phase4e-block-body-migration.md) — block bodies into the log; gated on the 4c-cutover soak. Last Phase 4 PR. Phase 8 compaction follows ([`phase8-log-compaction-and-retention.md`](./architecture/phase8-log-compaction-and-retention.md)).

### Deferred — do NOT dispatch without explicit user approval
- Phase 7a/7b/7c (io_uring, structured concurrency, hot reload) — optional frontier.

### Critical path to 100%
```
UNHALT (BIP30) ─► SAFE-FLIP GUARD ─► re-flip C-3 ─► C-3del/C-5 ─► C-6 ─► C-7 ─► C-8 ─► C-9
  (live P0)         (Condition)        (canary)       │  (each: canary + clean soak)        │
                                                      └─► dissolve utxo_recovery (P3, post C-8)
                                                                  C-9 ─► dissolve chain_advance + legacy_mirror (P3)
4c ✅ ─► 4e (bodies in log) ─► Phase 8 (log self-bounding)
```
C-3 is REVERTED (not ✅) — the chain must be unhalted and the safe-flip guard in
place before any re-flip. Everything in "Claimable NOW" runs in parallel; the
two top items (unhalt, safe-flip guard) ARE the critical path now.

---

## Recently completed

| Date | What | Worktree | Commit |
|---|---|---|---|
| 2026-05-25 | **Phase 6 COMPLETE** — postmortem capsules and chaos simulator harness shipped; `make chaos` is now the standing reproducibility gate | wt2 → main | 720906bf4..89fabc360, ca74cb4c2..6fb76f2b0 |
| 2026-05-25 | **Phase 4d-5 small projections COMPLETE** — contacts, onion announcements, and HODL history shadow projections with event payloads, boot wiring, diagnostics, and diff tools | wt2 → main | 0f10cd5f4..2f23d8352 |
| 2026-05-25 | **Snapshot halt recovery COMPLETE** — runtime recovery request path, local manifest builder, `tip_wedged_resnapshot` condition, verification gates, and recovery observability | wt3 → main | 4d7f7adee..8e25887b0 |
| 2026-05-25 | **Phase 3 chain_restore dissolve COMPLETE** — service implementation and compatibility header deleted; focused planner/executor/repair/boot modules own restore | main | 89892c441, 8658ef0d2 |
| 2026-05-25 | **Phase 3 header_probe dissolve COMPLETE** — PR-2 shrink to 392 LOC, legacy header RPC helper extracted, old `header_probe_service.{h,c}` deleted/renamed to `header_probe.{h,c}` | wt3 → main | 981ad4897, d17eb5ca0 |
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
