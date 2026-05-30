# Cutover finish tracker

_Auto-generated 2026-05-30 from the `cutover-finish-tracker` workflow (HEAD `13289b54b`). The source of truth is the live `zcl-rpc cutoverpreflight`; this board tracks the path to a fully-authoritative reducer + the B8 deletion._

## Where we are

Definitive finish runbook from HEAD 13289b54b. The KEYSTONE (ce499b321) and Item 1b boot anchor-seed (13289b54b) are committed: the author flip is wired at app/controllers/src/cutover_controller.c:278 (utxo_projection_set_author, behind the cutover_preflight_ready_now() gate + stage=all), with the stage-side reorg-unwind inverse path it depends on landed in utxo_apply_delta.c / utxo_apply_stage.c:371, and the boot anchor-seed addressing the utxo_commitment gate is in. Compile-time default is still UTXO_AUTHOR_LEGACY (shadow), so a bad flip is one RPC to revert, never a rebuild. The DEPLOYED binary is still pre-keystone (1793a449d) — every live preflight number is stale until `make deploy` runs. Authoritative re-measure: honest_authoritative_pct=0; three RED shadow->auth gaps remain (validate_headers_failures, conservation fed=67/diffed=0, utxo_commitment_divergent); live/chain_advance/guard are GREEN. The hard ordering to a fully-authoritative reducer: deploy keystone binary -> resolve validate_headers window (in_flight) + 675K backfill -> wire conservation diffed-driver Job (the only remaining net-new code) -> confirm all 8 preflight gates GREEN at at-tip clean datadir -> THE FLIP (human, never auto) -> post-flip soak with the auto-revert canary unfired -> compile-time default flip to AUTHORITATIVE + retire revert path -> THEN B8 deletion of the 7,650-LOC comparison apparatus in its mandatory 5-step sequence. B8 has zero E1/E6 baseline impact; its only CI contract is the MCP tool-count asserts in test_mcp_controllers.c (now 112/47/20). Two human gates: the live-chain authority flip and the trust/commitment posture; the orchestrator never flips autonomously.

- **honest authoritative %:** 0 (the flip MECHANISM is wired at `app/controllers/src/cutover_controller.c:278`, but compile-time default is `UTXO_AUTHOR_LEGACY` and nothing has flipped on the live chain).
- **legacy emitters silenced when `author==STAGE`:** `lib/validation/src/update_coins.c:53 (update_coins_emit_utxo_add_shadow)`, `lib/validation/src/update_coins.c:77 (update_coins_emit_utxo_spend_shadow)`
- **next owner action:** Run `make deploy` (item D1) to ship the keystone+anchor-seed binary (HEAD 13289b54b) to the live node — the currently deployed binary is pre-keystone 1793a449d, so every live preflight number (validate_headers_failures, fed=67/diffed=0, utxo_commitment_divergent) is stale and cannot be trusted to drive the next decisions. This restarts the live zclassic23 service (owner-gated; never stop zclassicd). After it comes back, read `zcl-rpc cutoverpreflight` to get the true keystone-build gate state, then dispatch the conservation Job (G2) — the only remaining net-new code on the path.

## Live preflight gates

| state | gate | tracked item |
|---|---|---|
| ✅ green | live | — |
| ✅ green | chain_advance | — |
| ✅ green | guard | — |
| 🔴 red | utxo_commitment_divergent | G3 (needs D1 deploy of K2 seed) |
| 🔴 red | validate_headers_failures | G1 (Item 2, in flight) + G1b backfill |
| 🔴 red | shadow fed==diffed | G2 (Item 3 conservation Job) |

## Critical path

`K1 → K2 → D1 → G1 → G1b → G2 → G3 → P1 → F1 → S1 → F2 → B8`

