# B8 — Exact Legacy-Deletion Inventory

> **Status:** research-only checklist for **B8** (`docs/REFACTOR_STATUS.md`).
> Updated 2026-05-28: `legacy_bootstrap_importer` is already deleted in this tree.
> **Read [`docs/REFACTOR_STATUS.md`](../REFACTOR_STATUS.md) C1–C4 first** — the
> C-workstream is the *how*; this doc is the *exact symbol/caller/test/build
> ledger* so B8 is mechanical with B7 completed.

**C1 status:** force-window deletion is complete (`force_mirror_promotion` and
`force_mirror_active` are removed from live paths); remaining C1 work is policy
rewire/consumer re-pointing.

## TL;DR — the framing was wrong; correct it before deleting

The task brief named "4 legacy modules (~6,059 LOC) slated for deletion." That
LOC is real but **only one of the four is a clean wholesale delete.** The honest
classification, derived below from `git grep` + reading every call site:

| Module | LOC | B8 verdict | Why |
|---|---|---|---|
| `app/services/src/legacy_bootstrap_importer.c` | 1721 | **DELETED in-tree** | The module is gone already (flags, module, and attach test were removed). |
| `app/services/src/legacy_mirror_sync_service.c` | 1487 | **KEEP the heartbeat/monitor; DELETE only the block-application half** | The always-on live-sync from zclassicd. Heartbeat + lag-SLO monitor + stats snapshot are load-bearing (health, metrics, conditions, supervisor all consume it). C2. |
| `app/services/src/chain_advance_coordinator.c` | 1354 | **REWIRE shell — pure policy already extracted** | Source-scoring/name/plan policy now lives in `block_source_policy.{h,c}` behind CAC compatibility wrappers. Remaining CAC shell owns runtime input, state/status/dump, Conditions, snapshot_offer, connect_tip, controllers, `zcl_status`. C1. |
| `app/services/src/utxo_recovery_service.c` | 1141 | **REWIRE — `clean_above_tip` → Condition; rest is boot-recovery, survives until A-workstream re-homes it** | Almost entirely `boot.c` boot-time recovery (wipe/import/restore/integrity). Not a live-tip feeder. C3. |
| `lib/validation/src/connect_tip.c` | 1087 | **KEEP — this is the SURVIVING authoritative connect path** | Called by `activate_best_chain.c` (10 sites). This is the *post-flip* consensus connect, not legacy. The brief's "connect_tip paths that exist only to feed the legacy tip" do **not** exist — there is one connect_tip and it survives. |
| `app/controllers/src/legacy_import.c` | 1105 | **KEEP / OUT OF SCOPE — wallet-scan utility, not a tip feeder** | Reads block files to recover the *wallet's own* txs (`snapshot_controller_import.c`, `wallet_diagnostic_repair.c`). Despite the `legacy_` prefix it is wallet recovery, not the chain pipeline. |

**The one block-application module the brief missed:**
`app/services/src/legacy_body_pull.c` (529 LOC) — `legacy_body_pull_range_incremental()`
is what the mirror catchup calls to actually *apply* blocks. This is the
"block-application coordination" C2 deletes. It is NOT baselined and NOT in the
brief's list, but it is the real deletable core that the mirror's KEEP half calls.

**Net wholesale-deletable now:** `legacy_bootstrap_importer.c` and its attach test are already removed. Everything else is a
*surgical extraction* (C1/C2/C3), not a file delete — deleting the mirror heartbeat
is still the real risk.

---

## 1. Per-module symbol table

Call sites from `git grep -n <symbol>`; each spot-checked by reading ±20 lines.

### 1a. `legacy_bootstrap_importer.c` — DELETED (already complete)

The module, header, and importer attach test are removed from this tree.

| Artifact | Final state |
|---|---|
| `app/services/src/legacy_bootstrap_importer.c` / `.../legacy_bootstrap_importer.h` | Deleted |
| `config/src/boot.c` CLI import flag branches | Deleted |
| `lib/test/src/test_legacy_bootstrap_attach.c` | Deleted |
| `app/services/src/legacy_bootstrap_spotcheck.c` | Deleted (importer delete-cascade) |
| `tools/rebuild_recent.c:499` height-map reader | Kept (`legacy_bootstrap_load_height_map` remains as local recovery reader; no importer dependency) |

### 1b. `legacy_mirror_sync_service.c` — KEEP heartbeat / DELETE block-apply

