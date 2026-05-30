# B8 — Comparison-Apparatus Deletion Plan (verified)

> **Status:** research-only, dependency-verified deletion plan for the
> **comparison apparatus** — the shadow-vs-legacy parity machinery that becomes
> dead *once the authority flip is permanent*. Companion to
> [`b8-deletion-inventory.md`](./b8-deletion-inventory.md) (that doc covers the
> legacy *block-application* modules; **this doc covers the comparison/diff/
> cutover machinery**). Read [`docs/FRAMEWORK.md`](../FRAMEWORK.md) §2 (the
> "Today vs target" shadow caveat) and `REFACTOR_STATUS.md` line **B8** first.
> Every classification below was spot-checked by reading the call sites — not
> from filenames.
>
> **Re-verified 2026-05-29 against tree `832d30156`.** The architecture and
> classification below held; the line-number/count drift that had accumulated was
> corrected and one whole apparatus component that the prior revision missed was
> added: the **`header_admit_stage_diff` ("S-11 mini-diff") harness** and its MCP
> tool **`zcl_diff_staged_header_admit`** (§1b row + §0 below). All MCP tool counts
> were re-read from `test_mcp_controllers.c` (now **112 / 47 / 20**, not the prior
> 109 / 46 / 18 — three recovery tools landed in between).

## 0. CRITICAL ADDITION — `header_admit_stage_diff` is apparatus living inside a SURVIVOR job

The prior revision of this doc did not list the **S-11 mini-diff harness**. It is
the same *trap* shape as `cutover_modes` (a KEEP file carrying an apparatus
sub-surface), so it must be enumerated:

- **Apparatus symbol:** `header_admit_stage_diff(int32_t,int32_t,struct
  header_admit_diff_report*)` — def `app/jobs/src/header_admit_stage.c:426`
  (body ~`:410-560`, incl. `static diff_sample_record` `:410`); decl + the whole
  `enum header_admit_diff_status` / `struct header_admit_diff_{sample,report}`
  block live in `app/jobs/include/jobs/header_admit_stage.h:113-182`.
- **The host file `header_admit_stage.c` is a SURVIVOR Job** (the authoritative
  header-admit stage; it reads `cutover_modes_get_header_admit()` at `:193`). Only
  the `*_diff` harness inside it is comparison — delete the diff fn + the diff
  structs/enum from the header, keep the stage.
- **Callers of `header_admit_stage_diff`:** MCP `zcl_diff_staged_header_admit`
  (`tools/mcp/controllers/chain_controller.c:308`, handler `:296`, **route `:1057`**,
  domain **chain**) — *apparatus, retires*; `cutover_controller_preflight.c:509,515`
  — *apparatus, vanishes with preflight*; tests `test_cutover_flip_dryrun.c:547`,
  `test_header_admit_stage.c` (×9: `:573,601,650,660,710,758,798,831,866`).
- **`test_header_admit_stage.c` (884 LOC) is a SURVIVOR test** of the stage — do
  NOT delete it; **trim only its `header_admit_stage_diff` assertions** (same
  treatment as `test_small_projections.c`).
- **MCP count impact:** `zcl_diff_staged_header_admit` is counted in
  `EXPECTED_CHAIN` (`test_mcp_controllers.c:88`) and has dedicated dispatch
  coverage. Its removal makes Step 3's chain decrement **−4, not −3**.

## TL;DR — three corrections to the framing before anyone deletes

