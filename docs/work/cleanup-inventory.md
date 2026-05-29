# Cleanup Inventory — Execution Plan

**Source:** 126 adversarially-verified findings (confirmed=true) deduplicated to **119 distinct actions** across **30 batches**.
**Net removable:** ~**-2,640 LOC**. Batches are grouped so **no two batches edit the same file** — they can run in isolation.

## Execution progress (2026-05-29)

- **[x] Wave A — dead-code deletions (6 batches): DONE + GREEN.** `dead-net-compat-header`,
  `tools-dead-shell`, `validation-audit-header-dead`, `dead-files-crypto`, `dead-msm-sapling`,
  `dead-net-modules`. 7 files deleted, ~1,272 deletions / 104 insertions. Node builds; `test_parallel`
  0/268 groups failed (63s). Uncommitted. The `validation_audit.h` matrix content was migrated to
  `docs/validation/VALIDATION_MATRIX.md` (not lost).
- **[x] Wave B — dedup/hygiene (7 batches): DONE + GREEN.** `rpc-dead-and-magic`, `lib-misc-slp-znam-dedup`,
  `adapters-hygiene`, `util-dead-metrics-health`, `domain-consensus-dedup`, `wallet-keys-dedup`,
  `chain-mmr-mmb-dedup`. 2 new shared headers (`op_return_push.h`, `reject_out.h`); duplicate helpers
  folded (printf→LOG_*, magic 8232/100 named, equihash params unified, MMR/MMB bag/append folded).
  Integration fixed 2 breaks: nested `/*` in an equihash comment, and an unguarded duplicate
  `MAX_BLOCK_SIZE` (now `#ifndef`-guarded in consensus.h). Node builds; `test_parallel` 0/268 failed (72s).
- **[x] Wave C-0 (10 batches) + C-1 (5 batches): DONE + GREEN.** controllers, jobs, models, services ×2,
  conditions, consensus-adj, net-hygiene, tests-helpers, validation-connect-tip (C-0); storage-projection-
  helpers, mcp, util-time, views, dead-validation-checkqueue (C-1). New shared modules: `stage_helpers.h`,
  `sha3_sidecar_io.{c,h}`, `projection_util.h`. Two workflow-caught build breaks fixed (`/*`/`*/` in a
  comment ×2); E6 one-write-path baseline line-numbers refreshed twice (writer set identical 64→64,
  multiset-proven — not a ratchet change); E4 projection-purity gate still HARD-passes. `test_parallel` 0/287.
- [ ] **root-config-cli-flags-includes — DEFERRED (1 batch, ~-150 LOC).** It edits `main.c`, which holds the
  uncommitted `--importblockindex`/`--mintutxocommitment` foundation that must NOT be committed until its
  FATAL-verify security gate lands. This last batch will land together with that work.

**Total committed:** 28/29 batches, 3 commits (`400dd42bf`, `04516d837`, `37128da64`), net **−2,790 LOC**
(219 files, +2,589/−5,379), 12 dead files deleted, 6 shared modules created. All green; node builds.

## How to read this
- `auto` = trivial, mechanical, zero behavior change (proven-dead deletion, unused include, exact-duplicate fold). Anything needing judgment is **not** auto.
- Run in `recommended_order` (safest-first, minimizes shared-file contention).
- Several batches **co-own** a small number of hub files (`test_helpers.h`, `time_compat.h`, `boot_services.c`, `scheme_equihash_200_9.c`). The co-ownership is resolved by assignment + ordering — see each batch's ordering note. **Do not parallelize co-owning batches.**

---

## Batches

### Pure deletions (safe first)
- [ ] **dead-net-compat-header** — `lib/net/include/net/compat.h` — **-75** — trivial — ✅ auto
  Never-included 75-line Windows shim.
- [ ] **tools-dead-shell** — `tools/lint/rewire_platform_clock.sh`, `tests/docs/test_power_node_contract_spec.sh` — **-78** — trivial — ✅ auto
  Completed one-shot migration + superseded shell spec test (C test still runs).
- [ ] **dead-files-crypto** — sha3_crypt/sha256/sha512/sha3 + crypto_registry — **-120** — trivial — ⚠️ judgment
  Dead encrypt/decrypt + 4 `_reset` aliases + make `keccakf` static + comment/doc fixes.
- [ ] **dead-msm-sapling** — msm_parallel / groth16_prover / bls12_381 — **-290** — low — ⚠️ judgment
  Dead g1/g2 parallel MSM (~215 LOC), abandoned final-exp block, FFT helper dedup. Run pairing tests.
- [ ] **validation-audit-header-dead** — `validation_audit.h`, `docs/SYNC.md` — **-194** — trivial — ⚠️ judgment
  Move 194-line doc header to `docs/`, repoint SYNC.md cross-ref.