Header: `app/services/include/services/legacy_mirror_sync_service.h`.

| Symbol (def line) | Purpose | Call sites | Verdict |
|---|---|---|---|
| `legacy_mirror_sync_init` (`:994`) | Init the always-on mirror service | `config/src/boot_services.c:452`; test `test_zclassicd_oracle.c:303` | **KEEP** — boot wires the heartbeat here. |
| `legacy_mirror_sync_start` (`:1121`) | Start heartbeat tick | `boot_services.c:455` | **KEEP**. |
| `legacy_mirror_sync_stop` (`:1143`) | Stop heartbeat | `boot_services.c:465` | **KEEP**. |
| `legacy_mirror_sync_request_catchup` (`:774`) | One synchronous catch-up — **the dual-purpose function** | `app/conditions/src/legacy_mirror_stuck.c:41` (remedy); test (×2) | **SPLIT** — its *monitor/probe* half (fetch chain-info, lag-SLO eval, anchor verify, observe-local) **KEEP**; its *block-application* half (`legacy_body_pull_range_incremental` at `:900` + `lms_run_activation_to_target` at `:678`,`:919`,`:947`) **DELETE** post-flip (B7 makes the stage pipeline the writer; the mirror should only *monitor* lag, not apply blocks). See §7. |
| `legacy_mirror_sync_stats_snapshot` (`:1151`) | Point-in-time stats | **18 sites**: conditions, `event_controller.c:352`, `health_controller.c:63,309`, `chain_advance_coordinator.c:934,1320`, `node_health_service.c:515`, `chain_supervisor.c:44`, `metrics.c:380`, MCP `meta_controller.c:563`, +tests | **KEEP** — the SLO/lag truth surface. |
| `legacy_mirror_sync_dump_state_json` (`:1272`) | `zcl_state subsystem=legacy_mirror` | `diagnostics_registry.c:421`; `projection_diff_controller.c:55`; test | **KEEP** (registered diagnostics dumper). |
| `legacy_mirror_sync_reload_from_env` (`:1095`) | Hot-reload knobs | MCP `meta_controller.c:561` | **KEEP**. |
| `legacy_mirror_sync_reset_for_test` (`:1358`) | Test reset | tests only (×20) | **KEEP** (test seam for the surviving monitor). |
| `legacy_mirror_sync_test_*` (`:1420,1473,1483`) | `ZCL_TESTING` seams | tests | **KEEP**. |

> Internal functions slated for DELETE when the block-apply half is removed
> (all `static`, no external callers, listed for the implementer): `lms_run_activation_to_target`
> (`:678`), `lms_drain_headers_to_target` (`:743`), `lms_try_recover_stale_next_failed`
> (`:456`), `lms_kick_local_pipeline` (`:638`), `lms_next_block_needs_mirror_body`
> (`:575`), and the body-pull / activation block inside `request_catchup` (`:893`–`:962`).
> **KEEP** the monitor statics: `lms_fetch_chain_info`, `lms_fetch_hash`,
> `lms_local_hash_at`, `lms_verify_anchor`, `lms_verify_after_tip`, `lms_evaluate_lag_slo`,
> `lms_cache_hashes`, `lms_record_stuck_status`, `lms_observe_local_primary`,
> `lms_mark_success`, `lms_state_name`, all `lms_set_*`/`lms_clear_*`.

### 1c. `chain_advance_coordinator.c` — policy extracted / force-window deleted

Header: `app/services/include/services/chain_advance_coordinator.h`.

