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
| MCP `zcl_utxo_projection_diff` | `tools/mcp/controllers/chain_controller.c:720` (handler), `:1040` (route) | (within 1063) | route table; reads `getutxocommitment` + projection | **(b)** | "24h cutover soak gate" (`chain_controller.c:704-709`). Domain "chain". Decrements `EXPECTED_CHAIN`+`EXPECTED_TOTAL`. |
| MCP `zcl_block_index_diff` | `tools/mcp/controllers/chain_controller.c:520` (handler), `:1049` (route) | (within 1063) | route table; folds block_index_projection vs live | **(b)** | "Gates the 4c-cutover PR … 24h of match=true" (`chain_controller.c:468,1054`). Domain "chain". |
| MCP `zcl_diff_with_legacy_shadow` | `tools/mcp/controllers/chain_controller.c:95` (handler), `:1015` (route) | (within 1063) | route table; calls the pure op + shadow_feeder_global | **(b)** | The live shadow-vs-legacy byte diff. Domain "chain". |
| MCP `zcl_diff_with_legacy` (composite verdict) | `tools/mcp/controllers/diagnostics_controller.c:252` (handler), `:715` (route) | (within 767) | route table | **(b)** | "composite are-we-tracking verdict" (`:17`) — reads `getmirrorstatus` + mirror_status. Domain "ops". NB: reads the KEEP mirror status, but is itself a comparison verdict → delete. |
| projection-side diff helpers | folded inline in `lib/storage/src/{mempool,peers,znam,contacts,onion_announcements}_projection.c` + `small_projections.h` | — | only the `*projectiondiff` RPCs | **(c) the projections / (b) the diff entry points** | The **projections themselves are KEEP** (they are the post-flip read-models, FRAMEWORK §2 fold targets). Only the *count/first_diff comparison* helpers invoked by the RPCs are apparatus. There is no separate `_projection_diff()` symbol to delete — the comparison lives in the controller; the projection just exposes `count`/`catch_up`. **Do NOT delete projection files.** |

### 1c. Cutover mode / preflight / canary plumbing

