# Forward plan — finishing zclassic23 "the zclassic23 way"

Authoritative ordered plan for the remaining work. Supersedes the scattered
work-notes for *forward planning*; the live debt board stays
[`docs/REFACTOR_STATUS.md`](../REFACTOR_STATUS.md) and the canonical
architecture stays [`docs/FRAMEWORK.md`](../FRAMEWORK.md).

## Where we are (measured, current)

- `app/` is 100% shape-conformant: **266 `.c` files** across the 8 shape
  folders, **0 over the 800-line ceiling**, all lint baselines at 0.
- **34 lint gates** enforce it; `make lint` ends with "all checks passed".
  Two gates shipped in this drive: `check-file-size-ceiling` now covers
  `config/` (boot files frozen), and Gate #22 `check-framework-filename-suffix`
  locks filenames to their folder shape.
- The **only** architecture/size debt is 3 `config/` boot files —
  `boot.c` 3618, `boot_services.c` 3517, `boot_index.c` 1539 — now frozen
  by the size gate (can only shrink).
- The **real finish bar** is runtime: a fresh boot FATAL-halts at the §3
  coins-integrity gate, and the live node holds at ~3.13M without catching
  the network tip. That work is owner-gated and validated on a datadir COPY
  only — never in-place.

Every step gates on **build + `make lint` + `make test_parallel` (0/355)**;
boot moves additionally gate on a datadir-copy boot proof
(`tools/repro_on_copy.sh`). Architecture work is unblocked and sequenced
first; the boot decomposition is sequenced AFTER §3 clears (no boot seam can
be copy-validated while a fresh boot halts).

---

## Phase 0 — Docs + comment cleanup (unblocked)

- [x] **Purge finished work-note:** delete `docs/work/block-map-phashblock-uaf.md`
      (UAF fixed `56656d9d6`; invariant preserved in the regression test +
      memory; zero backlinks).
- [x] **Strip stale cutover/extraction comments** (comment-only, no behavior
      change): the `B7`/"the flip" narration in `coins_view_projection.{h,c}`
      reworded present-tense AS-LIVE; the three deleted-tool ledgers in
      `test_mcp_controllers.c` removed (the `EXPECTED_*` counts kept); the
      Wave-D/"extracted verbatim" provenance lines dropped from
      `boot_tip_hooks.c`, `boot_projections.c`, `tip_finalize_log_store.c`,
      `msgprocessor_snapshot.c`; the bare `post-B7` prefix dropped from
      `legacy_mirror_sync_service.c` and `chain_supervisor.c` (behavior
      comments kept).
- [x] **Kept by design** (adversarially verified — NOT dev-history cruft):
      `docs/work/safe-unwedge-design.md` (design-of-record for shipped
      `stage_reconcile_clamp_tip_finalize_to_floor`, cited by `fast-path.md`),
      `service-state-machine.md`, `FINISH_CHECKLIST.md`. Never strip
      `// obs-ok:` / `// platform-ok:` / `// suffix-ok:` / `// supervisor-root-ok:`
      markers or incident-dated safety rationale.
- [ ] **Refresh stale numbers/refs** (content edits, re-measure at edit time):
      - `DEFENSIVE_CODING.md` intro "33 lint gates" → 34.
      - `docs/REFACTOR_STATUS.md` Rank-1 row boot sizes → current `wc -l`.
      - `docs/SYNC.md` `chain_evidence_store` → `chain_evidence_persistence_service`.
      - `docs/PROJECT_OVERVIEW.md` stale tracked-file / boot-LOC counts.
      - `LEGACY_LIFECYCLE.md` drop deleted `zcl_diff_with_legacy`; fix the
        broken `[body-pull pathology](MEMORY.md)` link.
      - `docs/USER_BENCHMARKS.md` / `BENCHMARKS_LOG.md` refresh latest pointers.
      - `docs/HANDOFF.md` reword the stale "docs/work holds only the protocol".
- [ ] **Future label-scrub:** the same `B7`-era label class may linger
      elsewhere; sweep on the next pass (not a sample-of-60 finding).

---

## Phase 1 — Architecture-conformance finish (unblocked)

Each rename/move must also update the scaffold-label `files[]` guard + the
layering ASSERT string refs in `lib/test/src/test_make_lint_gates.c`.

- [ ] **A1 — move read-only wallet diagnostics to the View shape.** Move
      `app/controllers/src/wallet_diagnostic_health.c` (584) +
      `wallet_diagnostic_keys.c` (574) + their internal header to
      `app/views/` (joins the `wallet_view_*` family). Read-only render code;
      RPC registration stays in `wallet_diagnostic_controller.c`. Lowest risk.