| Symbol (def line) | Purpose | External (non-test) call sites | Verdict |
|---|---|---|---|
| `cac_source_name` (`block_source_policy.c`) | enum→string | `cutover_controller.c:466`, `event_controller.c:264`, `health_controller.c:387` | **EXTRACTED** — compatibility name survives from `block_source_policy`; direct callers can repoint later. |
| `cac_source_trust_name` (`block_source_policy.c`) | trust label | `cutover_controller.c:468`, `event_controller.c:266`, `health_controller.c:389` | **EXTRACTED**. |
| `cac_decision_result_name` (`block_source_policy.c`) | result→string | `cutover_controller.c:464`, `event_controller.c:262`, `health_controller.c:383` | **EXTRACTED**. Note: `cutover_controller.c` itself is B8-deleted apparatus (the cutover plumbing line in REFACTOR_STATUS B8) — these 3 callers vanish with it. |
| `chain_advance_coordinator_init` (`:995`) | Init policy | `boot_services.c:2732` | **REWIRE** to the re-homed policy init. |
| `chain_advance_coordinator_plan` (`:607`) | Compatibility wrapper for score sources / pick one | **test-only** | **WRAPPER** over `block_source_policy_plan`; delete or repoint tests when the CAC shell is dissolved. |
| `chain_advance_coordinator_mirror_repair_allowed` | Mirror-repair gate | **test-only** | **DELETED** with the force/mirror-repair window. |
| `chain_advance_coordinator_peer_floor_recovery_needed` (`:840`) | Peer-floor recovery decision | `app/conditions/src/peer_floor_violated.c:81` | **REWIRE** — a Condition consumes it; the decision must survive (move to the policy module the Condition calls). |
| `chain_advance_coordinator_snapshot_offer_allowed` (`:891`) | Snapshot-offer gate | `app/services/src/snapshot_offer.c:503,520,552,566,575` | **REWIRE** — snapshot_offer survives; re-home the gate. |
| `chain_advance_coordinator_local_header_refill_needed` (`:941`) | Header-refill decision | `app/conditions/src/local_header_refill_needed.c:61` | **REWIRE** — Condition consumer. |
| `chain_advance_coordinator_note_projection_deferred` (`:1277`) | Record projection deferral | `sync_controller_blocks.c:543`, **`connect_tip.c:938`** | **REWIRE** — connect_tip (the surviving path) calls this; the projection-deferral counter must live somewhere connect_tip can reach. |
| `chain_advance_coordinator_get_status` (`:1268`) | Read last decision | `cutover_controller.c:425`, `event_controller.c:258`, `health_controller.c:372`, `node_health_service.c:319` | **REWIRE** — `node_health_service` + health survive. |
| `chain_advance_coordinator_dump_state_json` (`:1472`) | `zcl_state subsystem=chain_advance_coordinator` | `diagnostics_registry.c:449`, `event_controller.c:315`, `health_controller.c:442` | **REWIRE** — also feeds `zcl_status` MCP (§5). |
| `chain_advance_coordinator_reset_for_test` (`:1566`) | Test reset | tests only | **DELETE/REWIRE** with module. |
| `chain_advance_coordinator_force_mirror_promotion` | Force-promote mirror window | removed from `chain_tip_watchdog.c`, `chain_supervisor.c` | **DELETED** — the stage pipeline is the writer; there is no mirror to force-promote. |
| `chain_advance_coordinator_force_mirror_active` | Is force-window active | removed from tests/introspection | **DELETED** with the force window. |

### 1d. `utxo_recovery_service.c` — REWIRE (boot-recovery + one Condition)

Header: `app/services/include/services/utxo_recovery_service.h`. Every live
caller is `config/src/boot.c` (boot-time recovery) except `clean_above_tip`.

| Symbol (def line) | Purpose | Live call sites | Verdict |
|---|---|---|---|
| `utxo_recovery_wipe` (`:173`) | Policy-gated UTXO wipe (the 2026-04-10 guard) | `boot.c:313` (comment ref only — actual call is internal) | **KEEP/REWIRE** — destructive-op guard; survives until the A-workstream re-homes boot recovery. UNCERTAIN it's B8 scope at all. |
| `utxo_recovery_prepare_reimport` (`:197`) | Clear migration flag | `boot.c:1763` | **KEEP/REWIRE** (boot recovery). |
| `utxo_recovery_import_ldb` (`:210`) | LevelDB→SQLite UTXO import | `boot.c:2423` | **DELETE-CANDIDATE** — only used when importing from legacy LDB; tied to the same cold-import lineage as 1a. UNCERTAIN: also referenced by `activate_best_chain.c:867,875` (comments) — confirm those are comments only (they are) before deleting. |
| `utxo_recovery_restore_chain_tip` (`:530`) | Restore tip from coins-DB best block | `boot.c:2546` | **KEEP/REWIRE** (boot recovery; A-workstream). |
| `utxo_recovery_classify_count_check` (`:121`) | UTXO-count sanity classifier | `test_integrity.c` (×3) — **test-only live** | **KEEP/REWIRE** — pure classifier, reused by integrity checks. |
| `utxo_recovery_xor_mismatch_is_corruption_candidate` (`:155`) | XOR-commitment corruption heuristic | `test_integrity.c:294,295` | **KEEP/REWIRE** (pure). |
| `utxo_recovery_execute` (`:901`) | Run recovery from validator result | `boot.c:2737` | **KEEP/REWIRE** (boot recovery). |
| `utxo_recovery_clean_above_tip` (`:979`) | Delete orphan UTXOs above tip (refuses >1000) | `boot.c:3259` | **REWIRE → Condition** — C3 names this exact function: re-home the orphan-UTXO heal as a Condition first. |
| `utxo_recovery_backfill_shielded` (`:1115`) | Backfill sprout/sapling value into blocks | `boot.c:637` | **KEEP/REWIRE** (idempotent backfill; A-workstream). |