| | id | item | gate it greens | depends |
|---|---|---|---|---|
| ✅ | **K1** | KEYSTONE — stage-side reorg-unwind + UTXO author flip wired | n/a (code wired; no gate greens until deployed+flipped) | — |
| ✅ | **K2** | Item 1b — boot anchor-seed wiring (utxo_commitment foundation) | n/a until deployed (then enables utxo_commitment to converge) | K1 |
| 🔒 | **D1** | make deploy — ship the keystone+anchor-seed binary to the live node 🔒owner | none directly; makes all gate measurements truthful for the keystone build | K2 |
| 🔄 | **G1** | Item 2 — validate_headers: pass the full script-validation window with zero failures | validate (validate_headers_failures -> GREEN) | D1 |
| 🔒 | **G1b** | 675K validate_headers backfill — drive vh_cursor up to tip+1 🔒owner | validate (validate_headers_cursor_lag -> GREEN) | G1 |
| ⬜ | **G2** | Item 3 — conservation Job: drive shadow_conservation diffed counter from a supervised background Job | conservation (shadow_pipeline_dropped_blocks: fed==diffed -> GREEN) | D1 |
| ⬜ | **G3** | utxo_commitment gate converges — SHA3(legacy coins.db) == SHA3(utxo_projection) byte-for-byte | utxo_commitment (utxo_commitment_divergent -> GREEN) | D1, K2 |
| ⬜ | **P1** | Confirm cutoverpreflight GREEN at the clean at-tip import-anchored datadir | all 8 (ready=true, blockers=[]) | G1b, G2, G3 |
| 🔒 | **F1** | THE LIVE-CHAIN AUTHORITY FLIP — cutovermode all authoritative 🔒owner | n/a (flips authority; gates already GREEN pre-flip) | P1 |
| ⬜ | **S1** | Post-flip soak — log-derived pipeline as sole writer, live tip advancing 🔒owner | n/a (observation window; confirms flip safe) | F1 |
| 🔒 | **F2** | Make the flip PERMANENT — compile-time default AUTHORITATIVE, retire runtime revert 🔒owner | n/a (makes B8 Gate-0 #1 true) | S1 |
| 🔒 | **B8** | B8 — delete the comparison apparatus (~7,650 LOC) in the mandatory sequence 🔒owner | n/a (irreversible removal of the safety net; post-soak only) | F2 |

### Definitions of done

- **K1 KEYSTONE — stage-side reorg-unwind + UTXO author flip wired** — utxo_projection_set_author(UTXO_AUTHOR_STAGE) is the FIRST production caller, behind cutover_preflight_ready_now() + stage=all, flipping atomically with tip_finalize AUTHORITATIVE; the stage-side inverse path utxo_apply_reorg_unwind_if_needed exists so the stage can disconnect blocks once authoritative; legacy update_coins emitters silenced on flip. Committed ce499b321.
- **K2 Item 1b — boot anchor-seed wiring (utxo_commitment foundation)** — One-time idempotent anchor-seed copies coins.db utxos into the utxo_projection table byte-identically (gated on seeded_from_anchor flag, not row-count), so SHA3(legacy)==SHA3(projection) is reachable. Wired into boot. Committed 13289b54b (HEAD).
- **D1 make deploy — ship the keystone+anchor-seed binary to the live node** — `make deploy` (lint + build zclassic23/zclassic-cli/wal_checkpoint -> WAL-checkpoint node.db -> install service -> `systemctl --user restart zclassic23` -> deploy_verify.sh) runs the binary at HEAD 13289b54b. After restart, `zcl-rpc cutoverpreflight` reflects the keystone build (currently the deployed binary is pre-keystone 1793a449d, so EVERY live preflight number is stale). DoD = node back up, tip advancing, preflight readable, no crash. Restarts the LIVE chain service -> owner-gated. NEVER stop zclassicd.
- **G1 Item 2 — validate_headers: pass the full script-validation window with zero failures** — At the live tip: validate_no_failures && error_count==0 && window available+complete+failed_count==0 (validate_clean), AND vh_cursor>=rep.end_height+1 with vh_cursor_lag==0 (validate_caught_up). Failures originate in validate_headers_validator.c CheckProofOfWork/check_equihash_solution; g_failed_total must be 0 and the recheck cursor must clear any prior failed rows. Stage stays in shadow mode (mode read at validate_headers_stage.c:485) during preflight. Blocker name today: validate_headers_failures_present (preflight.c:665).
- **G1b 675K validate_headers backfill — drive vh_cursor up to tip+1** — The validate_headers stage backfills its ~675K-block window so vh_cursor reaches rep.end_height+1 (cursor_lag==0). This is long-running re-validation work on the live datadir; gate stays RED until the window is fully covered with zero failures. Owner-gated because it is bounded by live tail completion / sync-tip progress on the running node and shares the live validation surface.
- **G2 Item 3 — conservation Job: drive shadow_conservation diffed counter from a supervised background Job** — conservation_ok (preflight.c:358-390, shadow_conservation_ok) requires fed==diffed at quiesce; today diffed is bumped ONLY by the MCP handler (chain_controller.c:189) so fed climbs unbounded (live: fed=67 diffed=0). Add a supervised observe-only Job (shape = app/jobs, registered under -shadow in boot_shadow_feeder_start) that drives shadow_conservation_record_diffed. CRITICAL folds: (a) the import-anchored shadow log is SPARSE — diff from the log's actual FLOOR via a NEW min_height accessor (does not exist yet; grep on block_log_port/blf min_height is empty), not height 0 (else SHADOW_MISSING short-circuits with checked_count=0); (b) reconcile counter currency — fed is per-observation (reorg-double-counted), checked_count is distinct-height — make record_fed a distinct-height set OR compute diffed from the same height-set so fed==diffed is reachable and reorg-stable. Acceptance: live-smoke that diffed tracks fed and the gate goes GREEN at quiesce. THE ONLY remaining net-new code on the critical path.
- **G3 utxo_commitment gate converges — SHA3(legacy coins.db) == SHA3(utxo_projection) byte-for-byte** — push_cutover_utxo_commitment_gate_json: projection_catch_up drains pending folds (tip_lag==0, cursor_lag==0), then the SHA3-256 over the whole legacy UTXO set equals the SHA3 over the log-folded projection, byte-for-byte. The K2 boot anchor-seed populates the projection identically; this item is the LIVE verification that, after deploy + catch-up quiesce, the two walks match. Blocker name today: utxo_commitment_divergent. Largely satisfied by K2 once deployed and quiesced — primarily a convergence/verify milestone, not new code.
- **P1 Confirm cutoverpreflight GREEN at the clean at-tip import-anchored datadir** — All 8 ANDed gates satisfied from clean data at tip WITHOUT operator-driven RPCs: live, chain_advance, guard (already GREEN) + header (header_admit_diff converged + cursor_lag==0), validate (G1/G1b), modes (ha+vh in shadow), conservation (G2), utxo_commitment (G3). `zcl-rpc cutoverpreflight` returns ready=true with an empty blockers array. Read-only milestone; its inputs are everything above.
- **F1 THE LIVE-CHAIN AUTHORITY FLIP — cutovermode all authoritative** — Only after P1 GREEN: operator runs `zcl-rpc cutovermode all authoritative`. The preflight-gated flip at cutover_controller.c:278 sets UTXO_AUTHOR_STAGE, making utxo_apply_stage the SOLE consensus writer (forward delta + stage-side reorg-unwind) and silencing the legacy update_coins.c:53/77 shadow emitters. Compile-time default stays UTXO_AUTHOR_LEGACY at this stage, so revert is one RPC. THE single most safety-critical action; ALWAYS human-gated, never autonomous.
- **S1 Post-flip soak — log-derived pipeline as sole writer, live tip advancing** — Soak the authoritative event-log path on the LIVE tip for N blocks (>= several hundred) AND N seconds with: tip advancing, projection==replay invariant holding, fed==diffed conservation law held across the whole soak, ZERO divergence, and the cutover_no_forward_progress auto-revert Condition NEVER fired (it is the live rollback if the flip stalls). Forward-progress on the live tip is the gate, not green tests. This is B8 Gate-0 preconditions (2)(4)(5).
- **F2 Make the flip PERMANENT — compile-time default AUTHORITATIVE, retire runtime revert** — After the soak holds: flip the compile-time default at cutover_modes.c:16 + the :249 reset to the AUTHORITATIVE bitmask and retire cutover_modes_revert_all_to_shadow (:67). Do NOT edit the stage helpers (header_admit_stage.c:185, validate_headers_stage.c:494, tip_finalize_stage.c:620 — they only READ modes; editing them would leave the pipeline in SHADOW after a supposed permanent flip). This satisfies B8 Gate-0 precondition (1): default is AUTHORITATIVE and the runtime revert path is unreachable. Requires deploy + a soak on the permanent default. Owner-gated (consensus default + live restart).
- **B8 B8 — delete the comparison apparatus (~7,650 LOC) in the mandatory sequence** — ONLY after S1 soak passes AND F2 makes the default permanent (Gate-0 all true). Execute the b8-plan in MANDATORY order: Step 1 trim canary sub-API + retire flip RPC then preflight (preflight gates the flip — retire flip first); Step 2 delete the canary/no-forward-progress Conditions LAST (the auto-revert net); Step 3 retire the *_projection_diff + header_admit_stage_diff MCP tools + harness (extract getmirrorstatus to a survivor first; chain decrement is -4 incl zcl_diff_staged_header_admit); Step 4 delete the now-unreachable shadow ops/adapters (shadow_feeder_global delete + msg_blocks.c:410 extern removal in the SAME commit); Step 5 delete the apparatus test corpus. TRAPS: header_admit_stage.c is a SURVIVOR (trim only the diff); cutover_modes/cutover_controller KEEP the authority switch; getmirrorstatus survives. DoD = make lint clean + test_parallel green (read 'N passed, M failed'), MCP tool-count asserts pass, node still authoritative and advancing. Reducer is then the SOLE consensus writer with no shadow apparatus.

## B8 deletion inventory (post-flip only)

**7,650 LOC across 32 files.** Deletes ONLY after the flip is permanent + soaked (Gate-0). The apparatus is the *proof the flip is safe* — load-bearing until then.

> CRITICAL PRECONDITIONS (Gate-0 must ALL be true before ANY deletion): (1) compile-time stage-mode default is AUTHORITATIVE, runtime revert path is unreachable; (2) live tip has soaked on log-derived pipeline for N blocks (≥several hundred) with zero divergence; (3) cutoverpreflight reports GREEN at soak tip; (4) fed==diffed conservation law held across soak; (5) cutover_no_forward_progress auto-revert Condition never fired; (6) make lint + test_parallel green. Deletion sequence is MANDATORY: flip → soak → confirm → THEN delete (no safe reordering). TRAPS: cutover_no_forward_progress is the auto-revert safety net (delete LAST); cutoverpreflight is the flip RPC's own gate (retire flip RPC before preflight); cutover_modes is the authority switch (keep mode get/set, delete only canary sub-API); getmirrorstatus rides inside projection_diff_controller but survives; MCP tool-count asserts (EXPECTED_TOTAL/OPS/CHAIN in test_mcp_controllers.c) must be decremented in lockstep with tool deletions or suite fails; msg_blocks.c:410 extern must be removed in same commit as shadow_feeder_global.c deletion; header_admit_stage.c is SURVIVOR (trim only the diff harness + structs in header, keep the stage). The apparatus is load-bearing *until* the flip is permanent — it is the proof the flip is safe, not cruft. None of it is deletable today.

| file | LOC | what | when |
|---|---|---|---|
| `lib/test/src/test_reorg_parity.c` | 939 | Reorg parity test (shadow vs authoritative pipelines) | Step 5 |
| `lib/test/src/test_cutover_postflip_reorg.c` | 880 | Post-flip reorg parity test | Step 5 |
| `lib/test/src/test_cutover_flip_dryrun.c` | 725 | Flip dry-run safety proof (exercises cutover_modes + preflight + heade | Step 5 |
| `app/controllers/src/cutover_controller_preflight.c` | 718 | cutoverpreflight RPC (read-only ready-gate ANDing all safety gates inc | Step 1 |
| `app/controllers/src/projection_diff_controller.c` | 715 | 8 *projectiondiff RPCs folding projections vs legacy SQLite tables; KE | Step 3 |
| `app/jobs/src/header_admit_stage.c` | 677 | SURVIVOR Job (authoritative header pipeline); DELETE only the header_a | Step 3 |
| `lib/test/src/test_cutover_autorevert.c` | 638 | Pins the cutover_no_forward_progress auto-revert Condition | Step 2 or Step 5 |
| `lib/test/src/test_cutover_tip_parity.c` | 473 | Tip-parity proof (legacy vs authoritative) | Step 5 |
| `lib/test/src/test_shadow_conservation.c` | 333 | Pins the fed==diffed conservation law | Step 5 |
| `lib/test/src/test_cutover_preflight.c` | 333 | Pins the preflight ready-gate | Step 1 |
| `tools/shadow_replay_proof.c` | 291 | Standalone CLI tool for offline proof driver (not linked into node) | Step 4 |
| `app/controllers/src/cutover_controller.c` | 288 | cutovermode RPC (the operator's flip+revert control); KEEP the flip/re | Step 1 |
| `app/services/src/cutover_modes.c` | 258 | Mode switch (authority gate for stages) KEEP the get/set/dump; DELETE  | Step 1 |
| `lib/test/src/test_shadow_feeder.c` | 211 | Tests the -shadow feeder adapter | Step 5 |
| `lib/test/src/test_diff_with_legacy_shadow.c` | 207 | Pins the pure diff_with_legacy_shadow op | Step 5 |
| `lib/test/src/test_shadow_replay_full_driver.c` | 198 | Full shadow replay proof driver test | Step 5 |
| `app/jobs/include/jobs/header_admit_stage.h` | 188 | SURVIVOR header; DELETE only the diff harness (enum header_admit_diff_ | Step 3 |
| `app/conditions/src/cutover_no_forward_progress.c` | 142 | Auto-revert safety net detects tip stall in authoritative stages and r | Step 2 |
| `adapters/inbound/src/shadow_feeder.c` | 140 | -shadow flag gated adapter observing blocks→shadow log; pure side-chan | Step 4 |
| `lib/test/src/test_shadow_replay_proof.c` | 139 | Pins the shadow_replay_proof lib op + CLI | Step 5 |
| `lib/test/src/test_shadow_feeder_global.c` | 117 | Tests the forward-extern shim | Step 5 |
| `app/conditions/src/cutover_canary_complete.c` | 108 | Watches canary block completion; on completion calls revert or promote | Step 2 |
| `application/operations/src/shadow_replay_proof.c` | 105 | Offline cutover proof skeleton library op; replays primary→shadow then | Step 4 |
| `adapters/inbound/include/adapters/inbound/shadow_conservation.h` | 99 | Header declaring shadow_conservation counters (fed==diffed law enforce | Step 4 |
| `adapters/inbound/include/adapters/inbound/shadow_feeder.h` | 96 | Header for shadow_feeder adapter | Step 4 |
| `application/operations/src/diff_with_legacy_shadow.c` | 80 | Pure byte-compare op of primary vs shadow block-log ports; zero author | Step 4 |
| `app/services/include/services/cutover_modes.h` | 71 | Header for cutover_modes (trim canary struct/enum declarations, keep m | Step 1 |
| `adapters/inbound/src/shadow_conservation.c` | 70 | Conservation law counters (fed==diffed); preflight gate refusing flip  | Step 4 |
| `application/operations/include/application/operations/diff_with_legacy_shadow.h` | 61 | Header for diff_with_legacy_shadow pure op | Step 4 |
| `application/operations/include/application/operations/shadow_replay_proof.h` | 60 | Header for shadow_replay_proof lib op | Step 4 |
| `adapters/inbound/include/adapters/inbound/shadow_feeder_global.h` | 54 | Header for shadow_feeder_global shim | Step 4 |
| `adapters/inbound/src/shadow_feeder_global.c` | 34 | Forward-extern shim so lib/net can call feeder without layer dep; no-o | Step 4 |