1. **The apparatus is `(b) gated-on-flip`, not `(a) dead-now`.** Authority is
   still in SHADOW (FRAMEWORK §2: "the live tip is still driven by the legacy
   `connect_tip` path with `coins.db` as the mutable source of truth"). Every
   diff/conservation/canary surface is load-bearing *as the proof that the flip
   is safe*. It dies only **after** authority has been on the log long enough to
   trust — i.e. after the flip is made permanent (compile-time default flips
   from SHADOW to AUTHORITATIVE and the revert path is retired). **Nothing in
   the apparatus is deletable today.**

2. **`cutover_modes` + `cutover_controller` (the `cutovermode` RPC) are KEEP, not
   apparatus.** `cutover_modes_set_*` is the **authority switch itself** — the
   stages (`header_admit_stage.c:185`, `validate_headers_stage.c:494`,
   `tip_finalize_stage.c:620`) read these modes to decide whether to be
   authoritative. This is the *destination*, the thing the whole refactor flips.
   The `cutovermode all authoritative` RPC is how the operator performs/reverts
   the flip; the revert is the FRAMEWORK's safety escape hatch. Only the
   *canary/conservation* sub-surface inside these files is comparison.

3. **Deleting the apparatus does NOT shrink the E6 `one_write_path` baseline.**
   The E6 baseline (`tools/scripts/one_write_path_baseline.txt`, 64 surfaces /
   66 lines incl. comments — the brief's "64" matches the surface count) lists
   the legacy **write** path (`coins_view_cache_flush`, `connect_tip`,
   `active_chain_set_tip`, `coins_view_sqlite_batch_write`) — **zero**
   comparison-apparatus entries. E6 shrinks when the legacy *writer* is deleted
   (a separate B8 sub-task), not when the diff machinery is. Likewise the E1
   file-size baseline lists **none** of these files (`projection_diff_controller.c`
   is 715 < 800; the MCP `chain_controller.c` at 1063 and
   `diagnostics_controller.c` at 767 are `tools/`, not `app/`, so outside the E1
   `app/**/*.c` gate). **The apparatus deletion is a clean LOC reduction with no
   lint-baseline ratchet; its gate impact is the MCP tool-count contract, not
   E1/E6.**

---

## 1. Verified inventory + classification

LOC via `wc -l`. "Class" ∈ **(a) dead-now**, **(b) gated-on-flip** (load-bearing
shadow proof until the flip is permanent), **(c) keep** (survives the flip — the
authoritative path or operator introspection needs it).

### 1a. Core diff/feeder operations

| Component | File(s) | LOC | Callers / registration | Class | Spot-check note |
|---|---|---|---|---|---|
| `diff_with_legacy_shadow` (pure op) | `application/operations/src/diff_with_legacy_shadow.c` (+`.h` 61) | 80 | MCP `h_zcl_diff_with_legacy_shadow` (`chain_controller.c:95,181`); `shadow_replay_proof.c`; tests | **(b)** | Pure byte-compare of primary vs shadow block-log ports (`diff_with_legacy_shadow.c:43-78`). Zero authoritative-path callers. |
| `shadow_replay_proof` (lib op) | `application/operations/src/shadow_replay_proof.c` (+`.h` 60) | 105 | `tools/shadow_replay_proof.c` CLI; `test_shadow_replay_full_driver.c` | **(b)** | "offline cutover proof skeleton" (`shadow_replay_proof.h:4`): replays primary→shadow then diffs. Pure proof harness. |
| `shadow_replay_proof` (CLI tool) | `tools/shadow_replay_proof.c` | 291 | standalone bin (not linked into node) | **(b)** | Offline proof driver; remove with the lib op. |
| `shadow_feeder` (adapter) | `adapters/inbound/src/shadow_feeder.c` (+`.h` 96) | 140 | `boot_services.c:199` (only when `-shadow`); `shadow_feeder_global.c` | **(b)** | `-shadow`-flag gated, OFF by default (`boot.h:54`, `main.c:1485`). Observes blocks → shadow log + mutator validate queue. Pure side-channel; the live tip never reads it. |
| `shadow_feeder_global` (shim) | `adapters/inbound/src/shadow_feeder_global.c` (+`.h` 54) | 34 | `lib/net/src/msg_blocks.c:410` (extern); `chain_controller.c:126` | **(b)** | Forward-extern shim so `lib/net` can call the feeder without a layer dep. `msg_blocks.c:410` calls it on every connect, but it is a no-op unless `-shadow` armed the global (`shadow_feeder_global_is_active()`). |
| `shadow_conservation` (counters) | `adapters/inbound/include/adapters/inbound/shadow_conservation.h` (impl is header-declared; counters live in the feeder path) | 99 (hdr) | `shadow_feeder.c:118,126` (`record_skipped`/`record_fed`); `cutover_controller_preflight.c` (the fed==diffed gate); `test_shadow_conservation.c` | **(b)** | The fed==diffed conservation law — the preflight gate that refuses a flip onto a pipeline silently dropping blocks. |

### 1b. Projection-diff RPCs + MCP tools

| Component | File(s) | LOC | Callers / registration | Class | Spot-check note |
|---|---|---|---|---|---|
| `projection_diff_controller` (8 RPCs) | `app/controllers/src/projection_diff_controller.c` | 715 | RPC table `diagnostics_controller.c:38-47`; decls `diagnostics_internal.h:70-86` | **(b)** except `getmirrorstatus` = **(c)** | Each `*projectiondiff` RPC folds a projection over `event_log` and compares to the legacy SQLite table (`projection_diff_controller.c:14-16`). `diag_rpc_getmirrorstatus` (`:43`) just dumps `legacy_mirror_sync_dump_state_json` → **KEEP** (mirror monitor survives per `b8-deletion-inventory.md` §7). |
| MCP `zcl_{peers,mempool,znam,wallet,contacts,onion_announcements,hodl_history}_projection_diff` (7 tools) | `tools/mcp/controllers/diagnostics_controller.c:112-169` (handlers), `:677-706` (routes) | (within 767) | route table (`:677-706`); proxy to the RPCs above | **(b)** | Thin `mcp_node_rpc("…projectiondiff")` proxies. Delete with the RPCs. **Decrements `EXPECTED_OPS` (these route under domain "ops") and `EXPECTED_TOTAL`.** |
| MCP `zcl_utxo_projection_diff` | `tools/mcp/controllers/chain_controller.c:720` (handler), `:1076` (route) | (within ~1090) | route table; reads `getutxocommitment` + projection | **(b)** | "24h cutover soak gate" (`chain_controller.c:704-709`). Domain "chain". Decrements `EXPECTED_CHAIN`+`EXPECTED_TOTAL`. |
| MCP `zcl_block_index_diff` | `tools/mcp/controllers/chain_controller.c:520` (handler), `:1085` (route) | (within ~1090) | route table; folds block_index_projection vs live | **(b)** | "Gates the 4c-cutover PR … 24h of match=true". Domain "chain". |
| MCP `zcl_diff_with_legacy_shadow` | `tools/mcp/controllers/chain_controller.c:95` (handler), `:1051` (route) | (within ~1090) | route table; calls the pure op + shadow_feeder_global | **(b)** | The live shadow-vs-legacy byte diff. Domain "chain". |
| MCP `zcl_diff_with_legacy` (composite verdict) | `tools/mcp/controllers/diagnostics_controller.c:252` (handler), `:715` (route) | (within 767) | route table | **(b)** | "composite are-we-tracking verdict" (`:17`) — reads `getmirrorstatus` + mirror_status. Domain "ops". NB: reads the KEEP mirror status, but is itself a comparison verdict → delete. |
| projection-side diff helpers | folded inline in `lib/storage/src/{mempool,peers,znam,contacts,onion_announcements}_projection.c` + `small_projections.h` | — | only the `*projectiondiff` RPCs | **(c) the projections / (b) the diff entry points** | The **projections themselves are KEEP** (they are the post-flip read-models, FRAMEWORK §2 fold targets). Only the *count/first_diff comparison* helpers invoked by the RPCs are apparatus. There is no separate `_projection_diff()` symbol to delete — the comparison lives in the controller; the projection just exposes `count`/`catch_up`. **Do NOT delete projection files.** |

### 1c. Cutover mode / preflight / canary plumbing

| Component | File(s) | LOC | Callers / registration | Class | Spot-check note |
|---|---|---|---|---|---|
| `cutover_modes` (mode switch) | `app/services/src/cutover_modes.c` (+`.h` 71) | 258 | stages (`header_admit_stage.c:185`, `validate_headers_stage.c:494`, `tip_finalize_stage.c:620`); `cutover_controller.c`; conditions; `diagnostics_registry.c:454` (`zcl_state=cutover`) | **(c) the mode get/set; (b) the canary sub-API** | `cutover_modes_set_*`/`get_*` **IS the authority switch** the stages read — KEEP. The canary half (`record_change`, `canary_snapshot`, `canary_target_reached`, `clear_canary`, `struct cutover_canary_snapshot`) is flip-window comparison → **(b)**. `cutover_dump_state_json` (`zcl_state=cutover`) KEEP but shrinks. |
| `cutover_controller` (`cutovermode` RPC) | `app/controllers/src/cutover_controller.c` | 273 | RPC `diagnostics_controller.c:32`; MCP `zcl_cutovermode` (handler `ops_controller.c:294`, route `:954`) | **(c) the flip/revert RPC; (b) the canary push** | `diag_rpc_cutovermode` sets/reverts authority and is the operator's flip + safety-revert control — KEEP. `cutover_push_canary_state` + `push_cutover_modes`' canary fields are comparison → **(b)**. |
| `cutover_controller_preflight` (`cutoverpreflight` RPC) | `app/controllers/src/cutover_controller_preflight.c` | 718 | RPC `diagnostics_controller.c:33`; MCP `zcl_cutoverpreflight` (handler `ops_controller.c:323`, route `:963`); called by `cutover_controller.c:192` (the flip's own ready-gate); itself calls `header_admit_stage_diff` (`:509,515`) | **(b)** | Read-only "ready" snapshot ANDing every safety gate incl. the fed==diffed conservation law (`cutover_controller_preflight.c:14-18`). **TRAP:** `cutover_controller.c:192` (`cutover_preflight_ready_now`) calls it to *refuse an unsafe flip* — it cannot be deleted while the flip RPC still gates on it. It dies only when the flip is permanent (no more runtime flips to gate). |
| `cutover_no_forward_progress` (Condition) | `app/conditions/src/cutover_no_forward_progress.c` | 142 | `condition_registry.c:23,48` | **(b) — the auto-revert safety net** | Detects authoritative stages that stopped advancing the tip and **reverts to shadow** (`:75,134`). This is the live auto-rollback during the flip window. **Most dangerous to delete early** (see §4). |
| `cutover_canary_complete` (Condition) | `app/conditions/src/cutover_canary_complete.c` | 108 | `condition_registry.c:24,49` | **(b)** | Watches the canary block; on completion calls `cutover_modes_revert_all_to_shadow` (`:45`) / promotes. Flip-window only. |
| MCP `zcl_cutovermode` | `tools/mcp/controllers/ops_controller.c:294` (handler), `:954` (route) | (within file) | route table | **(c)** | Proxy to the KEEP flip RPC. Survives until flip permanent. |
| MCP `zcl_cutoverpreflight` | `tools/mcp/controllers/ops_controller.c:323` (handler), `:963` (route) | (within file) | route table | **(b)** | Proxy to preflight. Dies with preflight. |
| MCP `zcl_diff_staged_header_admit` (S-11) | `tools/mcp/controllers/chain_controller.c:296` (handler), `:1057` (route) | (within file) | route table; calls survivor `header_admit_stage_diff` | **(b)** | The S-11 mini-diff over staged header_admit vs legacy block_index. Domain **chain**. Decrements `EXPECTED_CHAIN`+`EXPECTED_TOTAL`. See §0. |

### 1d. The cutover *proof* test corpus (gated-on-flip; delete last)

LOC re-read 2026-05-29. All registered in the `test_parallel.c` X(...) list
(`:86,134-140`) and `test.c`.

| Test file | LOC | X-list entry | Class | Note |
|---|---|---|---|---|
| `test_diff_with_legacy_shadow.c` | 215 | `X(diff_with_legacy_shadow)` | **(b)** | Pins the pure diff op. |
| `test_shadow_feeder.c` | 225 | `X(shadow_feeder)` | **(b)** | |
| `test_shadow_feeder_global.c` | 131 | `X(shadow_feeder_global)` | **(b)** | |
| `test_shadow_conservation.c` | 347 | `X(shadow_conservation)` | **(b)** | fed==diffed law. |
| `test_shadow_replay_full_driver.c` | 206 | `X(shadow_replay_full_driver)` | **(b)** | |
| `test_shadow_replay_proof.c` | 147 | `X(shadow_replay_proof)` | **(b)** | Pins the lib op + CLI proof skeleton. (Was omitted by the prior revision.) |
| `test_cutover_flip_dryrun.c` | 725 | `X(cutover_flip_dryrun)` | **(b)** | Flip dry-run safety proof. Also exercises `header_admit_stage_diff` (`:547`). |
| `test_cutover_tip_parity.c` | 473 | `X(cutover_tip_parity)` | **(b)** | Tip-parity proof. |
| `test_cutover_postflip_reorg.c` | 880 | `X(cutover_postflip_reorg)` | **(b)** | Post-flip reorg parity. |
| `test_cutover_autorevert.c` | 638 | `X(cutover_autorevert)` | **(b)** | Pins the `cutover_no_forward_progress` auto-revert net. (Was omitted by the prior revision.) |
| `test_cutover_preflight.c` | 293 | `X(cutover_preflight)` | **(b)** | Pins the preflight ready-gate. (Was omitted by the prior revision.) |
| `test_reorg_parity.c` | 939 | `X(reorg_parity)` | **(b)** | Reorg parity (shadow vs authoritative). |
| `test_small_projections.c` | 683 | `test.c` | **(c) keep** | Tests the projection folds themselves (KEEP read-models), not just the diff. Keep; trim only the `*_projection_diff` assertions. |
| `test_header_admit_stage.c` | 884 | `X(header_admit_stage)` | **(c) keep** | Survivor stage test. Keep; trim only its 9 `header_admit_stage_diff` assertions (`:573,601,650,660,710,758,798,831,866`). See §0. |

**Apparatus LOC (classes a/b, deletable when flip is permanent), excluding KEEP:**
core diff/feeder ~750 (incl. headers + CLI tool) + projection_diff_controller ~700
(less the small `getmirrorstatus`) + preflight 718 + 2 cutover Conditions 250 +
canary sub-surface of cutover_modes/controller ~150 + `header_admit_stage_diff`
harness ~150 (in the survivor stage + header) + MCP route/handler stubs ~250 +
proof test corpus ~4,650 (re-tallied: the 6 originally-listed shadow/diff/cutover
tests + the 3 prior-revision omissions `test_shadow_replay_proof`/
`test_cutover_autorevert`/`test_cutover_preflight` + `test_reorg_parity` now 939;
plus trims to the 2 KEEP tests). **≈ 7,600 LOC** total once the flip is permanent
(~2,950 production + ~4,650 tests). None of it deletable *now*.

---

## 2. Ordered deletion sequence (executes only AFTER the flip is made permanent)

> **ORDERING IS A HARD CONSTRAINT, NOT A PREFERENCE.** The apparatus is the tooling
> that *performs* the live flip (cutovermode + cutoverpreflight) and *verifies* it
> during soak (the diff tools + conservation law + auto-revert Condition). The
> mandatory order is: **flip → soak → confirm → THEN delete.** Deleting any of it
> before the flip is permanent removes the proof or the safety net. **§3 (Gate-0)
> below lists the exact conditions that must ALL hold before Step 1 may run.** Until
> they hold, every step here is a no-op — **stop**.

Gate-0 precondition for the whole sequence (see §3 for the hard checklist): the
compile-time stage-mode default has flipped from SHADOW to AUTHORITATIVE, the
runtime revert path is retired, and the live tip has been on the log-derived
pipeline long enough to trust. Until then, **stop** — the apparatus is the proof,
not cruft.

Each step is independently `make lint`-clean and `./test_parallel`-green. The
only gate that moves is the **MCP tool-count contract** in
`test_mcp_controllers.c` (`EXPECTED_TOTAL`/`EXPECTED_OPS`/`EXPECTED_CHAIN` —
**currently 112 / 47 / 20** as of `832d30156`); E1 and E6 are **untouched** by
this sequence.

### Gate-0 — the HARD checklist (ALL must be TRUE before Step 1)

Deletion may begin only when **every** one of these holds. If any is false, the
apparatus is still load-bearing — **stop and report, do not delete.**

1. **Flip is PERMANENT, not runtime.** The compile-time stage-mode default in the
   three stages (`header_admit_stage.c:185`, `validate_headers_stage.c:494`,
   `tip_finalize_stage.c:620`) is AUTHORITATIVE, and the runtime revert path
   (`cutover_modes_revert_all_to_shadow`, `cutover_modes.c:67`) is no longer
   reachable from any live caller. (Until the default is AUTHORITATIVE, the stages
   read SHADOW and the apparatus is the only proof the flip is right.)
2. **Live tip has SOAKED on the log-derived pipeline for N blocks** with zero
   divergence: a sustained run where every authoritative stage advanced the real
   tip and `zcl_diff_with_legacy_shadow` / `zcl_diff_staged_header_admit` /
   `zcl_utxo_projection_diff` / `zcl_block_index_diff` all reported match. (The
   diff tools EXIST precisely to produce this evidence — they cannot be deleted
   until they have produced it.) Suggested floor: ≥ several hundred blocks of
   continuous AUTHORITATIVE advance with the node never falling back to mirror
   block-apply.
3. **`cutoverpreflight` is GREEN at the soak tip** — every gate in
   `cutover_controller_preflight.c` ANDs true: guard, conservation (fed==diffed),
   utxo_commitment, modes, `header_admit_diff` converged. A green preflight is the
   single-call attestation that the pipeline is safe to make permanent.
4. **The fed==diffed conservation law has held** across the whole soak (the
   `shadow_conservation` counters show no silently-dropped blocks).
5. **The auto-revert Condition NEVER FIRED during the soak.**
   `cutover_no_forward_progress.c` must show zero reverts-to-shadow for the entire
   soak window. (One fire = the flip was not safe; the apparatus saved the chain
   and deletion is OFF the table until the underlying cause is fixed and a fresh
   soak passes.)
6. **`make lint` + `./test_parallel` green** on the AUTHORITATIVE-default build
   *before* any apparatus deletion, so the baseline is known-good.

**Why this exact order (flip → soak → confirm → delete):** the apparatus performs
the flip (cutovermode), refuses an unsafe flip (cutoverpreflight, called by
`cutover_controller.c:192`), verifies parity during soak (the diff tools +
conservation), and auto-reverts a bad flip (`cutover_no_forward_progress`). Delete
any of it earlier and you remove either the mechanism that does the flip, the proof
it worked, or the net that catches it failing. There is no safe reordering.

**Step 1 — retire the runtime flip surface (unblocks deleting modes/canary).**
- Delete the `cutovermode`/`cutoverpreflight` RPCs (`cutover_controller.c`,
  `cutover_controller_preflight.c`), their decls (`diagnostics_internal.h:43,47`)
  and rows in `diagnostics_controller.c:32,33`.
- Delete MCP `zcl_cutovermode` (handler `ops_controller.c:294`, route `:954`) +
  `zcl_cutoverpreflight` (handler `:323`, route `:963`), plus their param tables.
  **`EXPECTED_OPS −2`, `EXPECTED_TOTAL −2`** + drop the count-comment lines
  (`test_mcp_controllers.c:63,64,85`) **and** the two dedicated dispatch tests
  (`test_zcl_cutovermode_shape_and_dispatch` `:486`,
  `test_zcl_cutoverpreflight_shape_and_dispatch` `:547`, called at `:1692,1693`),
  the `mock_cutovermode_rpc` hook (`:467`), and the name-list mentions (`:358,1162`).
- Collapse the stage mode-getters to a compile-time constant AUTHORITATIVE, then
  delete the canary half of `cutover_modes.c` (`record_change`,
  `canary_snapshot`, `canary_target_reached`, `clear_canary`,
  `struct cutover_canary_snapshot`) and `cutover_controller.c`'s
  `cutover_push_canary_state`. Keep `cutover_dump_state_json` (shrunk) so
  `zcl_state=cutover` (`diagnostics_registry.c:454`) keeps resolving — or drop
  the `cutover` dumper row and its enum mention if the subsystem is retired.
- **Unblocks:** the two cutover Conditions (Step 2) and the preflight (no caller).
- Tests: migrate `test_cutover_flip_dryrun.c`, `test_cutover_tip_parity.c`
  (these exercise the flip + preflight). `test_rpc.c`/`test_watchdog_conditions_pr3.c`
  reference cutover — repoint/trim.

**Step 2 — delete the flip-window Conditions (now no flip to guard).**
- Delete `cutover_no_forward_progress.c` + `cutover_canary_complete.c`; drop their
  `register_*` decls (`condition_registry.c:23,24`) and calls (`:49,50`) and
  `watchdog_dissolve_pr3.h` decls. Delete `test_cutover_autorevert.c` (pins
  `cutover_no_forward_progress`); migrate/drop `test_condition_engine.c`,
  `test_watchdog_conditions_pr3.c` cutover cases.
- **Unblocks:** nothing depends on these; safe once Step 1's RPC revert is gone.

**Step 3 — delete the diff RPCs + MCP tools (the daily `*_projection_diff` surface).**
- Delete the 7 `*projectiondiff` RPCs in `projection_diff_controller.c` (KEEP
  `getmirrorstatus` — move it to a surviving controller or keep the file as a
  one-RPC stub), `zcl_diff_with_legacy` (`diagnostics_controller.c:252,715`), and
  the 7 `zcl_*_projection_diff` MCP routes (`diagnostics_controller.c:112-169,
  677-706`).
- Delete `zcl_utxo_projection_diff` (handler `chain_controller.c:720`, route `:1076`),
  `zcl_block_index_diff` (handler `:520`, route `:1085`),
  `zcl_diff_with_legacy_shadow` (handler `:95`, route `:1051`), **and
  `zcl_diff_staged_header_admit` (handler `:296`, route `:1057`)** from
  `chain_controller.c`. Also delete the survivor-internal harness it calls:
  `header_admit_stage_diff` (`header_admit_stage.c:426`, body ~`:410-560`) + the
  `enum header_admit_diff_status`/`struct header_admit_diff_*` block in
  `header_admit_stage.h:113-182` — **keep the rest of the stage** (§0).
- **MCP counts:** `EXPECTED_OPS −` (mempool/znam/wallet/peers/contacts/onion/hodl
  projection diffs + `diff_with_legacy`, all route "ops"), **`EXPECTED_CHAIN −4`**
  (`utxo_projection_diff` + `block_index_diff` + `diff_with_legacy_shadow` +
  **`diff_staged_header_admit`** — the prior revision said −3, missing the last),
  `EXPECTED_TOTAL −(all of the above)`. Update every count-comment line
  (`test_mcp_controllers.c:53,54,56-60,80-82,87-90`) in the same commit.
- **Do NOT touch the projection files in `lib/storage`** — they are KEEP folds.
  **Do NOT delete `header_admit_stage.c` / `.h`** — survivor; trim only the diff
  harness. Trim the 9 `header_admit_stage_diff` assertions in the KEEP test
  `test_header_admit_stage.c` (`:573,601,650,660,710,758,798,831,866`).

(Recommended first 3 steps = Steps 1–3 above.)

**Step 4 — delete the pure ops + feeder + conservation (no MCP caller left).**
- Delete `application/operations/{src,include}/diff_with_legacy_shadow.*`,
  `shadow_replay_proof.*`, `tools/shadow_replay_proof.c`.
- Delete `adapters/inbound/src/shadow_feeder.c`, `shadow_feeder_global.c`,
  **`adapters/inbound/src/shadow_conservation.c` (70 LOC)**, their headers, and
  `shadow_conservation.h`. Remove the `-shadow` flag
  (`main.c:1485`, `boot.h:54`), `boot_services.c:156-230,2949,3167,3180` feeder
  lifecycle, and the `msg_blocks.c:266-410` extern + observe call.
- **Conservation-API orphan trim (do in this same commit, glob-build link-safety):**
  deleting `shadow_conservation.*` orphans three live references in KEEP files —
  (1) `app/services/src/cutover_modes.c:228` (`shadow_conservation_ok` inside the
  KEEP `cutover_dump_state_json`) — **must be removed by Step 1's canary trim FIRST**
  (Step 4 depends on it); (2) `lib/test/src/test_rpc.c:333,477-479,524`
  (`shadow_conservation_reset/record_fed/record_diffed` + the `#include` at `:9`) —
  trim in lockstep with this delete or `test_rpc.c` (a KEEP test) won't link.

**Step 5 — delete the proof test corpus.**
- Remove `test_{diff_with_legacy_shadow,shadow_feeder,shadow_feeder_global,
  shadow_conservation,shadow_replay_full_driver,shadow_replay_proof,
  cutover_tip_parity,cutover_flip_dryrun,cutover_postflip_reorg,cutover_preflight,
  reorg_parity}.c` (and `test_cutover_autorevert.c` if not already removed in
  Step 2), their decls (`test_helpers.h`) and registrations (`test.c`,
  `test_parallel.c:134-140` X-list — drop `X(shadow_feeder)`,
  `X(shadow_feeder_global)`, `X(shadow_conservation)`, `X(diff_with_legacy_shadow)`,
  `X(shadow_replay_proof)`, `X(shadow_replay_full_driver)`, `X(cutover_tip_parity)`,
  `X(cutover_flip_dryrun)`, `X(cutover_postflip_reorg)`, `X(cutover_autorevert)`,
  `X(cutover_preflight)`, plus `X(reorg_parity)` at `:86`).
- **KEEP** `test_small_projections.c` (trim its `*_projection_diff` assertions only)
  and `test_header_admit_stage.c` (trim only its `header_admit_stage_diff`
  assertions — the stage itself survives).

**Build note:** `app/`, `application/`, `adapters/` are glob-built (no explicit
source list) — file deletion is self-effecting. The MCP tool-count contract and
the `test_parallel.c:86,134-140` X(...) macro list are the only manual non-baseline
edits.

---

## 3. Net effect on lint gates (verified)

| Gate | Effect of this deletion | Why |
|---|---|---|
| **E6 `one-write-path`** (64 surfaces) | **NONE** | Baseline lists only the legacy *writer* (`coins_view`/`connect_tip`/`active_chain_set_tip`); zero apparatus entries. E6 shrinks when the legacy writer dies, a different B8 sub-task. |
| **E1 `file-size-ceiling`** (7→ baseline) | **NONE** | No apparatus file is in `file_size_ceiling_baseline.txt`. `projection_diff_controller.c`=715<800; the MCP files are `tools/`, outside the `app/**` gate. |
| **MCP tool-count contract** (`test_mcp_controllers.c`) | **MUST shrink in lockstep** | `EXPECTED_TOTAL 112` (`:49`), `EXPECTED_OPS 47` (`:66`), `EXPECTED_CHAIN 20` (`:86`) are hard asserts (re-read 2026-05-29; the prior revision's 109/46/18 was stale — three recovery tools landed since). Each deleted tool decrements the matching count + the human-readable comment in the same commit, or the suite fails. |
| **E2 `one-result-type`** | **1 entry: `cutover_modes.c`** | `one_result_type_baseline.txt` lists `app/services/src/cutover_modes.c`. But `cutover_modes.c` is **(c) KEEP** (the authority switch) — only its canary sub-API is trimmed, the file survives, so its E2 line **stays**. The pure ops (`diff_with_legacy_shadow.c`, `shadow_replay_proof.c`, `shadow_feeder.c`) are NOT in the E2 baseline (they use `zcl_result` correctly), so deleting them is E2-neutral. |

---

## 4. Traps (most dangerous first)

1. **`cutover_no_forward_progress.c` is the live auto-revert safety net, not a
   diff.** It watches the authoritative stages and, if the tip stops advancing,
   **reverts authority to shadow** (`:75,134`). Deleting it (or the
   `cutover_modes_set_header_pipeline(SHADOW,…)` revert it calls) **before the
   flip is permanent and proven** removes the one mechanism that backs out a bad
   flip automatically — exactly the resilience-first-class failure mode that
   wedged the live chain on 2026-05-24 (MEMORY). It is `(b)`, the **last** thing
   deleted, only after the SHADOW default is gone.

2. **`cutoverpreflight` is the flip RPC's own refusal gate.**
   `cutover_controller.c:192` (`cutover_preflight_ready_now`) calls
   `diag_rpc_cutoverpreflight` to refuse an unsafe flip. Delete the preflight
   while the `cutovermode` flip RPC still exists and the flip silently loses its
   safety check (could flip onto a pipeline that dropped blocks). Order matters:
   retire the runtime flip RPC (Step 1) *before* the preflight, never the
   reverse.

3. **`cutover_modes` is the authority switch, mis-classifiable as "cutover
   plumbing."** The stages read `cutover_modes_get_*` to decide authoritativeness
   (`header_admit_stage.c:193`, `validate_headers_stage.c:502`,
   `tip_finalize_stage.c:628`). Deleting `cutover_modes.c` wholesale as
   "comparison apparatus" would silently drop the live stages back to shadow
   reads. Only the **canary sub-API** is apparatus; the mode get/set is `(c) keep`.

4. **`getmirrorstatus` rides inside `projection_diff_controller.c`** (`:43`) but
   is `(c) keep` — it dumps the surviving `legacy_mirror` monitor
   (`legacy_mirror_sync_dump_state_json` at `:55`) and backs MCP `zcl_mirror_status`
   (handler `ops_controller.c:274`, route `:891`) + the `zcl_diff_with_legacy`
   composite. Deleting the file wholesale 404s the mirror-status tool. Extract
   `getmirrorstatus` before deleting the file.

5. **The MCP tool-count asserts are silent landmines.** `EXPECTED_TOTAL/OPS/
   CHAIN` (`test_mcp_controllers.c:49,66,86`) fail the suite if a tool is removed
   without decrementing the count *and* the human-readable comment block. Bundle
   the count edit into the same commit as each tool deletion.

6. **`msg_blocks.c:410` calls `shadow_feeder_global_observe` on every connect**
   via a forward extern (`:266-268`) to avoid a `lib/net`→`adapters` layer dep.
   It is a no-op unless `-shadow` armed the global, but the extern symbol must be
   removed from `msg_blocks.c` in the **same** change that deletes
   `shadow_feeder_global.c`, or the link breaks.

7. **`header_admit_stage_diff` is apparatus buried in a SURVIVOR job** — same trap
   shape as #3 (`cutover_modes`). `header_admit_stage.c` is the authoritative
   header-admit Job and **must survive**; only its `header_admit_stage_diff`
   (`:426`, body ~`:410-560`) + the diff structs/enum in
   `header_admit_stage.h:113-182` are comparison. Deleting the file wholesale (or
   `cutover_modes_get_header_admit` at `:193` that the stage reads) drops the live
   header pipeline. Trim the harness, keep the stage. Likewise its test
   `test_header_admit_stage.c` is a KEEP test — trim only the 9 diff assertions.
   This component, and its MCP tool `zcl_diff_staged_header_admit`, were entirely
   missing from the prior revision and account for the Step-3 `EXPECTED_CHAIN`
   decrement being **−4, not −3**.

---

## 5. Open UNCERTAINs (resolve at execution, do not guess)

1. Is `zcl_replay_verify` / the `replay_*` family part of the apparatus or a
   surviving PROVE/postmortem tool? It is co-mentioned with the cutover proof
   (`test_mcp_controllers.c:65,91`) but `zcl_replay_dump/exec/verify` look like a
   general replay-buffer capability (`ops_controller`/`chain_controller`). Read
   `replay_verify_service.h` callers before assuming it dies with the cutover.
   **Provisional: KEEP** (general replay, not shadow-diff).
2. Whether to retire the `zcl_state subsystem=cutover` dumper entirely or shrink
   it. If any post-flip operator view still wants "which mode is each stage in,"
   keep a trimmed dumper; otherwise drop the `diagnostics_registry.c:454` row and
   its enum mention (and any `enum_csv` assertion).
3. `one_result_type_baseline.txt` lists `cutover_modes.c` (verified). Since that
   file is KEEP, the line stays; the other apparatus files are absent (verified
   clean). No E2 baseline edit is needed by this sequence.