> **utxo_recovery_service is largely NOT a B8 delete.** It is boot-time recovery
> machinery (extracted from boot.c in the 2026-04 decomposition, see its header).
> Only `import_ldb` is cold-import-lineage. `clean_above_tip` becomes a Condition.
> The rest survives until the A-workstream re-homes boot recovery — do not delete
> it as part of "legacy tip" removal.

### 1e. `connect_tip.c` — KEEP (surviving authoritative path)

Single public symbol `connect_tip` (`:80`), called by `activate_best_chain.c`
at `:205,255,282,354,409,417,474,833`. Declared in
`lib/validation/include/validation/process_block.h:42`. **This is the canonical
consensus block-connection.** B7 activates projection reads during cutover authority;
outside cutover, it remains on legacy reader defaults. It is the *destination* of the
cutover, not legacy. **Do not delete.**

### 1f. `legacy_import.c` (controller) — KEEP / out of scope

Single public symbol `legacy_import` (`:688`), a wallet-tx scanner over block
files (`snapshot_controller_import.c:440`, `wallet_diagnostic_repair.c:205`).
Header doc: "Import wallet data from a legacy node's data directory… no LevelDB,
no RPC, no legacy code." Despite the prefix this is **wallet recovery**, not the
chain-tip pipeline. Out of B8 scope.

---

## 2. Caller graph — the cut points (external to the legacy cluster)

"External" = files that survive B8 and call into the modules. These are where
edits land *before* any deletion.

| Caller file:line | Calls | Post-flip action |
|---|---|---|
| `config/src/boot.c:1005,1859,1900` | `legacy_bootstrap_import_blocking` | No longer present (flag branches removed). |
| `config/src/boot.c:1865` | `legacy_attach_outcome_name` | Removed with the importer branch. |
| `tools/rebuild_recent.c:499` | `legacy_bootstrap_load_height_map` | Confirmed as the surviving recovery reader. |
| `config/src/boot_services.c:452,455,465` | mirror `init`/`start`/`stop` | **KEEP** — heartbeat lifecycle. |
| `app/conditions/src/legacy_mirror_stuck.c:22,36,41,49` | mirror `stats_snapshot`, `request_catchup` | **KEEP** — Condition's detect/remedy/witness over the surviving monitor. (remedy calls `request_catchup`, which post-split must only re-probe, not apply blocks.) |
| `app/conditions/src/peer_floor_violated.c:81` | CAC `peer_floor_recovery_needed` | **REWIRE** to re-homed policy. |
| `app/conditions/src/local_header_refill_needed.c:61` | CAC `local_header_refill_needed` | **REWIRE**. |
| `app/services/src/snapshot_offer.c:503,520,552,566,575` | CAC `snapshot_offer_allowed` | **REWIRE**. |
| `app/controllers/src/sync_controller_blocks.c:543` | CAC `note_projection_deferred` | **REWIRE**. |
| `lib/validation/src/connect_tip.c:938` | CAC `note_projection_deferred` | **REWIRE** — surviving path; keep the deferral counter reachable. |
| `app/services/src/node_health_service.c:319,515` | CAC `get_status`, mirror `stats_snapshot` | **REWIRE** (CAC) / **KEEP** (mirror). |
| `app/supervisors/src/chain_supervisor.c:44` | mirror `stats_snapshot` | **KEEP** stats; force-promotion calls were deleted and replaced with named stall/revalidation-exhausted events. |
| `app/services/src/chain_tip_watchdog.c` | tip-stall escalation | **KEEP** watchdog; force-promotion call was deleted and replaced with a named stall event. |
| `app/controllers/src/health_controller.c:63,309,372,383,387,389,442` | mirror stats + CAC status/names/dump | **KEEP** mirror; **REWIRE** CAC. |
| `app/controllers/src/event_controller.c:258,262,264,266,315,352` | CAC status/names/dump + mirror stats | **KEEP** mirror; **REWIRE** CAC. |
| `app/controllers/src/cutover_controller.c:425,464,466,468` | CAC status/names | **DELETED** — cutover_controller is itself B8 apparatus (the cutover plumbing). |
| `lib/metrics/src/metrics.c:380` | mirror `stats_snapshot` | **KEEP**. |
| `tools/mcp/controllers/meta_controller.c:561,563` | mirror `reload_from_env`, `stats_snapshot` | **KEEP**. |
| `config/src/boot.c:637,1763,2423,2546,2737,3259` | utxo_recovery `backfill`/`prepare_reimport`/`import_ldb`/`restore_chain_tip`/`execute`/`clean_above_tip` | **REWIRE** (boot recovery, A-workstream); `import_ldb` DELETE-candidate; `clean_above_tip` → Condition. |
| `config/src/boot_services.c:2732` | CAC `init` | **REWIRE**. |
| `app/controllers/src/diagnostics_registry.c:421,449` | mirror + CAC dumpers | **KEEP** mirror; **REWIRE** CAC dumper to re-homed policy (keeps `zcl_state` working). |
| `app/controllers/src/projection_diff_controller.c:55` | mirror dump | **KEEP**. |