### Net subsystem
- [ ] **dead-net-modules** — secure_channel, msgprocessor_sync, msgprocessor — **-340** — low — ⚠️ judgment
  Delete secure_channel.c/.h, dissolve 1-liner dispatch shims, collapse BIP37 handlers. *Co-owns `test_helpers.h` — run before tests batch.*
- [ ] **net-hygiene-dedup** — connman, onion_service, https_server.h, msg_blocks, boot_services, msg_version, fast_sync, network_controller — **-48** — low — ⚠️ judgment
  Drop redundant externs, add https decls, dedup IPv4-mapped encoding (6 sites), reuse ReadLE32/64. *Co-owns `boot_services.c` — serialize with root-config.*

### Validation
- [ ] **dead-validation-checkqueue** — checkqueue + 3 test files — **-260** — low — ⚠️ judgment
  Dead module (workpool replaced it); delete 3 test blocks + cq_check_* helpers. *Co-owns `test_helpers.h`.*
- [ ] **validation-connect-tip** — connect_tip, process_block_self_heal, process_block_internal.h — **-95** — low — ⚠️ judgment
  Dead branch, hex dedup, named constant, scan-loop extraction (cohesion), `expand -t4`.

### Controllers / views / models
- [ ] **controllers-dead-and-hex** — wallet/swap/messaging/file_market/projection_diff/explorer/api controllers + 5 hex-loop sites — **-240** — low — ⚠️ judgment
  **Merged the two `dup-hash-to-hex` findings.** Delete dead wallet_direct_shield; migrate to `HexStr` (5-arg rewrite); fold is_all_digits + format_zcl. *Excludes htlc.c / rolling_anchor / mcp ops — owned elsewhere.*
- [ ] **views-format-zcl-consolidate** — wallet_view, explorer_internal.h, stats views, test_explorer, wallet_view_internal.h — **-40** — trivial — ⚠️ judgment
  Drop format_zcl wrapper + explorer_format_zcl inline copy + redundant ZATOSHI_PER_ZCL define.
- [ ] **models-dead-and-dedup** — wallet_tx/block/wallet_key/utxo/tx_index/peer models + coins_view + update_coins — **-95** — low — ⚠️ judgment
  Dead wrappers, hex dedup, hook-registration dedup, fprintf→LOG_FAIL, MoneyRange().

### Services / conditions / jobs
- [ ] **services-sleep-time-helpers** — time_compat.h + 6 services + block_sync — **-28** — trivial — ⚠️ judgment
  Collapse 6 sleep_ms + 4 now helpers; named block-interval. *Co-owns `time_compat.h` — run before util batch.*
- [ ] **services-sidecar-rolling-anchor** — addrman/block_index sidecar, rolling_anchor, chain_evidence_names → controller — **-150** — low — ⚠️ judgment
  Shared sidecar I/O, dead rolling-anchor exports, move name tables. Owns rolling_anchor hex-loop site.
- [ ] **conditions-snapsync-dedup** — snapshot_sync_service + 4 conditions + 2 READMEs + contradiction_frozen — **-30** — low — ⚠️ judgment
  Shared snapsync accessor, naming fix, rewrite DSL-myth READMEs, fix Phase-0 comment.
- [ ] **jobs-stage-dedup** — stage_helpers.h + job.h + 8 stage files + staged_sync_supervisor — **-240** — low — ⚠️ judgment
  Extract cursor/drain/reader/row-count helpers, eliminate wall_now_s, name SHADOW_STAGE_QUIET_US.

### Storage / util / lib-misc
- [ ] **storage-projection-helpers** — projection_util.h + event_log.h + 8 projections + block_index + test — **-310** — low — ⚠️ judgment
  **Merged the two projection-statics findings.** Shared static-inline helpers + frame-overhead constant. Keep per-file exec_sql.
- [ ] **util-time-dedup** — time_compat.h + 5 util srcs + workpool + util.c/util.h + safe_alloc.h — **-60** — **medium** — ⚠️ judgment
  Dedup mono_us, dedup num-cpus, **fix LogPrintStr dead sink (observability behavior change)**, fix misplaced include. *Co-owns `time_compat.h` — run after services batch.*
- [ ] **util-dead-metrics-health** — metrics, heartbeat, event.h — **-40** — low — ⚠️ judgment
  Dead solution-checks metric, Sol/s + cursor-math fix, health register dedup, stale comment.
- [ ] **lib-misc-slp-znam-dedup** — op_return_push.h + slp.c + znam.c — **-40** — low — ⚠️ judgment
  Shared OP_RETURN push/read helper.

### Chain / rpc / wallet / consensus-adjacent / domain
- [ ] **chain-mmr-mmb-dedup** — mmr, chainparamsbase, chainparams, mmb — **-85** — low — ⚠️ judgment
  MMR append/bag dedup, dead command-line params, testnet/regtest b58 dedup, strip mmb deliberation comments.
- [ ] **rpc-dead-and-magic** — legacy_chain_oracle, protocol.c/.h — **-33** — low — ⚠️ judgment
  Dedup result-str parser, named port 8232, hide unexported helper. *Internal edits only — not a legacy/shadow flip.*
