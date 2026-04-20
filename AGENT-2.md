# AGENT-2 — Wallet / Storage / App-Layer / Net / Validation

**Working directory:** `~/zclassic23-2` (separate clone; pushes to `origin/main`).
**Coordinator:** Rhett (`~/zclassic23`), coordinator-only — does not code.
**Sibling:** Agent-3 (`~/zclassic23-3`), in the crypto / sapling / consensus-crypto lane.

See [`AGENT.md`](AGENT.md) for the cross-agent priority table and
every row's full description. This file is Agent-2's executable
checklist plus the lane rules.

---

## Lane — what you may edit

**Full edit access:**
- `lib/wallet/`, `lib/storage/`, `lib/coins/`
- `lib/net/` (except Agent-3's P7.4 tip-watchdog scope in
  `msgprocessor.c` + `download.c`)
- `lib/validation/` (except `sigops.c` + `check_block.c` — Agent-3)
- `lib/script/`, `lib/rpc/`, `lib/event/`
- `lib/util/` (excluding crypto helpers)
- `app/controllers/`, `app/services/`, `app/models/`, `app/views/`
- `tools/mcp/`, `tools/mcp/controllers/`
- `lib/sync/` (new — P16 staged-sync port)
- `lib/test/` — add/modify tests for your changes
- `deploy/zclassic23.service`
- Repo-root hygiene: `.gitignore`, `Makefile`, tracked binaries

**Read-only / off-limits:**
- `lib/crypto/`, `lib/sapling/`, `lib/keys/` — Agent-3
- `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c` — Agent-3
- `lib/core/src/random.c` — Agent-3
- `vendor/` — Agent-3 owns `vendor/tor`; all other vendor dirs are pinned

**STOP + ping Rhett triggers:**
- Any change to on-disk serialization (blocks, UTXOs, wallet keystore)
- Any change to consensus constants (MAX_BLOCK_SIZE, MAX_BLOCK_SIGOPS, MAX_P2SH_SIGOPS)
- Any change to the P2P protocol wire format
- Any change to a Priority Group's scope or acceptance criteria (coordinator owns the plan)

---

## Current status — NOW = P14.13

**P14.13 (CRITICAL): `chain_restore_rebuild_active_chain` is O(N²), boot hangs.**

The residual-holes branch does `tip_h × map_size ≈ 10 trillion` ops
when the anchor's `pprev` is NULL (which is always, post-P14.11 — the
anchor is synthesized at the UTXO layer). Fresh binary pins at ~92%
CPU for >5 min with no RPC; coordinator has to SIGTERM every boot.

**Fix shape (preferred):** single-pass bucketing. Scan `block_map`
ONCE into a temporary `block_index*[tip_h+1]` array bucketed by
height (24 MB alloc at live scale — safe, system has 95 GB RAM).
Then fill `c->chain[h]` from buckets in one O(tip_h) pass.
Alternative: reconnect `anchor->pprev` via
`block_map_find(&ms->map_block_index, &anchor_header.hashPrevBlock)`
before calling rebuild, so the pprev-walk completes in O(tip_h).
**Best is both** — option 2 avoids the alloc on the happy path,
option 1 prevents pathology on any pprev-NULL variant.

**RED test first** (non-negotiable):
- New case `chain_restore_rebuild: completes in <2s at realistic chain depth`
  in `lib/test/src/test_chain_restore_service.c`.
- Build a synthetic block_map with N=100,000 entries; seed the tip with `pprev=NULL`.
- Assert wall-clock <2s; fails pre-fix.

