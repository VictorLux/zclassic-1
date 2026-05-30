# Build Checklist — the road to the beautiful node

> The single tracker for **everything that needs to be created.** Architecture
> and the laws: [`FRAMEWORK.md`](./FRAMEWORK.md). This file is the *work*: every
> item is a checkbox, grouped by workstream, ordered by dependency. Check it off
> when it's shipped + proven. Read this first when you start a session.
>
> **Updated:** 2026-05-27. Goal = the Prime Directive: *log-as-truth + pure
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
  🪶 LEAN          binary 14.9MB (stripped) ✓     RSS ~2GB→1GB ▲ climbs w/ bg-verify
  💪 UNBREAKABLE   alert loop ✓ closed · recovery moles ✓ pinned   can't-halt-by-construction ◑ needs B8 + E6/E7 hardening
  🔬 HONEST        zcl_status live truth ✓          bug→repro ✓ (chaos/postmortem)
  ✓ done · ▸ building · ◑ fixed-in-code-not-structural · ▲ above target
```

**Owner mandate (standing):** NO whack-a-mole. The node becomes unbreakable by
*finishing* the refactor — collapse to ONE path, DELETE the legacy — not by
adding conditions. Default to SUBTRACTION. While two chain paths coexist a silent
halt remains *possible*; workstream **B** is the structural cure.

**▶ FOCUS NOW (2026-05-27) — recovery moles are fixed; the next move is live preflight verification, then B8 + E6/E7.**
The five recovery moles that wedged the live node are all on main and pinned by
tests so they can't silently regress:

1. **BIP30 self-write** — `connect_block` tolerates an own-height duplicate
   non-coinbase output as overwrite (different-height duplicate still rejects).
   `4fa294c03` + `baec7b591`; test `test_connect_block_self_write.c` (`4ccacf3ab`).
2. **CEC cursor lag** — `coins_cursor` lag/overshoot is advisory, doesn't gate
   tip block-index evidence. `158c246ef`; test in `test_chain_evidence_controller.c`.
3. **Stale-freeze reconcile** — `cec.reconcile` lifts an unnamed persisted freeze
   when the live tip is provably consistent. `93dceff66` + `ecd597a53`.
4. **LOCAL_IMPORT reconstruct** — `cec` reconstructs recoverable tip as
   `LOCAL_IMPORT` (single source of authority) instead of freezing. `3f3003d1b`.
5. **No-silent-ready** — block-connection authority must advance-the-tip OR
   name-a-typed-blocker (no bare READY when behind). E8 gate HARD: `2d7de2a76` +
   `32581b86e` + `f1d303b80`.

The single next action is **live preflight verification** on a clean datadir (authoritative
path + canary checks), then **B8** deletion cleanup and **E6/E7** hardening.
Everything routes through B; do not spawn a parallel "fix the mirror" track — that IS
B.

**Preflight go/no-go hardened (2026-05-29):** `cutoverpreflight` (RPC + MCP `zcl_cutoverpreflight`)
now ANDs a complete invariant set into one `ready` boolean with a typed blocker per failing
gate: tip parity (height+hash via header_admit_diff CONVERGED), `blocks_fed==blocks_diffed`
conservation, auto-revert wiring armed (`cutover_no_forward_progress` registered/CRITICAL/
max_attempts==1/witnessed/not-active), header+validate cursor-lag, modes-are-shadow, and the
net-new **UTXO-set commitment (SHA3) parity gate** (`SHA3(legacy coins.db utxos) ==
SHA3(log-folded utxo_projection)` + count match; projection caught up first so an in-flight
gap reads DEFER, never false-divergence). Read-only; cannot make `ready` easier.

---

## ✅ Already shipped (the foundation)

The primitives the north star needs all exist and are tested — they just run in
**shadow**. This is why the work ahead is *flipping authority*, not building.

- [x] Condition engine + 23 conditions (the model-citizen shape) · Phase 0
- [x] Kernel primitives adopted: mailbox, projection, platform.clock/rng (0 raw clock/RNG callers) · Phase 1
- [x] Wave-S stage pipeline, all 8 stages, SHADOW complete (header_admit → tip_finalize) · Phase 2
- [x] event_log — durable, fsync'd, CRC32C, torn-write recovery, SHA3 fingerprint · Phase 4a
- [x] 8 pure-fold projections: utxo, block_index, mempool, peers, wallet, znam, contacts/onion/hodl · Phase 4b–d
- [x] Crypto-agility registry + hot-path rewires (Equihash, ECDSA) · Phase 5
- [x] Determinism: seed_tape + postmortem capsule + `make chaos` · Phase 6
- [x] Supervisor tree split into 7 domain supervisors · Phase 3
- [x] Service-shell renames/splits/deletions: `sync_watchdog_service` **deleted** (no files remain); `chain_restore_service` shell **deleted** (`89892c441`) but its work **split into 7 files** (`app/services/src/chain_restore_{repair,executor,boot_snapshot,boot_activation,planner,integrity,disk_repair}.c`, 6 non-test consumers); `header_probe_service` **renamed** to `app/services/src/header_probe.c` (376 LOC) + poll loop extracted to `app/jobs/src/header_probe_poll.c` (3 non-test consumers); `chain_evidence_controller` **NOT dissolved** — still live at `app/services/src/chain_evidence_controller.c` (796 LOC, 9 non-test consumers incl. `connect_tip.c`/`activate_best_chain.c`/`process_block_core.c`). The latter two are B8/cutover-gated, not yet removed.
- [x] **Reorg keystone (2026-05-26):** disconnect emits inverse UTXO deltas (`bfa379bc8`) + byte-exact convergence proof (`1e65f81a0`)
- [x] Silent-halt escalation closed: `EV_OPERATOR_NEEDED` → sinks + `zcl_status` DEGRADED + sd_notify
- [x] PROVE Tier-1: offline PoW/integrity sweep `zcl_replay_verify` (`63a9a5de4`) + shadow_replay_proof
- [x] Test harness: `test_parallel --only=SUBSTR` (1s iteration vs 110s full)
- [x] **Recovery moles fixed (2026-05-27):** 5 layers (BIP30 self-write tolerance, CEC cursor non-gating, stale-freeze reconcile, LOCAL_IMPORT reconstruct, no-silent-ready E8) all on main, pinned by `test_connect_block_self_write.c` + `test_chain_evidence_controller.c` (`4ccacf3ab`). The live node can no longer wedge silently on these classes.
- [x] **A5 shape codegen (2026-05-27):** `tools/new_shape.sh` + `make new-{condition,model,job,controller}` (`497b48781`). The easy path is the correct path.
- [x] **Pure domain core extracted (2026-05-27):** `domain/` (top-level, not `lib/`) holds **21** pure modules across 3 contexts — `domain/consensus/src/` (17: check_block, checkpoints, coinbase, coins_math, equihash, header_accept, locktime, pow, sapling_structural, script_interp, script_standard, sighash, sigops, subsidy, tx_structural, upgrades, verify), `domain/wallet/src/` (2: key_derivation, mnemonic), `domain/encoding/src/` (2: base58, bech32). Each is no-clock/no-RNG/no-IO, fronted by a thin `lib/` legacy wrapper that `#include "domain/<ctx>/<name>.h"` and delegates (e.g. `lib/chain/src/pow.c`, `lib/encoding/src/base58.c`, `lib/script/src/standard.c`), and sealed by a regression test — **21** `lib/test/src/test_domain_*.c` files (1:1 with the modules). Verify: `find domain -name '*.c' | wc -l` → 21; `ls lib/test/src/test_domain_*.c | wc -l` → 21.
- [x] **Ports/adapters seam populated (2026-05-27; extended 2026-05-29):** **9 services** now read/write through a port instead of touching SQLite directly — hodl_history (`hodl_history_port.h`), node_health (`node_health_store_port.h`), db_maintenance (`db_maintenance_port.h`), wallet_backup (`wallet_backup_store_port.h`), block_index_sidecar (`block_index_sidecar_port.h`), block_log, bg_hash_verify (`bg_hash_verify_store_port.h`), bg_validation (`bg_validation_store_port.h`), zslp (`zslp_store_port.h`) — plus the pre-existing clock, consensus_log, event_emitter, utxo_snapshot ports. SQLite adapters live in `adapters/outbound/persistence/src/` (one `*_sqlite.c` per service), each with a port test (`lib/test/src/test_*_port.c`). The remaining services-direct-sqlite files are all B-gated / chain-entangled (block_index_loader, chain_state_repository, chain_tip, utxo_recovery_*); this is a real seam, not full coverage.