- [ ] **wallet-keys-dedup** — wallet, keystore, hd_keychain + test — **-55** — low — ⚠️ judgment
  Signing-loop + address-encode extraction, eq-helper collapse, printf→LOG, named constants, dead hd wrapper.
- [ ] **consensus-adj-coreio-script-htlc** — core_io, script_error, htlc — **-135** — low — ⚠️ judgment
  Dead format_script + ScriptErrorString, double-serialize fix, SWAP_COLS + HTLC_CONTRACT_SIZE. Owns htlc.c hex site.
- [ ] **domain-consensus-dedup** — reject_out.h, check_block, header_accept, upgrades, verify, equihash (domain+crypto+registry+test) — **-35** — low — ⚠️ judgment
  Shared reject helpers, epoch composition, equihash param map (3→1), verify include fix. **Owns the scheme_equihash_200_9.c comment** (merged out of dead-files-crypto).

### Adapters / root-config / mcp / tests
- [ ] **adapters-hygiene** — db_maintenance/health sqlite, shadow_feeder, process_block headers — **-4** — low — ⚠️ judgment
  Unused string.h, reword stale shadow comment, dedup process_block_get_node_db decl.
- [ ] **root-config-cli-flags-includes** — main.c, cli.c, cli_rpc.{c,h}, boot.h, boot.c, boot_services.c, Makefile — **-150** — low — ⚠️ judgment
  Extract shared CLI unit, delete dead -daemon/-saplingscan flags, dedup includes, fix comment. *Serialize with net-hygiene-dedup (boot_services.c).*
- [ ] **mcp-dedup** — diagnostics/meta/ops/wallet/app/chain controllers, router, middleware, conditions_controller, mcp_server + 3 tests — **-150** — low — ⚠️ judgment
  DEFINE_PT folds, envelope/scan-int dedup, dissolve conditions_controller, OOM returns, DEFINE_PT_STR macro. Owns mcp ops_controller hex site.
- [ ] **tests-helpers-consolidate** — test_helpers.h/.c, test_parallel + ~25 test files — **-170** (+19 from drift registration) — low — ⚠️ judgment
  Register 19 drifted groups; centralize rm_rf/tmpdir/paths/easy_params helpers. **Run LAST** — co-owns `test_helpers.h` with net/checkqueue batches.

---

## do_not_touch (intentional — exclude from all future proposals)

- **Legacy/shadow comparison apparatus** — cutoverpreflight, cutovermode, the `*_projection_diff` SHADOW-vs-LEGACY data path, shadow_feeder data path, diff_with_legacy_shadow, legacy_mirror, legacy_chain_oracle authority pairing. (Only the *MCP handler wrappers* for projection_diff are DEFINE_PT-folded, and only internal dedup inside legacy_chain_oracle.c is in scope — **never flip the legacy↔shadow pair**.)
- **domain/<ctx>/ pure modules + their thin lib/ wrapper delegators** — the wrapper is deliberate, not duplication.
- **docs/FRAMEWORK.md:218** (`No CONDITION(){DETECT{...}}` rejection note) — correct; only the README DSL-myth prose is rewritten.
- **snapshot_complete_resume.c / snapshot_offer_ready.c** runtime_snapsync with `g_test_svc` override; **tip_wedged_resnapshot.c** different-signature runtime_snapsync — keep local.
- **block_index_projection.c / coins_view_projection.c / progress_store.c** local helpers — intentionally divergent; not folded into projection_util.h.
- **Per-file exec_sql** in the 8 projections — `[module]` prefix + `obs-ok:` tag are intentional.
- **blocker.c** local mono_us — keeps `g_test_clock_us` test override.
- **`// obs-ok:` markers** — relocate with code, never delete.
- **Generated/table files** — sha3_windows.c, crypto field-arithmetic tables, sha3_avx512.c `keccakf_4way` (distinct from the `keccakf` being made static).
- **Required idioms** — LOG_*, zcl_malloc(size,label), AR_*_SAVE; activerecord.h body macros.
- **Lint gates** — `tools/scripts/check_*.sh`, `one_result_type_baseline.txt`, `lib_layering_baseline.txt`, the `one-result-type-ok`/`raw-sql-ok` markers, and `check_no_raw_clock_outside_platform.sh`. Never weaken.
- **Intentionally-divergent test helpers** — `make_easy_params` (verify), mkdtemp `make_tmpdir` family, `rf_/pf_/ua_tmpdir`, the `rm_rf` (system) vs `rm_rf_simple` (pure-C) distinction.
- **keystore uint160_eq** (surviving primitive — don't delete both), **block_template_free** (3 callers, stays non-static), **ScriptError enum + set_script_* inlines** (fuzz + 2 tests).
- **MCP EXPECTED_TOTAL=112 / EXPECTED_OPS=47** — zcl_conditions stays registered (via diagnostics) when conditions_controller.c is dissolved.