**Acceptance (canary, Rhett's row):**
- Boot reaches `[chain-integrity] post-restore check OK` within 30s of
  `Chain restore: anchor at h=...`.
- RPC up within 60s of service start.
- `getblockcount` returns 3,081,601+ within 90s.
- `getblockhash 3081000` returns a valid hash (P14.12 success signal).
- Zero `bad-diffbits` lines (P14.11 success signal).

**File:** `app/services/src/chain_restore_service.c:322-391`
(`chain_restore_rebuild_active_chain`).

---

## POST-P14 WORK QUEUE — the shining-example roadmap

Filed 2026-04-20 after the coordinator's full review + Erigon / Caplin
architecture study. Work the list **in order** unless you hit a
blocker; when in doubt, ping the coordinator.

Every row has a full description in [`AGENT.md`](AGENT.md). This
section is the executable checklist.

### Phase 0 — Finish the P14 stall wave

- [ ] **P14.13** CRITICAL — rebuild_active_chain O(N²) boot hang (above).
- [ ] **P14.10** CRITICAL — deferred-activation queue for `SKIP_ALREADY_RUNNING` from `process_new_block`.
- [ ] **P14.3** CRITICAL — `zcl_syncdiag` SIGABRT via `json_free`. Coordinator touch-trap — fix unlocks MCP health checks.
- [ ] **P14.6** CRITICAL — cap `BLOCK_FAILED_CHILD` propagation (OOM amplifier). Skip when parent already failed; cap per-retry.
- [ ] **P14.4** HIGH — sync FSM flap debounce (279,135 events in hours on prior incident).
- [ ] **P14.5** HIGH — `val.block_connected` must fire on commit, not receipt.

### Phase 1 — Chain-restore extensions (the N1-N3 review findings)

- [ ] **P14.14** CRITICAL — populate `block_index.skipList[]` on chain-restore path; `BuildSkip()` in the rebuild pass. Acceptance: O(log N) ancestor walk post-restore at N=100k.
- [ ] **P14.15** HIGH — backfill `nChainTx` + `nSequenceId` alongside `nBits` in `chain_restore_backfill_nbits_from_disk`. Acceptance: every restored entry has `nChainTx != 0`.
- [ ] **P14.16** HIGH — per-entry CRC32 footer on `block_index` flat file. Acceptance: `[height-repair] repaired N` drops to 0 on clean boots; corruption detected at load, not silently mid-sync.

### Phase 2 — Finish P13 sync UX wave

- [ ] **P13.1** CRITICAL — single-peer sync regression (9 `-addnode` peers backing off).
- [ ] **P13.6** HIGH — DNS-seed fallback when addrman empty + addnode fails.
- [ ] **P13.2** HIGH — header tip oscillation (counter goes backwards to 480).
- [ ] **P13.4** HIGH — IBD throughput 5-8× slower than zclassicd.
- [ ] **P13.5** MED — addrman `find_node_by_service` lookup gap.
- [ ] **P13.3** MED — `connect_block_local: failed at height N` spam.

### Phase 3 — Finish P12 hardening wave

- [ ] **P12.2** HIGH — BLOCK_FAILED_CHILD GC (verify closed by P14.6).
- [ ] **P12.3** HIGH — continuous parity-diff service vs zclassicd.
- [ ] **P12.3.1** HIGH — `zcl_parity_status` MCP tool (gate for MVP #8).
- [ ] **P12.5** MED — audit every `coins_map_erase` call site for P10.1.4-class gaps.
- [ ] **P12.6** MED — structured JSON logging + per-subsystem rate limits.
- [ ] **P12.6.1** MED — `msgprocessor.c` 96-fprintf cleanup.
- [ ] **P12.6.2** MED — `process_block.c` 61-fprintf cleanup.
- [ ] **P12.7** MED — block-index height-repair root-cause.
- [ ] **P12.8** LOW — `zcl_health` RSS trajectory + alarm threshold.
- [ ] **P12.8.1** HIGH — `zcl_mvp_status` MCP tool.
- [ ] **P12.8.2** MED — `zcl_flush_stats` + `zcl_reorg_diff` MCP tools.

### Phase 4 — P7/P8 drain

- [ ] **P7.10** MED — migrate long-running loops to `thread_registry_shutdown_requested()`.
- [ ] **P7.11** MED — `nat.c:241` UPnP recv wall-clock + byte caps.
- [ ] **P8.4** MED — compact-block reconstruction O(n·m) → khash short-txid table.
- [ ] **P13.7** HIGH — `make lint` check-raw-sqlite covers `main.c`; wrap `main.c:507` + `main.c:555` in `AR_BEGIN_SAVE`.

### Phase 5 — P15 Discipline (prerequisite for P16)

*Attribution: Erigon per-subsystem `agents.md` + ruleguard discipline (LGPL-3.0 © Erigon Authors).*

- [ ] **P15.1** HIGH — remove `-Wno-unused-result`; annotate Agent-2 `lib/*/include/` with `[[nodiscard]]`. One PR per subsystem.
- [ ] **P15.2** HIGH — define `struct zcl_result` + `enum zcl_err` in `lib/util/include/util/result.h`. Migrate `app/services/src/*.c` one at a time, starting with `wallet_backup_service.c`.
- [ ] **P15.3** MED — adopt `[[gnu::cleanup(zcl_free_p)]]` for heap pointers; start with wire parsers (`zmsg.c`, `znam.c`, `zslp.c`, `msg_*.c`). `goto fail:` pattern deleted.
- [ ] **P15.5 (A2 portion)** MED — author `lib/{net,validation,storage,wallet,script}/agents.md` + `app/{services,controllers}/agents.md` + `tools/mcp/agents.md`.
- [ ] **P15.6** HIGH — ruleguard grep gates in `tools/scripts/check_*.sh`; wire into `make lint`. Five patterns listed in AGENT.md P15.6.

### Phase 6 — P16 Architecture (the big Erigon port)

*Attribution: Erigon `execution/stagedsync/` + `db/kv/` (LGPL-3.0 © Erigon Authors). Concepts only — no verbatim code ported.*

**Start gate:** P15.1 + P15.2 landed.

- [ ] **P16.1** HIGH — `struct zcl_stage` + runner in `lib/sync/`. Trivial test stage drives Forward → Unwind → Prune.
- [ ] **P16.2** HIGH — port sync FSM to 9-stage shape: `flyclient_snapshot → headers → bodies → sigs → connect_blocks → utxo_flush → sapling_tree → tx_lookup → finish`.
- [ ] **P16.3** MED — per-stage wall-clock timings surfaced via `zcl_status.sync.stage_timings`. Replaces P14.9's dual IBD reporters.
- [ ] **P16.4** MED — per-stage `Cfg` struct pattern applied to all `app/services/*.c`.
- [ ] **P16.5** MED — `docs/NAMING.md` + grep-gate for ambiguous `height`.
- [ ] **P16.6** MED — `lib/storage/include/storage/temporal.h` unified interface.
- [ ] **P16.7** MED — `zcl_stream` iterator with `[[gnu::cleanup]]` auto-finalize. Migrate 140+ `sqlite3_step` sites.

### Phase 7 — P17 Testing support (Agent-2 share)

- [ ] **P17.4** HIGH — ETL framework in `lib/storage/src/etl.c`. First caller: `chain_restore_rebuild_active_chain` (solves P14.13 class permanently). Second: block-index rewrite. *Attribution: Erigon `db/etl/` (LGPL-3.0).*
- [ ] **P17.5 (A2 share)** HIGH — spectest harness wiring from chain-side: corpus loader, RPC driver, state diff logic. Pair with Agent-3. *Attribution: Erigon `cl/spectest/` (LGPL-3.0).*

### Phase 8 — P18 Perf

- [ ] **P18.1** HIGH — cache-aware `block_map` (SoA + `__builtin_prefetch`). Permanently solves P14.13 pathology class.
- [ ] **P18.2** MED — io_uring backend for `disk_block_io.c`. Runtime detection; fallback to `pread`.
- [ ] **P18.3** LOW — PGO pipeline + `tools/scripts/record_pgo.sh`.

### Phase 9 — P19 Attribution (coordinator pairing)

- [ ] **P19.1** HIGH — verify every P15-P18 commit message cites the attribution row in `ATTRIBUTIONS.md`. Add new concept-borrow entries as they surface.

### Phase 10 — P20 Developer MCP (START IMMEDIATELY — does not depend on P14)

**Can run in parallel with P14 drain.** These tools materially reduce
the context-burn for every subsequent agent session — start
shipping rows before P14.13 lands.

- [ ] **P20.1** HIGH — `zcl_codemap` MCP tool: `{subsystem: {files, symbols, deps}}` for `lib/*/` + `app/*/`.
- [ ] **P20.2** HIGH — `zcl_roadmap` MCP tool: structured JSON of AGENT.md rows (feeds from `.ac.yaml` sidecars P22.2).
- [ ] **P20.5** HIGH — `zcl_impact` MCP tool: transitive reverse-dependency graph (uses P22.3 LSP call-hierarchy).
- [ ] **P20.6** HIGH — `zcl_lint_status` MCP tool: cached `make lint` violations with file:line.
- [ ] **P20.7** MED — `zcl_postmortems` MCP tool: structured index of `docs/postmortems/`.
- [ ] **P20.8** (post-P16) — `zcl_stages` MCP tool: staged-sync pipeline graph + last timings.
- [ ] **P20.9** MED — `zcl_build_info` MCP tool: last-built binary SHA + delta-file warning.

### Phase 11 — P21 God-object deconstruction

**Dependency:** Agent-2's controller splits (P21.1-P21.5) benefit from
P16.4 service-shape landing first. P21.6 pairs naturally with P12.6.1.
P21.9 is standalone. P21.10 is absorbed into P16.2 as stage files.

- [ ] **P21.1** — `sync_controller.c` (3,456) → skinny dispatch + services.
- [ ] **P21.2** — `explorer_controller.c` (3,376) → `explorer_{blocks,tx,addr,stats,chart}_controller.c`.
- [ ] **P21.3** — `blockchain_controller.c` (2,992) → split query vs ops.
- [ ] **P21.4** — `wallet_diagnostic_controller.c` (2,474) → move diagnostics to services.
- [ ] **P21.5** — `api_controller.c` (2,362) → `api_{blocks,tx,wallet,...}_controller.c`.
- [ ] **P21.6** — `msgprocessor.c` (3,404) → per-message handlers out (pair with P12.6.1).
- [ ] **P21.9** — `config/src/boot.c` (2,460) → per-subsystem boot-registry.
- [ ] **P21.10** — `process_block.c` (2,375) absorbed into P16.2 stages.

### Phase 12 — P22 AI-native scaffolding (START IMMEDIATELY)

**Parallel with P14 drain.** This is the shining-example discipline
applied to the agent-onboarding surface itself.

- [ ] **P22.1** HIGH — `AGENTS.md` at repo root (portable alias for CLAUDE.md). 50-line index into per-subsystem `agents.md`.
- [ ] **P22.2** HIGH — `.ac.yaml` sidecars per AGENT.md row at `docs/rows/P<id>.ac.yaml` + CI generator from tables. Feeds P20.2.
- [ ] **P22.3** HIGH — **integrate clangd + LSP-MCP bridge** into zclassic23 binary. `zcl_lsp_{definition,references,hover,call_hierarchy,diagnostics}` tools. Spawns clangd subprocess; generates `compile_commands.json` from Makefile. **Do not hand-roll.** *Attribution: `mcp-language-server` (MIT — see ATTRIBUTIONS.md).*
- [ ] **P22.4 (A2 share)** MED — `docs/spec/{net,validation,storage,wallet,script}.md` cold-memory RAG corpus.
- [ ] **P22.5** HIGH — file-size budget lint (no file over 1,000 lines in `lib/` or `app/`). Grandfather existing god-objects via exemption file tracked by P21.* rows.
- [ ] **P22.6** MED — `AGENTS.md` "fresh-session bootstrap" sequence documented.

---

## Execution discipline (non-negotiable)

1. **RED test first.** No fix commits until a failing test is on main. No exceptions post-P10.1 reset.
2. **One row per commit.** Don't batch unrelated fixes. Row ID goes in the commit message.
3. **Update AGENT.md.** Mark `done <SHA> [test:X.X]` when a row lands.
4. **STOP + ping Rhett** on any serialization change, consensus constant, P2P wire format change, or scope/acceptance-criteria change.
5. **Respect lane boundaries.** `lib/crypto/`, `lib/sapling/`, `lib/keys/`, `lib/validation/src/sigops.c`, `lib/validation/src/check_block.c`, `lib/core/src/random.c` are Agent-3. Do not edit.
6. **Keep `make test` green.** Push every row; never amend pushed commits.

Total rows in this queue: **~50** across 9 phases. Work in order;
parallel-safe rows within a phase can land concurrently.