**Cut-point count (live, non-test, non-comment files):** ~16 distinct files.
Of these, the *hard* edits (must precede deletion) are: `boot.c` (flag branches),
`rebuild_recent.c` (extract height-map reader), `chain_supervisor.c` +
the CAC consumers that must
repoint at the re-homed policy.

---

## 3. Test pins

Registered in `lib/test/src/test.c` and `test_parallel.c` (decls in
`lib/test/include/test/test_helpers.h`).

| Test file | Group | Verdict |
|---|---|---|
| `test_legacy_bootstrap_attach.c` | `test_legacy_bootstrap_attach` (`test.c:813`) | **DELETED** — this test was import-only and is removed with the module. |
| `test_chain_advance_coordinator.c` | `test_chain_advance_coordinator` (`test.c:64,680`) | **MIGRATE** — the scoring-policy behavior survives (re-homed). Repoint at the new policy module; drop the force-promotion sub-tests (`:331,362`) and `mirror_repair_allowed` tests with the deleted window. |
| `test_lag_slo.c` | `test_lag_slo` (`test.c:854`) | **MIGRATE** — lag-SLO is the KEPT monitor behavior; keep, repointed at the surviving mirror monitor. |
| `test_legacy_mirror_stuck_condition.c` | `test_legacy_mirror_stuck_condition` (`test.c:438,779`) | **MIGRATE** — the Condition survives; ensure its remedy (`request_catchup`) still targets the monitor-only path. |
| `test_utxo_recovery_service.c` | `test_utxo_recovery_service` (`test.c:326,741`) | **MIGRATE** — boot-recovery behavior survives; `clean_above_tip` test moves to the new Condition test. |
| `test_integrity.c` | `test_integrity` (`test.c:529,686`) | **MIGRATE** — uses `classify_count_check` / `xor_mismatch` pure helpers; keep, repointed if those helpers move. |
| `test_zclassicd_oracle.c` | `test_zclassicd_oracle` (`test.c:473,852`) | **MIGRATE** — exercises the mirror monitor/oracle; KEEP behaviors. |
| `test_syncdiag_rpc.c` | `test_syncdiag_rpc` (`test.c:480,670`) | **MIGRATE** — calls `*_reset_for_test` on both CAC + mirror; repoint. |
| `test_node_health_service.c` | `test_node_health_service` (`test.c:78,669`) | **MIGRATE** — consumes CAC status + mirror stats; KEEP, repoint CAC. |
| `test_make_lint_gates.c` | (lint self-test) | **EDIT** — still checks `utxo_recovery_import_ldb`, `utxo_recovery_restore_chain_tip`, `chain_advance_coordinator_init` (`:1138,1279,1326,1394`). Remove or update these assertions when symbols move/delete. |

No new `.c` test files are pure DELETE beyond the already-removed `test_legacy_bootstrap_attach.c`.

---

## 4. Build-graph references