- [ ] **A3 — split `app/jobs/src/utxo_apply_delta.c` (AT the 800 ceiling).**
      Time-sensitive (cannot absorb any future edit). Clean pure-extraction:
      the reorg-unwind cluster → `utxo_apply_delta_reorg.c` (move the 6 reorg
      fns **plus** `blob_get_u32`/`blob_get_i64`/`wall_now_s` + dup
      `#define STAGE_NAME`, per the verifier's amended move-list).
- [ ] **A2 — split `app/controllers/src/sync_controller_catchup.c` (775).**
      Clean: 11 header-declared job fns + the 2 file-local thread statics +
      `struct catchup_args` + `node_db_sync_catchup_thread` →
      `sync_controller_catchup_jobs.c`. Has headroom; lower urgency than A3.
- [ ] **Rejected this wave (re-scope as non-pure-extraction later):** splits
      of `script_validate_stage`, `wallet_tx`, `chain_evidence_authority_service`,
      `explorer_pages_view`, `chain_state_service`, `sync_controller_import` —
      each drags shared statics across the boundary (verifier-confirmed).

---

## Phase 2 — §3 coins-wedge keystone (OWNER-GATED, validate-on-copy only)

The real finish bar; it gates the boot decomposition. Green unit tests are
NOT a finish proof — forward progress on the running node is. Plan +
adversarial vetting: [`coins-commitment-persist-plan.md`](./coins-commitment-persist-plan.md).

- [ ] Persist height-stamped `utxo_sha3` at a cadence-gated boundary
      immediately after `coins_view_sqlite_batch_write_ex()` returns true in
      `flush_coins_if_needed()` (`lib/validation/src/process_block_flush_policy.c`).
- [ ] Adversary fix 1: never SHA3-scan on the IBD hot path — gate on
      caught-up-to-tip + Nth-flush + 1800s wall-clock.
- [ ] Adversary fix 2: resolve-height + `rows_above==0` + SHA3 scan in one
      read txn holding `cvs->mutex`.
- [ ] Adversary fix 3: bind to block IDENTITY — store the 32-byte
      `coins_best_block` hash (extend `utxo_sha3` to 76 bytes, back-compat on
      len), re-point the anchor to the STORED hash. Test via the
      `ZCL_TESTING` hook `process_block_test_persist_commitment_once`.

---

## Phase 3 — Live tip climb (OWNER-GATED, validate-on-copy only)

The node boots clean but holds at ~3.13M. A cluster, not one fix. Validate
on `repro_on_copy` checking tip_finalize HEALTH (reorg_detected flat,
finalized_total climbing) — not merely "tip didn't drop".

- [ ] Wire `active_chain_extend_window_have_data` (`chainstate.c:418`,
      unit-tested, UNWIRED) into the 8 reducer stages; pass utxo_apply's
      cursor as `max_height`; tip_finalize reads the contiguous finalized
      frontier. Reconcile anchored catch-up with legitimate higher-work
      reorg exposure (two naive in-place attempts were reverted).
- [ ] Root-cause the ~54 `script_validate` `internal_error`s above block
      3132742; restore zclassicd / zcl23 peer reachability to clear
      `peer_floor_violated`.

---

## Phase 4 — Boot decomposition (gated on §3 clearing + datadir-copy proof)

The only remaining size debt; pure verbatim extraction is EXHAUSTED. Seam
design: [`boot-decomposition-seams.md`](./boot-decomposition-seams.md).

- [ ] **C1 step 1 — extract `config/src/boot_msg_callbacks.c`** (~25 P2P
      callbacks, ~380 LOC) via one wiring seam `boot_wire_msg_callbacks(svc)`,
      EXCLUDING the `g_mmb_leaf_store` cluster. Mechanical blockers: export
      the 2 statics referenced by stayers, keep the 3 MMB registrations in
      `boot_services.c`, repoint the boot string assertions in
      `test_make_lint_gates.c`. Boot-validate on a datadir copy.
- [ ] **Keystone — redesign the shared `static struct boot_svc_ctx *S`**
      (`boot_services.c:153`) into a passed-in handle so worker/adapter
      bodies read `svc->*`. Unblocks service-kernel-adapters,
      background-workers, shutdown-phases.

---

## Phase 5 — Window-authority cleanup + closeout (P3, after Phase 3)

- [ ] Escalate `chain_evidence_controller_reconcile_startup` INFO →
      `LOG_ERR`/`EV_OPERATOR_NEEDED` once boot tip seeds from the finalized cursor.
- [ ] Collapse to ONE window authority: confirm the anchored extender's
      bounded scan (`MAX_GAP=8192`) still exposes legitimate higher-work
      reorgs on a 3M-entry map, THEN delete the generic
      `active_chain_extend_window` + `best_header` path.