| Component | File(s) | LOC | Callers / registration | Class | Spot-check note |
|---|---|---|---|---|---|
| `cutover_modes` (mode switch) | `app/services/src/cutover_modes.c` (+`.h` 71) | 258 | stages (`header_admit_stage.c:185`, `validate_headers_stage.c:494`, `tip_finalize_stage.c:620`); `cutover_controller.c`; conditions; `diagnostics_registry.c:454` (`zcl_state=cutover`) | **(c) the mode get/set; (b) the canary sub-API** | `cutover_modes_set_*`/`get_*` **IS the authority switch** the stages read — KEEP. The canary half (`record_change`, `canary_snapshot`, `canary_target_reached`, `clear_canary`, `struct cutover_canary_snapshot`) is flip-window comparison → **(b)**. `cutover_dump_state_json` (`zcl_state=cutover`) KEEP but shrinks. |
| `cutover_controller` (`cutovermode` RPC) | `app/controllers/src/cutover_controller.c` | 273 | RPC `diagnostics_controller.c:32`; MCP `zcl_cutovermode` (`ops_controller.c:930`) | **(c) the flip/revert RPC; (b) the canary push** | `diag_rpc_cutovermode` sets/reverts authority and is the operator's flip + safety-revert control — KEEP. `cutover_push_canary_state` + `push_cutover_modes`' canary fields are comparison → **(b)**. |
| `cutover_controller_preflight` (`cutoverpreflight` RPC) | `app/controllers/src/cutover_controller_preflight.c` | 607 | RPC `diagnostics_controller.c:33`; MCP `zcl_cutoverpreflight` (`ops_controller.c:939`); called by `cutover_controller.c:192` (the flip's own ready-gate) | **(b)** | Read-only "ready" snapshot ANDing every safety gate incl. the fed==diffed conservation law (`cutover_controller_preflight.c:14-18`). **TRAP:** `cutover_controller.c:192` (`cutover_preflight_ready_now`) calls it to *refuse an unsafe flip* — it cannot be deleted while the flip RPC still gates on it. It dies only when the flip is permanent (no more runtime flips to gate). |
| `cutover_no_forward_progress` (Condition) | `app/conditions/src/cutover_no_forward_progress.c` | 142 | `condition_registry.c:23,48` | **(b) — the auto-revert safety net** | Detects authoritative stages that stopped advancing the tip and **reverts to shadow** (`:75,134`). This is the live auto-rollback during the flip window. **Most dangerous to delete early** (see §4). |
| `cutover_canary_complete` (Condition) | `app/conditions/src/cutover_canary_complete.c` | 108 | `condition_registry.c:24,49` | **(b)** | Watches the canary block; on completion calls `cutover_modes_revert_all_to_shadow` (`:45`) / promotes. Flip-window only. |
| MCP `zcl_cutovermode` | `tools/mcp/controllers/ops_controller.c:294,930` | (within file) | route table | **(c)** | Proxy to the KEEP flip RPC. Survives until flip permanent. |
| MCP `zcl_cutoverpreflight` | `tools/mcp/controllers/ops_controller.c:323,939` | (within file) | route table | **(b)** | Proxy to preflight. Dies with preflight. |

### 1d. The cutover *proof* test corpus (gated-on-flip; delete last)

| Test file | LOC | Group reg. | Class | Note |
|---|---|---|---|---|
| `test_diff_with_legacy_shadow.c` | 215 | `test.c` | **(b)** | Pins the pure diff op. |
| `test_shadow_feeder.c` | 225 | `test.c` | **(b)** | |
| `test_shadow_feeder_global.c` | 131 | `test.c:844` | **(b)** | |
| `test_shadow_conservation.c` | 347 | `test.c` | **(b)** | fed==diffed law. |
| `test_shadow_replay_full_driver.c` | 206 | `test.c` | **(b)** | |
| `test_cutover_flip_dryrun.c` | 725 | `test.c` | **(b)** | Flip dry-run safety proof. |
| `test_cutover_tip_parity.c` | 473 | `test.c` | **(b)** | Tip-parity proof. |
| `test_cutover_postflip_reorg.c` | 880 | `test.c` | **(b)** | Post-flip reorg parity. |
| `test_reorg_parity.c` | 673 | `test.c` | **(b)** | Reorg parity (shadow vs authoritative). |
| `test_small_projections.c` | 683 | `test.c` | **(c) keep** | Tests the projection folds themselves (KEEP read-models), not just the diff. Keep; trim only the `*_projection_diff` assertions. |

**Apparatus LOC (classes a/b, deletable when flip is permanent), excluding KEEP:**
core diff/feeder ~750 (incl. headers + CLI tool) + projection_diff_controller ~700 (less the small `getmirrorstatus`) + preflight 607 + 2 cutover Conditions 250 + canary sub-surface of cutover_modes/controller ~150 + MCP route/handler stubs ~200 + proof test corpus ~3,200. **≈ 6,300 LOC** total once the flip is permanent (~3,100 production + ~3,200 tests). None of it deletable *now*.

---

## 2. Ordered deletion sequence (executes only AFTER the flip is made permanent)

Gate-0 precondition for the whole sequence: the compile-time stage-mode default
has flipped from SHADOW to AUTHORITATIVE, the runtime revert path is retired, and
the live tip has been on the log-derived pipeline long enough to trust. Until
then, **stop** — the apparatus is the proof, not cruft.

Each step is independently `make lint`-clean and `./test_parallel`-green. The
only gate that moves is the **MCP tool-count contract** in
`test_mcp_controllers.c` (`EXPECTED_TOTAL`/`EXPECTED_OPS`/`EXPECTED_CHAIN`); E1
and E6 are **untouched** by this sequence.

**Step 1 — retire the runtime flip surface (unblocks deleting modes/canary).**
- Delete the `cutovermode`/`cutoverpreflight` RPCs (`cutover_controller.c`,
  `cutover_controller_preflight.c`), their decls (`diagnostics_internal.h:43,47`)
  and rows in `diagnostics_controller.c:32,33`.
- Delete MCP `zcl_cutovermode` + `zcl_cutoverpreflight` (`ops_controller.c:294,
  323,830,838,930,939`). **`EXPECTED_OPS −2`, `EXPECTED_TOTAL −2`** + drop the two
  count-comment lines (`test_mcp_controllers.c:62,63,83`).
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
  `register_*` decls/calls (`condition_registry.c:23,24,48,49`) and
  `watchdog_dissolve_pr3.h` decls. Migrate/drop `test_condition_engine.c`,
  `test_watchdog_conditions_pr3.c` cutover cases.
- **Unblocks:** nothing depends on these; safe once Step 1's RPC revert is gone.

**Step 3 — delete the diff RPCs + MCP tools (the daily `*_projection_diff` surface).**
- Delete the 7 `*projectiondiff` RPCs in `projection_diff_controller.c` (KEEP
  `getmirrorstatus` — move it to a surviving controller or keep the file as a
  one-RPC stub), `zcl_diff_with_legacy` (`diagnostics_controller.c:252,715`), and
  the 7 `zcl_*_projection_diff` MCP routes (`diagnostics_controller.c:112-169,
  677-706`).
- Delete `zcl_utxo_projection_diff`, `zcl_block_index_diff`,
  `zcl_diff_with_legacy_shadow` from `chain_controller.c` (`:95,520,720,1015,
  1040,1049`).
- **MCP counts:** `EXPECTED_OPS −` (mempool/znam/wallet projection diffs +
  diff_with_legacy that route "ops"), `EXPECTED_CHAIN −3`,
  `EXPECTED_TOTAL −(all of the above)`. Update every count-comment line
  (`test_mcp_controllers.c:52,55-60,78-80,85-88`) in the same commit.
- **Do NOT touch the projection files in `lib/storage`** — they are KEEP folds.

(Recommended first 3 steps = Steps 1–3 above.)

**Step 4 — delete the pure ops + feeder + conservation (no MCP caller left).**
- Delete `application/operations/{src,include}/diff_with_legacy_shadow.*`,
  `shadow_replay_proof.*`, `tools/shadow_replay_proof.c`.
- Delete `adapters/inbound/src/shadow_feeder.c`, `shadow_feeder_global.c`,
  their headers, and `shadow_conservation.h`. Remove the `-shadow` flag
  (`main.c:1485`, `boot.h:54`), `boot_services.c:156-230,2949,3167,3180` feeder
  lifecycle, and the `msg_blocks.c:266-410` extern + observe call.

**Step 5 — delete the proof test corpus.**
- Remove `test_{diff_with_legacy_shadow,shadow_feeder,shadow_feeder_global,
  shadow_conservation,shadow_replay_full_driver,cutover_postflip_reorg,
  reorg_parity}.c`, their decls (`test_helpers.h`) and registrations
  (`test.c`, `test_parallel.c:131` X-list). Keep `test_small_projections.c`
  (trim its diff assertions only).

**Build note:** `app/`, `application/`, `adapters/` are glob-built (no explicit
source list) — file deletion is self-effecting. The MCP tool-count contract and
the `test_parallel.c:131` X(...) macro list are the only manual non-baseline
edits.

---

## 3. Net effect on lint gates (verified)

| Gate | Effect of this deletion | Why |
|---|---|---|
| **E6 `one-write-path`** (64 surfaces) | **NONE** | Baseline lists only the legacy *writer* (`coins_view`/`connect_tip`/`active_chain_set_tip`); zero apparatus entries. E6 shrinks when the legacy writer dies, a different B8 sub-task. |
| **E1 `file-size-ceiling`** (7→ baseline) | **NONE** | No apparatus file is in `file_size_ceiling_baseline.txt`. `projection_diff_controller.c`=715<800; the MCP files are `tools/`, outside the `app/**` gate. |
| **MCP tool-count contract** (`test_mcp_controllers.c`) | **MUST shrink in lockstep** | `EXPECTED_TOTAL 109`, `EXPECTED_OPS 46`, `EXPECTED_CHAIN 18` are hard asserts. Each deleted tool decrements the matching count + comment in the same commit, or the suite fails. |
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
   (`legacy_mirror_sync_dump_state_json`) and backs MCP `zcl_mirror_status`
   (`ops_controller.c:867`) + the `zcl_diff_with_legacy` composite. Deleting the
   file wholesale 404s the mirror-status tool. Extract `getmirrorstatus` before
   deleting the file.

5. **The MCP tool-count asserts are silent landmines.** `EXPECTED_TOTAL/OPS/
   CHAIN` (`test_mcp_controllers.c:49,65,84`) fail the suite if a tool is removed
   without decrementing the count *and* the human-readable comment block. Bundle
   the count edit into the same commit as each tool deletion.

6. **`msg_blocks.c:410` calls `shadow_feeder_global_observe` on every connect**
   via a forward extern (`:266-268`) to avoid a `lib/net`→`adapters` layer dep.
   It is a no-op unless `-shadow` armed the global, but the extern symbol must be
   removed from `msg_blocks.c` in the **same** change that deletes
   `shadow_feeder_global.c`, or the link breaks.

---

## 5. Open UNCERTAINs (resolve at execution, do not guess)

1. Is `zcl_replay_verify` / the `replay_*` family part of the apparatus or a
   surviving PROVE/postmortem tool? It is co-mentioned with the cutover proof
   (`test_mcp_controllers.c:64`) but `zcl_replay_dump/exec/verify` look like a
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