| Location | What it lists | Action on B8 |
|---|---|---|
| `Makefile:10` `APP_SRCS = $(wildcard app/$(d)/src/*.c)` | **Glob** — no explicit list | Deleting the `.c` auto-drops it. **No Makefile edit needed.** |
| `tools/scripts/file_size_ceiling_baseline.txt` (E1) | `chain_advance_coordinator.c 1355`, `legacy_mirror_sync_service.c 1487`, `utxo_recovery_service.c 1141`, `legacy_import.c 1105`, `sync_controller_catchup.c 1262`, `sync_controller_import.c 995` | **DROP baseline lines** for each file actually deleted/shrunk below 800. Shrink (mirror/CAC/utxo) → drop the line once under 800. E1 baseline can only shrink — every removal ratchets the gate forward. |
| `tools/scripts/one_result_type_baseline.txt` (E2) | `chain_advance_coordinator.c` (`:18`), `legacy_mirror_sync_service.c` (`:35`), `utxo_recovery_service.c` (`:46`) | **DROP** the line for any deleted file. `legacy_bootstrap_importer.c` no longer belongs here and the line is already absent in-tree. |
| `tools/scripts/supervisor_baseline.txt` | `legacy_mirror_sync_service.c` (`:33`) + comment "the alt source" (`:26`) | **KEEP** while the mirror monitor survives; the supervisor-child registration is part of the heartbeat. Do not drop. |
| `tools/scripts/typed_blocker_baseline.txt` | `legacy_mirror_sync_service.c`, `chain_advance_coordinator.{c,h}`, `legacy_mirror_sync_service.h` (`:26-29`) | **EDIT** — `lms_set_blocker()` string surface stays with the monitor (KEEP `legacy_mirror_sync_service.*`); drop `chain_advance_coordinator.{c,h}` lines when the typed-blocker classification table moves to the re-homed policy. |
| `tools/scripts/lib_layering_baseline.txt` | `metrics.c → legacy_mirror_sync_service.h` (`:20`); `activate_best_chain.c`, `connect_tip.c`, `process_block_core.c → chain_advance_coordinator.h` (`:83,94,107`) | **EDIT** — keep the mirror line (KEEP); repoint the 3 CAC include lines at the re-homed policy header (these are `lib/validation` → `app/services` layering exceptions; they move, not vanish). |
| E1/E2 doc-accuracy gate (E11) | `make lint` E11 cross-checks 29 gates vs Makefile/doc | Run `make lint` after baseline edits; E11 verifies doc and Makefile agree. |

There is **no explicit source list** anywhere for these files — the glob build
means file deletion is self-effecting; only the **baseline txt files** and the
**lint self-test** (`test_make_lint_gates.c`) need manual edits.

---

## 5. MCP / RPC / flag surface (would 404 after deletion)

| Surface | Names which module | Action |
|---|---|---|
| **Flag** `-cold-import[=path]` | `main.c:1456-1466` → `boot.c:1880` → `legacy_bootstrap_import_blocking` | Removed (no parse branch; ctx field removed). |
| **Flag** `-fastimport[=path]` | `main.c:1492-1502` → `boot.c:981`/`boot_step_fastimport` | Removed. |
| **Flag** `-legacy-attach[=path]` | `main.c:1474-1484` → `boot.c:1841` | Removed. |
| `config/include/config/boot.h:46,53,60` | `fastimport_from`, `cold_import_from`, `legacy_attach_from` fields | Removed. |
| **MCP `zcl_state subsystem=legacy_mirror`** | `diagnostics_registry.c:421` | **KEEP** — monitor survives. |
| **MCP `zcl_state subsystem=chain_advance_coordinator`** | `diagnostics_registry.c:449` + help text `:520` | **REWIRE** the dumper to the re-homed policy so the subsystem name keeps resolving (or rename the subsystem — but `zcl_status` depends on it, see next row). |
| **MCP `zcl_status`** | `tools/mcp/controllers/ops_controller.c:148` calls `dumpstate ["chain_advance_coordinator"]` | **HARD DEPENDENCY** — if the CAC dumpstate key is removed without a replacement, `zcl_status` (the daily-driver tool) silently loses its source field. Repoint to the re-homed policy's dumpstate key. |
| **MCP `meta_controller`** | `:561 reload_from_env`, `:563 stats_snapshot` (mirror) | **KEEP**. |
| `tools/mcp/metrics.{c,h}` (`:93`, `:205`) | mirror `lag_blocks` metric | **KEEP** (monitor metric). |

No dedicated MCP *tool* (`zcl_cold_import`, etc.) names these modules — the
import paths are CLI-flag-only, so retiring the flags is the whole surface there.
The RPC `dumpstate` method is generic; only the **string keys** `legacy_mirror`
(keep) and `chain_advance_coordinator` (rewire) matter.