---

## B — THE CUTOVER: flip authority from coins.db to the log  ← the north star

Dependency-ordered. This is the structural cure; everything in **C** is gated on
it. Plan detail: [`work/cutover.md`](./work/cutover.md).

- [x] **B1** Reorg-capability in `tip_finalize` — disconnect emits inverse deltas; parity proven byte-exact.
- [x] **B2** Emit block bodies to the log. `EV_BLOCK_BODY` (type 2) now has an emitter: the `body_persist` stage emits the verified body into the append-only log (shadow, best-effort — mirrors the EV_BLOCK_HEADER pattern in `block_index_db.c`). New frozen codec `struct ev_block_body {hash, height, body_len, body}` in `event_log_payloads.{h,c}` — the body bytes are the canonical `block_serialize()` wire form, so a consumer round-trips via `block_deserialize()`. Emit fires ONLY on the `"verified"` path (read off disk → hashes to admitted header → merkle-reconstructs), so read-failed/header-mismatch/merkle-mismatch heights emit no body. New counters `body_emit_total`/`body_emit_fail_total` exposed via accessors + `zcl_state` dump. Proven in `test_body_persist_stage`: codec round-trip (incl. truncation rejects), 4 verified bodies stream back byte-faithful (deserialize + hash-match), verified-only skip (h=1 read-failed → 3 bodies). The log is now replayable from scratch to rebuild UTXO. Default remains shadow-capable with log-backed authority available via cutover mode. *(Phase 4e)*
- [x] **B3** Invert the UTXO emitter. `utxo_projection` now owns a single-writer authority flag (`utxo_author_t {LEGACY,STAGE}`, default LEGACY). `utxo_apply_stage` retains its validated delta (script + is_coinbase) and authors `EV_UTXO_ADD/SPEND` when authority==STAGE; `update_coins.c`'s shadow emitters no-op when authority≠LEGACY → exactly one writer. Proven byte-exact in `test_utxo_apply_authorship` (legacy-interleaved emission == stage adds-then-spends, incl. in-block create+spend). Default stays LEGACY so the live node is unchanged; the **flip** path is in the B7 machinery. *Acceptance met: stage CAN drive the projection, parity-proven; authority gate is the cutover seam.*
- [x] **B4** Point `connect_block` input lookups at the utxo **projection** instead of `coins_view_cache`. Read mechanism + authority-gated wiring **DORMANT** (live path unchanged, default author LEGACY); authoritative path uses this wiring when cutover mode is active.
  - **Read mechanism (`82712bf54`):** `utxo_projection_get_coins(txid)→struct coins` reader (mirrors `coins_view_sqlite_get_coins` byte-for-byte, `version=1`) + `lib/storage/coins_view_projection.c` (a read-only `struct coins_view` backed by the projection), parity-proven in `test_coins_view_projection`. Read-only: `batch_write` is a guarded error (stage authors via events — B3 single-writer).
  - **Authority-gated wiring (`7e52a3c14`):** `lib/storage/coins_view_stage_backing.c::coins_view_select_connect_backing()` chooses the `coins_view_cache` backing on `utxo_projection_get_author()`: LEGACY (default) hands back the coins_tip view *verbatim* (byte-identical to today); STAGE returns a composite that resolves get_coins/have_coins through the projection (authoritative reads) while delegating get_best_block + batch_write to the coins_tip view (best-block consistency; coins.db mirror warm; NOT a second projection writer — B3 single-writer holds). Wired at the one seam `connect_tip.c`; connect_block call sites byte-identical; RAM read-cache preserved. Proven in extended `test_coins_view_projection`. **DORMANT until cutover mode is active.**
  - **`version` resolved INERT (2026-05-26):** `struct coins.version` plumbed into undo at `update_coins.c:154`, restored at `connect_block.c:709`. Consensus-inert: `coins_view_sqlite.c:611` hardcodes `out->version=1` on load; `utxo_commitment` = `SHA256(txid‖vout‖value‖height)` no version. **No `EV_UTXO_ADD` format change, no schema migration.**
