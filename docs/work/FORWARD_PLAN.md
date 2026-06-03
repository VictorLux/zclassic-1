# zclassic23 — V1 PLAN (MVP-anchored)

> **READ THIS FIRST.** This is THE finishing plan. The v1 bar is the 8
> acceptance criteria in **[`docs/MVP.md`](../MVP.md)** (v1 = MRS 8/8).
> Everything below is sequenced to move MRS toward 8/8.
>
> The framework/architecture refactor ([`docs/REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
> [`docs/FRAMEWORK.md`](../FRAMEWORK.md), boot decomposition) is **~90% done and
> OFF the v1 path. Do NOT jump the queue into it.** It is reference, not the
> mission.

---

## ⛔ #1 PRIORITY — the live wedge

The node **holds at tip but does not finalize forward.** `tip_finalize` shows
`reorg_detected_total` climbing while `finalized_total=0` (it oscillates,
never commits a new finalized frontier); the boot self-heal
`tip_stall_oracle_rebuild` is **exhausted**. **No v1 criterion that needs live
forward progress — C3 real cold-sync, C6 soak, C8 parity — can pass until the
tip finalizes again.** This is the single most valuable thing to fix.

**Method (never skip):** diagnose on a datadir **COPY**, never live. Run
`tools/diagnose_gap.sh`, then follow [`fast-path.md`](./fast-path.md)
(diagnose → design + adversarial critique → reset-safe test → repro-on-copy →
commit). NEVER delete `tip_finalize_log` rows; NEVER ship a consensus-adjacent
fix without a copy proof (see [`import-reset-and-write-ordering-assessment.md`](./import-reset-and-write-ordering-assessment.md)
— the 47279 reset mistake).

**Leading fix (autonomous — bucket A):** swap the window extender used by the
8 reducer stages from the unsafe most-work extender to the bounded have-data
one:
- `app/jobs/include/jobs/stage_helpers.h` `reducer_extend_window_to_candidate`
  → call `active_chain_extend_window_have_data`
  (`lib/validation/src/chainstate.c:418-485`, already unit-tested, unwired),
  bounded by `utxo_apply_stage_cursor() - 1`.
- Add the regression assertion in `lib/test/src/test_active_chain_extend.c`
  that the wrapper never overwrites a finalized slot with a header-only
  successor. A prior naïve pre-extend wiring was reverted (`481c520b9`) for
  churning `tip_finalize` — **this is HIGH risk; copy-prove before any deploy.**

**Companion fix (owner-gated — bucket B):** the coins-commitment-persist
keystone makes the boot self-heal auto-fire — design-of-record
[`coins-commitment-persist-plan.md`](./coins-commitment-persist-plan.md).
Recovery FSM design: [`service-state-machine.md`](./service-state-machine.md).

---

## Honest MRS scoreboard (supersedes any stale ✅ in MVP.md)

**Actually met (manual only): ~2 / 8. CI-verified: 0 / 8.** No criterion's
acceptance test runs under `make ci` — every one gates on `ZCL_STRESS_TESTS=1`,
which `make ci` (`Makefile:1110`) and `.github/` never set; C1/C6/C8 have no CI
test at all.

| # | Criterion | Honest status | CI? | Note |
|---|-----------|--------------|-----|------|
| 1 | Single-binary install (clean Ubuntu) | met-manual | no | no clean-container install + `systemctl` CI job |
| 2 | Tor onion bootstrap <60s | met-manual | no | onion live, but <60s timing not measured; test SKIPs in CI |
| 3 | Cold-start sync to tip <10 min | partial | no | FSM-only test; real sync unproven; node wedged |
| 4 | Receive shielded payment e2e | partial | no | gate exists but opt-in + needs `~/.zcash-params` |
| 5 | List + sell file via store | partial | no | gate exists, opt-in, not in CI |
| 6 | 7-day soak, zero intervention | **regressing** | no | no soak harness; node wedged — the opposite of soak |
| 7 | Recover from kill -9 <2 min | met-manual | no | SQLite-atomicity slice; test SKIPs in CI |
| 8 | Consensus parity w/ zclassicd | unmet | no | **no continuous diff service exists** — net-new build |

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **make the node hold tip → make v1 measurable in CI → prove
features → soak.** Refactor debt does not block a working sovereign node and
must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [ ] **Fix the wedge** — wire the have-data window extender (see #1 above);
      copy-prove that `reorg_detected_total` stops climbing and the finalized
      tip advances monotonically past the held height. ~55 LOC, HIGH risk.
- [ ] **Make criterion tests real CI gates** — add a CI step that runs the
      stress tests with `ZCL_STRESS_TESTS=1` for #2/#3/#4/#5/#7 (Makefile
      already has `test-shielded-payment` / `test-store-e2e` targets). Until
      this lands, MVP.md must not call any criterion CI-enforced.
- [ ] **Scope + build the consensus-parity-diff service (C8)** — net-new; none
      exists in `app/` or `lib/` (only in-process `test_reorg_parity.c` /
      `test_projection_replay_invariant.c`). A standing service that
      block-by-block diffs `zcl_utxocommitment` against a reference and emits
      `EV_OPERATOR_NEEDED` on mismatch. Develop/unit-test autonomously; running
      it needs the oracle up (bucket C). Ports: zcl23 RPC 18232, zclassicd RPC
      8232 / P2P 127.0.0.1:8034.
- [ ] **Cleanup** — comment STRIP/REWORD pass + doc-pointer fixes; gate with
      `make lint && make test_parallel`.

### B. OWNER-GATED (consensus-critical; explicit owner go + repro-on-copy)
- [ ] **Coins-commitment-persist keystone** — write the 76-byte anchored
      `utxo_sha3` record inside `coins_view_sqlite_batch_write_ex`'s txn
      (`lib/storage/src/coins_view_sqlite.c`), table-derived height/count,
      + `_save_anchored`/`_load_anchor` in `lib/coins/src/utxo_commitment.{c,h}`,
      + re-validating heal in `coins_reconcile_stale_anchor`. Design-of-record
      [`coins-commitment-persist-plan.md`](./coins-commitment-persist-plan.md)
      (adversary-vetted; original verdict DO_NOT_APPLY → corrected design at
      top). **Do NOT apply live without owner go.**
- [ ] Persist `utxo_sha3` at finalized-tip so the self-heal has a fresh input.
- [ ] Deploy the wedge fix (A) live only after a clean copy proof.
- [ ] After the wedge clears, apply the deferred consensus hazards in
      [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md)
      (item 1 = a real bg_validation lock-free `chain_active` UAF, same class
      as the fixed phashBlock bug).
- [ ] MVP feature e2e proofs once the tip holds: C4 (receive shielded) + C5
      (store sell) on a funded test wallet.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Stand up a second `zcl23` serving node** — C3 cold-sync (FlyClient +
      SHA3 snapshot) is unprovable end-to-end with 0 `zcl23` peers serving the
      snapshot protocol. No code fixes this; a second node must exist.
- [ ] **Restore peer health above the floor of 3** (deliberate policy — do not
      lower it) — supply working `-addnode=` peers with group diversity.
- [ ] **Fix the crash-looping `zclassicd-rhett` oracle** (RPC 8232 unreachable
      → C8 parity has no reference). Investigate its logs; per doctrine do NOT
      stop `zclassicd`.
- [ ] **Run the 7-day soak (C6)** once the tip finalizes: live node + synthetic
      load, RSS plateau, zero manual restarts — measure against
      [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md).
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip. Operator coverage: [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** A.wedge gates C6/C8 and the feature proofs. A.CI-wiring
gates honest measurement of everything. C8 needs the parity service built (A) +
the oracle up (C). C3 needs a second node (C). Boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
[`../FRAMEWORK.md`](../FRAMEWORK.md). The only remaining size debt is the three
`config/` boot files (`boot.c` 3618, `boot_services.c` 3517, `boot_index.c`
1539), frozen shrink-only by the size gate; seam plan in
[`boot-decomposition-seams.md`](./boot-decomposition-seams.md). Engineering
quality detail: [`FINISH_CHECKLIST.md`](./FINISH_CHECKLIST.md). Safe-execution
method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