---

## 6. Ordered deletion checklist

Rewire callers FIRST, delete second, drop baselines third, delete tests last,
rebuild. Each step independently `make lint`-clean and `test_parallel`-green.

**Phase A — `legacy_bootstrap_importer.c` wholesale (already complete):**
- [x] Extracted/retained `legacy_bootstrap_load_height_map` reader in `tools/rebuild_recent.c` (`:499`) while deleting importer.
- [x] Deleted `-cold-import` / `-fastimport` / `-legacy-attach` parse branches in `main.c` + `boot.c`.
- [x] Confirmed no spotcheck caller remains and deleted `legacy_bootstrap_spotcheck.c` as a delete-cascade.
- [x] Deleted `app/services/src/legacy_bootstrap_importer.c` + its header.
- [x] Deleted `lib/test/src/test_legacy_bootstrap_attach.c`; removed its decl (`test_helpers.h`) + registration (`test.c`).
- [x] Drop `file_size_ceiling_baseline.txt` line (`legacy_bootstrap_importer.c 1721`).
- [x] Update `test_make_lint_gates.c` pins (legacy importer pins removed; legacy boot strings no longer present).
- [ ] `make lint && ./test_parallel`. Est. **−600 (mirror block-apply half)** residual; importer delta is already reflected. 

**Phase B — `chain_advance_coordinator.c` rewire + force-window delete (gated on B7):**
- [x] Stand up the re-homed scoring policy (`block_source_policy.{h,c}`) for the pure source-selection/name/plan surface.
- [ ] Re-home or wrap the remaining surviving CAC surface (`peer_floor_recovery_needed`, `snapshot_offer_allowed`, `local_header_refill_needed`, `note_projection_deferred`, `get_status`, `dump_state_json`).
- [ ] Repoint consumers: `peer_floor_violated.c:81`, `local_header_refill_needed.c:61`, `snapshot_offer.c` (×5), `sync_controller_blocks.c:543`, `connect_tip.c:938`, `node_health_service.c:319`, `health_controller.c`, `event_controller.c`, `boot_services.c:2732`, `diagnostics_registry.c:449`.
- [ ] Repoint `zcl_status` MCP key (`ops_controller.c:148`) + the `zcl_state` dumper.
- [x] Delete force-promotion: `force_mirror_promotion` + `force_mirror_active`; drop calls in `chain_tip_watchdog.c`, `chain_supervisor.c`, and `mirror_repair_allowed`.
- [ ] Delete `chain_advance_coordinator.c` + header (or shrink to the moved surface).
- [x] Migrate `test_chain_advance_coordinator.c` (drop force/repair sub-tests).
- [ ] Update `test_make_lint_gates.c:1138` when the remaining CAC surface moves.
- [~] Drop/edit baselines: E1 ratcheted (`chain_advance_coordinator.c 1355`, importer line dropped), `typed_blocker_baseline.txt` repointed to `block_source_policy.h`; remaining E2 and `lib_layering_baseline.txt:83,94,107` wait for CAC shell deletion/repoint.
- [ ] `make lint && ./test_parallel`. Est. **−1714** (less the surface moved to the policy module).

**Phase C — `legacy_mirror_sync_service.c` block-apply removal (gated on B7) + `legacy_body_pull.c`:**
- [x] Confirmed in B7 rollout: stage pipeline is the authoritative writer (the mirror no longer needs to apply blocks).
- [ ] In `request_catchup`, delete the body-pull/activation block (`:893-:962`) and the `static` helpers `lms_run_activation_to_target`, `lms_drain_headers_to_target`, `lms_try_recover_stale_next_failed`, `lms_kick_local_pipeline`, `lms_next_block_needs_mirror_body`. Keep the monitor (fetch/verify/lag-SLO/observe/mark_success). The function becomes "probe + report lag," not "apply."
- [ ] Delete `app/services/src/legacy_body_pull.c` + header once the mirror is its last caller and that call is removed.
- [ ] Migrate `test_lag_slo.c`, `test_legacy_mirror_stuck_condition.c`, `test_zclassicd_oracle.c` to the monitor-only contract.
- [ ] Drop E1 baseline line for `legacy_mirror_sync_service.c` once it falls under 800; keep E2/supervisor/typed-blocker/lib-layering mirror lines (monitor survives).
- [ ] `make lint && ./test_parallel`. Est. **−529 (body_pull) − ~600 (mirror block-apply half)**; mirror file shrinks to a lean monitor.