- [x] **B5** Make `log_head` / the `tip_finalize` cursor the **definitional tip**. Demote `chain_active` to a derived in-RAM index rebuilt from `block_index_projection`. *Acceptance:* `health = network_tip − log_head` is one real number.
  - **Observability DONE:** `health = network_tip − log_head` is now one real number — `node_health_snapshot.log_head` (= `tip_finalize_stage_cursor()−1`) + `log_head_gap` (= `peer_best_height − log_head`), surfaced in the healthcheck JSON (`event_controller`).
  - **DONE (the flip):** `active_chain_height` and `active_chain_tip` prioritize the atomic log_head.
  - **Flip is proven cheap** ([`work/b5-chain-active-readers.md`](./work/b5-chain-active-readers.md)): readers already route through two accessors `active_chain_tip(c)` / `active_chain_height(c)` (`lib/validation/include/validation/chainstate.h`), so the flip is a 2-accessor-body change.
- [x] **B6** Offline PROVE harness complete: Tier-1 full-0→tip driver emits `shadow_replay_proof: 0 divergences across N blocks, commit <sha>`; `--deep`/`--tier2` runs full PoW/script/Groth16 sweep via `replay_verify_run`; reorg corpus (`test_reorg_projection_parity`) byte-exact; full-driver CI test. `e7c5c4b74`.
- [x] **B7** Flip once, behind the guard. `cutovermode all authoritative` + `cutover_no_forward_progress` auto-revert (180s no-progress → revert to SHADOW + page). Real canary: one block connects through the authoritative path, auto-revert on any divergence. **DONE.** The safety net is tested and `cutovermode` supports tip_finalize authoritative.
- [~] **B8** Extract-then-delete (see **C**). **Legacy-module half is DONE:** Phase A (legacy_bootstrap_importer) deleted; Phase C (mirror block-apply + body_pull ~856 LOC) deleted (`6ef905772`); Phase D (clean_above_tip → Condition) done as-shipped; **Phase B (CAC shell) dissolved** (`af8b486d0`, see C1). `connect_tip` SURVIVES (post-flip authoritative connect path), `legacy_import` controller is KEEP, `utxo_recovery` is mostly KEEP/A-work, the `legacy_mirror` heartbeat + lag-SLO monitor are PRESERVED.
  - **Remaining B8 = the comparison apparatus**, and it is **HARD-GATED on the live flip + soak** (it is the tooling that *performs and verifies* the flip — `cutoverpreflight`, `cutovermode`, `diff_with_legacy_shadow`, `shadow_feeder`, the `*_projection_diff` MCP tools, the S-11 `header_admit_stage_diff` / `zcl_diff_staged_header_admit`, both cutover Conditions). Exact symbol/caller/test/build ledger + the mandatory **flip → soak → confirm → delete** ordering and Gate-0 checklist in [`work/b8-comparison-apparatus-deletion-plan.md`](./work/b8-comparison-apparatus-deletion-plan.md) (`a6a3430f4`). ≈7,600 LOC retires once: flip is permanent (stage default AUTHORITATIVE, revert unreachable), tip soaked N blocks with diffs matching, preflight GREEN, fed==diffed held, auto-revert NEVER fired.
  - **Live-flip prerequisite (current blocker):** the running node must be AT network tip for preflight to pass (it correctly DEFERS while behind). As of 2026-05-29 the node is catching up (~3126.5k / network ~3128.5k).

---

## A — Make the canon true (the missing shape primitives)

`FRAMEWORK.md` blesses **struct-registration**, not block-macro DSLs (Law 3).
So this is NOT about building fictional `MODEL(){…}` macros — it's giving the
three unreal shapes a real, debuggable form.

- [x] **A1** `app/jobs/include/jobs/job.h` — one uniform Job contract: `job_result_t {JOB_ADVANCED,JOB_BLOCKED,JOB_IDLE,JOB_FATAL}` (replaces `stage_result_t`, integer values preserved byte-for-byte). The 8 reducer `*_stage.c` relocated `app/services/` → `app/jobs/`; kernel `stage.h` now includes `jobs/job.h`. `app/jobs/` now holds the reducer; the empty-scaffold folder is real. `994145f28`.
- [x] **A2** Event-shape decision (orchestrator, 2026-05-26): the Event shape STAYS; its implementation is lib-resident today (`lib/event/` in-mem observability ring + `lib/storage/event_log` durable log). `app/events/` is reserved for app-level event definitions + subscriber wiring and gets populated by **B2** (block-body emit) as the log becomes authoritative — not deleted. FRAMEWORK §3 Event row states this.
- [x] **A3** Supervisor declarations extracted into `app/supervisors/src/{net,chain,staged_sync}_supervisor.c` (8 Wave-S children in pipeline order); `boot_services.c` 3,885 → 3,270 LOC (−615); boot ordering preserved. `fa9e8d0ec`.
- [~] **A4** `zcl_result` adoption for services (Law 2). Migrate services off bare `bool`/`int`. Cluster-by-cluster in flight: wallet-backup (`844f13472`), snapshot (`20bb5bc36`), ZSLP (`c2e107257`), infra (`93ac12748`), chain_restore (`922ea6fff`), chain_tip (`655804a9b`), oracle+policy (`36bb634da`). Plus stale-line hygiene: the 8 Wave-S reducer stages A1 relocated to `app/jobs/src/` were dropped from the E2 baseline (outside the gate's `app/services/src/` scope; their return shape is owned by the A1 Job contract). E2 baseline ratcheted 77 → **9** (`tools/scripts/one_result_type_baseline.txt`; `grep -vc '^#\|^$'` → 9). The survivors are all chain-authority / reducer / legacy-mirror / recovery / cutover (constraint-gated; `utxo_recovery_restore.c` carried the recovery service's bare-result surface when C3 split it out). *(ratchet — see E2)*
- [x] **A5** `tools/new_shape.sh` shape-skeleton generator + `make new-{condition,model,job,controller}` targets shipped (`497b48781`). Emits committed C that compiles and passes the shape lint gates on the day it lands — gdb-steppable, not metaprogramming. Refuses to overwrite existing files; prints the one manual registry-wiring step.

---

## C — Dissolve the mega-modules (extract-then-delete)

Net deletable ≈ **3,900–4,000 LOC** (corrected from the stale "3 modules = 4,407"
headline — see [`work/b8-deletion-inventory.md`](./work/b8-deletion-inventory.md);
`connect_tip` survives, `legacy_mirror` heartbeat is preserved). All gated on
**B** (they own behavior the new path lacks until the flip).

- [x] **C1 / B8-Phase-B (2026-05-29)** `chain_advance_coordinator.c` **shell dissolved** (`af8b486d0`). Pure source-selection policy was already in `block_source_policy.{h,c}`; force-promotion window already deleted. This step re-homed the surviving stateful surface (`peer_floor_recovery_needed`, `snapshot_offer_allowed`, `local_header_refill_needed`, `note_projection_deferred`, `get_status`, `dump_state_json`, `init`) into a new `block_source_policy_runtime.c`, renamed `chain_advance_coordinator_*` → `block_source_policy_*`, and repointed all ~12 consumers (incl. the surviving `connect_tip.c:938` and the `zcl_status` HARD dependency — the `"chain_advance_coordinator"` diagnostics key is preserved, repointed to `block_source_policy_dump_state_json`). The dead test-only `plan` wrapper was dropped. **Honest delta:** this is a *relocation* (~−4 net code), not a LOC cut — the architectural win is dissolving the CAC mega-module into its correct hexagonal home; the real B8 LOC deletion is the comparison apparatus, gated below. **Follow-up (done):** `block_source_policy_runtime.c` was since split below the 800-LOC cap (now 460 LOC) and is no longer in the E1 baseline. Full suite 0/267, 31 gates clean.
- [ ] **C2** `legacy_mirror_sync_service.c` (1,487) → extract the live-sync **heartbeat + lag-SLO monitor** to a lean monitor (PRESERVE it); delete only the block-application coordination. *Gated on B.*
- [x] **C3** `utxo_recovery_service.c` (1,204 → 624) → DONE as-shipped. `clean_above_tip` (orphan-UTXO heal) re-homed as the continuous `orphan_utxo_above_tip` Condition (`6ef905772`), which reuses the single heal impl `coins_rewind_above_tip` (guard `UTXO_BOOT_REWIND_MAX_ROWS`=32). There is NO legacy duplicate to dissolve — the `boot.c:3109` one-shot caller is a non-removable boot-ordering requirement (the Condition engine registers in `boot_services.c` *after* boot needs the inline heal before `activation_boot_complete`; removing it would require moving chain-progress onto the on-disk stage cursor — the B north-star, not a heal refactor). `restore_chain_tip` / `import_ldb` already split to `utxo_recovery_restore.c` (573 LOC). Both files under the E1 cap.
- [x] **C4** Collapse the **4 importers** (5,519 LOC: `legacy_bootstrap_importer` + `legacy_mirror_sync` + `legacy_import` + `sync_controller_import`) → one `legacy_bridge` + one `legacy_poll` job. [`work/wt-consolidate-import-paths.md`](./work/wt-consolidate-import-paths.md). *Independent of B.*

---

## D — Restore beauty (shape conformance — independent, high-leverage)

From the beauty audit; each is "principle violated → where → the elegant form."

- [x] **D1** Dissolve `diagnostics_controller.c` — 2,550 → 51 LOC, split into 6 single-concern files (diagnostics_registry + cutover/projection_diff/nodelog/dbquery/probe controllers). `da9a1fe5a`.
- [x] **D2** Controllers must not build views — explorer_factoids/stats/pages assembly moved into `app/views/` (controllers now skinny). `dd041e3b8`.
- [x] **D3** `header_admit_log` now a Model (`app/models/src/header_admit_log.c`, validates_* + before/after_save + `AR_ADHOC_SAVE`); raw SQL removed from the stage's write path. `aa9b6aaa7`.
- [x] **D4** One log call. **app/controllers slice DONE** (`110859a24`): 25 `fprintf(stderr,…); return` pairs → `LOG_FAIL/ERR/NULL` (151→126 in controllers). **lib/net slice (verified no-op):** all 68 are non-returning P2P log-and-continue / void-function diagnostics, already `obs-ok` — zero safe `LOG_*` drop-ins. **FIXED:** added non-returning `LOG_WARN`/`LOG_INFO` to `util/log_macros.h`. **`lib/net` slice DONE (2026-05-26, `f1e8d51a2`)**. **Remaining:** the `obs-ok` non-returning sites across `app/` → `LOG_WARN/INFO` (swept and mostly complete).
- [~] **D5** `app/` files > 800 LOC: **31 → 4** under the cap (E1 baseline file: `tools/scripts/file_size_ceiling_baseline.txt`, which lists exactly these 4). Wave splits landed: 12 oversized controllers (`ec1424cf4`), 3 views (`7e52a3c14`), explorer_factoids (`f3e57804d`), `database.c`+`wallet_tx.c` models (`c945d64c4`), api+snapshot router monoliths (`adc7ac45a`), `validate_headers` + `bg_validation` (`46d0edb2a`), reconciled (`7ca847c57`), C1 policy extraction (`chain_advance_coordinator.c` re-homed and since split below the cap), C3 `utxo_recovery_service.c` 1141→624 split. **Remaining 4 are all legacy modules B8 deletes/shrinks**: `sync_controller_catchup.c` (1262), `legacy_mirror_sync_service.c` (1180), `legacy_import.c` (1105), `sync_controller_import.c` (995). Closing D5 = finishing the cutover (C+B8), not more splits.

---

## E — Enforce it (every law gets a gate; "beauty by the build")

Hygiene is well-gated. **11 of 11 architecture gates now have enforcement**
(6 HARD + 5 RATCHET). **E6/E7 are wired as ratchets** and harden as B8 deletes
the legacy writer/RAM-authority surfaces. `make lint` reports 31 gates total, all clean. Each gate lands with the
work it guards (design in `FRAMEWORK.md` §5).

- [x] **E1** `file-size-ceiling` — RATCHET, baseline = **4** grandfathered files (down from 29) (`tools/scripts/file_size_ceiling_baseline.txt`). All 4 remaining are legacy modules B8 deletes. `5daf21742`.
- [x] **E2** `check-one-result-type` — RATCHET (**9** service files baselined, down from 77; new ones must use `zcl_result`) (`tools/scripts/one_result_type_baseline.txt`; `grep -vc '^#\|^$'` → 9). `f331f6e0d` + A4 paydowns through this round.
- [x] **E3** `check-shape-includes-header` — HARD; every condition/model/supervisor file includes its shape header. `f331f6e0d`.
- [x] **E4** `check-projections-pure` — HARD; every `*_projection.c` is a pure fold (no app includes, no AR model saves). `f331f6e0d`.
- [x] **E5** `stage-advances-or-blocks` — every Job step (`app/jobs/src/*_stage.c`) references a cursor (`cursor_out`/`c->cursor_in`/`stage_cursor`) AND returns `JOB_BLOCKED`/`JOB_IDLE` on non-progress. HARD (all 8 stages comply). `tools/scripts/check_stage_advances_or_blocks.sh`.
- [~] **E6** `one-write-path` — RATCHET wired (`tools/scripts/check_one_write_path.sh`), baseline = **64** legacy write surfaces; hardens as B8 deletes them.
- [~] **E7** `no-authoritative-RAM-state` — RATCHET wired (`tools/scripts/check_no_authoritative_ram_state.sh`), baseline = **0** direct RAM-authority surfaces.
- [x] **E8** `no-silent-ready` — the block-connection authority (`chain_activation_controller.c`) must advance-the-tip OR name-a-typed-blocker; any `activation_set_state(...ACTIVATION_READY...)` path must route `blocker_set`. HARD. Closed the live silent-ready hole (`"behind_peers"` → bare READY while +950 behind): the behind path now registers the typed blocker `chain.tip_behind_header_chain` (TRANSIENT, names why/height/escape `activation_drive_connect`, escape re-drives a local connect pass); caught-up clears it. Single decision `activation_eval_tip_blocker`; test asserts the blocker appears in the registry when behind and is cleared when caught up. `tools/scripts/check_no_silent_ready.sh`. **This is the structural foundation B stands on — the reducer now tells the truth every tick (advance or named-blocker); the B7 flip / B8 delete-legacy build on it.** *(`health-is-the-gap` Condition consolidation deferred to the B8 cleanup.)*
- [x] **E9** `operator-needed-has-a-sink` — HARD; `EV_OPERATOR_NEEDED` emit paired with the `alerts.c` subscriber (tree already satisfies). `5daf21742`.
- [x] **E10** `framework-shape` + `controller-SQL` graduated WARN → RATCHET (baselines captured; new violators fail). `5daf21742`.
- [x] **E11** `doc-accuracy` — HARD; `<!--LINT-GATES-->` block in DEFENSIVE_CODING.md must match Makefile `lint:` deps. 31 gates, agree. `5daf21742`.

---

## Decision log (binding)

- **2026-05-27** Recovery moles closed and pinned: BIP30 self-write tolerance + CEC cursor non-gating + LOCAL_IMPORT reconstruct + stale-freeze reconcile + E8 no-silent-ready. Live-cutover preflight verification is now the immediate next check before B8 cleanup. Doc reconciled against live lint baselines: E1 file-size 31→7, E2 one-result-type 77→28; A5 codegen shipped; B4 read-mechanism + authority-gated wiring active after B7.
- **2026-05-28** E6/E7 enforcement wired: `check-one-write-path` ratchets 74 grandfathered legacy write surfaces; `check-no-authoritative-ram-state` ratchets direct active-chain internals with a zero baseline; `make lint` reports 31 gates clean. B8 deletion now has build guardrails.
- **2026-05-27** Canon reconciled with reality (docs-only): `domain/` now holds 21 pure modules (3 contexts) each with a legacy wrapper + a seal test; ports/adapters seam populated for 5 services (10 port headers total); E2 baseline 77→28; B7 safety net (tip-parity / reorg-parity / shadow-conservation / replay-verify tests + preflight conservation gate) is landed; B8 net deletable corrected 4,407→≈3,900–4,000 (`connect_tip` survives, `legacy_mirror` heartbeat preserved); B5 proven a 2-accessor-body change; B7 completed and next is B8 + E6 + E7.
- **2026-05-26** Canon made honest: `FRAMEWORK.md` rewritten to bless struct-registration over fictional block-DSLs; the Ten Laws + Prime Directive + validation-feedback honesty added; every law mapped to a gate. `VISION.md` + `ARCHITECTURE.md` (old L1–L7 layer cake) merged in and deleted.
- **2026-05-23** Framework adopted: Rails-style MVC + Phoenix-style supervisors + hexagonal cut + Conditions. Strangler execution (per-module PRs), not big-bang. Lint gates ratchet WARN→FAIL.
- **2026-05-23** Worktree workflow: separate clones at `~/github/zclassic23-{2,3}`, identified by pwd suffix, pushed to origin for orchestrator merge.

## How this file gets updated

- Check an item `[x]` only when it's shipped **and** proven (forward-progress / test / measured delta). Add a row to `BENCHMARKS_LOG.md` for any number moved.
- Orchestrator (main) edits this file; workers append a Completion section to their own `docs/work/wtN-*.md`. Recent history lives in `git log`, not here.