**Phase D — `utxo_recovery_service.c` (gated on A-workstream re-homing boot recovery; only `clean_above_tip` + `import_ldb` are B8-adjacent):**
- [ ] Re-home `clean_above_tip` as a Condition (C3); repoint `boot.c:3259`; migrate its test.
- [ ] If cold-import lineage is fully retired (Phase A done), evaluate deleting `import_ldb` (`boot.c:2423`) — **UNCERTAIN**, confirm no recovery path still needs LDB import.
- [ ] Leave the rest (wipe/restore/execute/backfill/classifiers) until A re-homes boot recovery — **not a B8 tip-deletion concern.**

**Net LOC removed by B8 proper (Phases A–C, the "legacy tip" surface):**
≈ **~1100 (CAC net of moved policy) + ~600 (mirror block-apply) + 529 (body_pull)**. The brief's "6,059" over-counts because `connect_tip` (1087) and `legacy_import` controller (1105) are KEEP, and `utxo_recovery` (1141) is mostly KEEP/A-workstream.

---

## 7. Preserved-heartbeat callout (DO NOT DELETE)

**The legacy_mirror live-sync is LOAD-BEARING (MEMORY: "Both nodes under linger;
NEVER stop zclassicd; live-sync via in-process mirror"). These SURVIVE B8:**

- **Lifecycle:** `legacy_mirror_sync_init` (`:994`), `legacy_mirror_sync_start`
  (`:1121`), `legacy_mirror_sync_stop` (`:1143`) — wired at `boot_services.c:452-465`.
- **The heartbeat tick:** `lms_on_tick` (`:988`) and its registration. The tick
  calls `request_catchup("heartbeat")` every `cadence_secs` (default 3s).
- **The monitor half of `request_catchup`** (`:774`): fetch chain-info, lag
  computation, `lms_evaluate_lag_slo`, anchor/after-tip verification, hash
  caching, observe-local, mark-success. This is the lag-SLO truth that drives
  health/metrics/the stuck Condition. **Only the block-*application* half
  (body_pull + run_activation, `:893-:962`) is deleted.**
- **The full stats/introspection surface (18+ live consumers):**
  `legacy_mirror_sync_stats_snapshot` (`:1151`), `legacy_mirror_sync_dump_state_json`
  (`:1272`, the `zcl_state subsystem=legacy_mirror` dumper),
  `legacy_mirror_sync_reload_from_env` (`:1095`), the entire
  `struct legacy_mirror_sync_stats` + `struct legacy_mirror_sync_config`.
- **The Condition that heals it:** `app/conditions/src/legacy_mirror_stuck.c`
  (detect/remedy/witness) — KEEP; its remedy calls `request_catchup` (post-split
  this re-probes, no longer applies).
- **Baseline registrations that stay:** `supervisor_baseline.txt:33` (mirror is a
  registered supervisor child — the heartbeat liveness), `typed_blocker_baseline.txt`
  mirror lines (`lms_set_blocker` surface), `lib_layering_baseline.txt:20`
  (`metrics.c → legacy_mirror_sync_service.h`).

**Litmus test before deleting anything in `legacy_mirror_sync_service.c`:** if a
symbol is reached from `boot_services.c`, `health_controller.c`,
`node_health_service.c`, `metrics.c`, `chain_supervisor.c`, the diagnostics
registry, or `legacy_mirror_stuck.c` — it is the **monitor** and **KEEP**. Only
the internal body-pull/activation statics (no external caller) are deletable.

---

## Open UNCERTAINs (verify during execution, do not guess)

1. Resolved — `legacy_bootstrap_spotcheck.c` was removed with importer deletion.
2. CAC `chain_advance_coordinator_plan` — is `build_runtime_input`/`get_status`
   the de-facto live entry, or is `plan` truly test-only? Read `node_health_service.c:319`
   path before deleting `plan`.
3. `utxo_recovery_import_ldb` — does any non-cold-import recovery still need LDB
   import after Phase A? Confirm `activate_best_chain.c:867,875` are comments only
   (they are) and no live path depends on it.
4. The "6,059 LOC" headline — corrected to ≈3,900–4,000 deletable; the rest is
   KEEP (connect_tip, legacy_import controller) or A-workstream (utxo_recovery).
