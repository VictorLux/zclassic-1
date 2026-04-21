# zclassic23 — Master Agent Checklist

Owner: Rhett (coordinator). Delegates: Agent-2 (see
[`AGENT-2.md`](AGENT-2.md)), Agent-3 (see [`AGENT-3.md`](AGENT-3.md)).

---

## Core focus

Three things matter, in order:

1. **The chain works.** Live node syncs to tip and stays there. No manual surgery. No OOM cycle. Everything else is decoration until this is true.
2. **Consensus parity with zclassicd is provable.** Continuous diff against the legacy peer; CRITICAL alarm on any divergence.
3. **Recovery is automatic.** Crashes, disk corruption, power loss — the node comes back up and rejoins the chain on its own.

**The rule, every bug fix (non-negotiable post-P10.1 reset):**
1. Reproduce the failure on a fixture deterministically.
2. Identify the EXACT root cause — not the symptom.
3. Write the regression test FIRST. It must fail pre-fix.
4. Implement the fix. Test passes.
5. Deploy.

No hotfixes. Every row is `[test:1.0]` by construction going forward.

---

## Progress — last updated 2026-04-20

**Overall: 81 / 130 rows closed (62%) | KPIS estimate: ~42/100 (honest post-audit — see rubric below)**

Row count expanded with the 2026-04-20 full-review wave: P14.14-P14.16
(chain-restore extensions), P13.6/P13.7 (net + lint), P12.3.1/P12.6.1/
P12.6.2/P12.8.1/P12.8.2 (MCP + log cleanup), P7.11 / P9.11, and the
new Priority groups P15-P19 (discipline + architecture + testing + perf
+ attribution — the "shining example" roadmap).

| Tier | Closed / Total | Open rows |
|---|---|---|
| **CRITICAL** | 20 / 28 | P14.14, P14.3, P14.6, P13.1 |
| **HIGH** | 31 / 50+ | P12.2, P12.3, P12.3.1, P13.2, P13.4, P13.6, P13.7, P14.4, P14.5, P14.15, P14.16, P15.1, P15.2, P15.4, P15.6, P16.1, P16.2, P17.1, P17.2, P17.4, P17.5, P17.6, P18.1, P19.1 |
| **MED** | 26 / 50+ | P7.10, P7.11, P8.4, P9.6-P9.9, P12.5-P12.8, P12.6.1, P12.6.2, P12.8.2, P13.3, P13.5, P15.3, P15.5, P16.3-P16.7, P17.3, P18.2, P18.4 |
| **LOW** | 2 / 7 | P9.10, P9.11, P12.8, P18.3 |
| (P0 baseline) | 4 / 4 | — |

**Owner state (2026-04-20, post-cleanup):**
- **Agent-2 NOW:** **P14.14** — populate `block_index.skipList[]` on chain-restore path (P14.10 landed 8b5443a8d [test:1.0 fd23f77a3]; P14.13 landed a62394130 — coordinator canary pending for both). Then P14 drain → P13/P12/P7/P8 drain → P15 discipline → P16 staged-sync port → P17.4/P17.5 support → P18 perf → P19.1 attribution → P20 dev-MCP → P21 oversized-file split → P22 AI-native scaffolding → P23 simplification + generator MCP. Full checklist in [`AGENT-2.md`](AGENT-2.md).
- **Agent-3 NOW:** **P9.6** — `zclassic_sapling_spend_proof` witness length not bounded (P9.2 landed c8f6bb7bc). Then P9 drain → P11.4/P11.5/P11.6/P11.8 MVP CI gates → P15.4/P15.5 discipline → P17 testing lead → P18.4 crypto perf → P20 dev-MCP (coverage + test-map) → P21 test oversized-file split → P22.4 spec corpus → P23.7 scaffold_test_from_row. Full checklist in [`AGENT-3.md`](AGENT-3.md).
- **Coordinator (Rhett):** canary post-P14.13 deploy; review P15-P18 acceptance; own license decision (P19.2); monitor KPIS.

**Live-node state:** chain pinned at h=3,081,601 (SQLite); legacy
zclassicd at 3,084,847 (gap 3,246 blocks). Service stopped since
2026-04-20 20:13 — P14.13 boot hang fixed a62394130; coordinator
canary pending. **DO NOT call `zcl_syncdiag`** until P14.3 lands
(crashes the node). `zcl_status` is safe.

---

## MVP + Hardening KPIs

**MVP target:** "Someone we don't know can run zclassic23 and use it
for a week without intervention." 8 binary criteria — see
[`MVP.md`](MVP.md). **MRS today: ~3 / 8.** MVP achieved at MRS = 8/8
AND HI ≥ 80%.

**KPIS (shining-example score, 0-100):** 10 pillars × 10 pts each
(Correctness / Robustness / Performance / Security / Operability /
Observability / Code quality / Test discipline / Documentation /
Architecture). Rubric lands with P15-P19 implementation. Release
gate: KPIS ≥ 85. Shining-example bar: KPIS = 100.

**SWRC formula:** CRIT=4, HIGH=2, MED=1, LOW=0.5. P0 weighted as
HIGH. Today ~48%.

**HI (Hardening Index):** fraction of closed rows with RED-first
test. Today ~0.50. Every P10.1+ row is 1.0 by construction; P15-P19
is 1.0 by construction.

---

## Ground rules (every agent)

- zclassic23 is the next-gen product. zclassicd is a legacy bootstrap peer only.
- Read [`CLAUDE.md`](CLAUDE.md) and [`DEFENSIVE_CODING.md`](DEFENSIVE_CODING.md) before touching code.
- Everything is in the single binary — no standalone shell scripts, no Docker.
- `make test` MUST pass before any commit. `make ci` MUST pass before push.
- Small commits. Push frequently. Never amend pushed commits.
- Do NOT touch files outside your assigned lane.
- Borrowed concepts cite their source in the commit message + [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).

---

## Priority 0 — Build enforcement (CLOSED)

Build lint gates live. `make lint` fails-exit-1 on raw `sqlite3_step` hits.

| # | Task | Status |
|---|---|---|
| P0.1 | `check-raw-sqlite` fail-exit-1 | done a5511028d |
| P0.2 | `-DZCL_AR_ENFORCE` | done a5511028d |
| P0.3 | `check_no_secret_printf.sh` wired | done a5511028d |
| P0.4 | `deploy` depends on `ci` | done a5511028d |

## Priority 1 — Money-loss / consensus-split (CLOSED)

P1.1-P1.16b all landed. Wallet silent-error, Sapling fail-open,
raw-step in batch writer, P2SH sigops, difficulty skip, Ed25519 /
RedJubjub canonicality, RNG hygiene, constant-time scalar mul, group-
hash return-checking, ed25519 CT audit, curve25519 CT audit, Sapling
nullifier-path CT reduction.

## Priority 2 — P2P attack surface (CLOSED)

P2.1-P2.8 all landed. Mempool peer-tx validation, 1.6MB stack alloc,
fast_sync AR_BEGIN_SAVE, per-chunk hash verify, cs_nodes deadlock,
g_swarm_active TOCTOU, FlyClient rate-limit, recv-queue byte budget.

## Priority 3 — MCP / application layer (CLOSED)

P3.1-P3.7 all landed. JSON injection, raw sqlite3_step sweep in
controllers, checksum validation, realloc-NULL check, CSRF token,
`/metrics` auth.

## Priority 4 — Script / consensus memory safety (CLOSED)

P4.1-P4.5 all landed. 520KB stack alloc, silent stack_push, script_num
outsize bounds, disconnect unbounded realloc, sigencoding parity audit.

## Priority 5 — Operator / deploy hygiene (CLOSED)

P5.1-P5.7 all landed. Tracked-binary purge, service flags →
EnvironmentFile, `/home/rhett` sweep, shell-script audit, Tor submodule
pin bump, SQLite pin bump to 3.53.0, repo-root tidy.

## Priority 6 — Wallet / storage medium (CLOSED)

P6.1-P6.6 all landed. write_sapling_key UPDATE, flusher reset,
read_keys skip, migration framework, best_block re-prepare, coins_alloc
OOM.

## Priority 7 — Live-node hardening

| # | Task | Status |
|---|---|---|
| P7.1 | Tip stuck — update_tip bool propagate | done a6bedccad |
| P7.2 | DB_ERR_TIP_MISMATCH auto-rewind + fatal | done 57e6ef391 |
| P7.3 | Crash-handler async-signal-safe stderr | done e9e79dda2 |
| P7.4 | Backpressure tip-watchdog | done f6474c77b (Agent-3) |
| P7.5 | TimeoutStopSec=90 | done ec7948ee3 |
| P7.6 | StartLimit relax | done ec7948ee3 |
| P7.7 | LimitCORE=infinity | done ec7948ee3 |
| P7.8 | SQLite cache_size/mmap_size | done dbca0be78 |
| P7.9 | Thread registry | done 19b2cac1d |
| **P7.10** | Migrate long-running loops to registry | **open — Agent-2** |
| **P7.11** | `nat.c:241` UPnP recv bounds | **open — Agent-2** |

## Priority 8 — Fresh review (zmsg / dandelion / bloom / zslp / znam)

| # | Task | Status |
|---|---|---|
| P8.1 | zmsg_deserialize heap overflow | done b6726f83b |
| P8.2 | Dandelion PRNG quality | done 576b5cde2 (Agent-3) |
| P8.3 | mmb_deserialize height cap | done c06515cbd (Agent-3) |
| **P8.4** | Compact-block reconstruction O(n·m) | **open — Agent-2** |
| P8.5 | rolling_bloom hash-func clamp | done 21da0531e (Agent-3) |
| P8.6 | zslp ticker / txid collision | done 93936c5fb |
| P8.7 | zmarket num_chunks truncation | done 8e5522a8b |
| P8.8 | ZNAM type cap lift | done bb8f293b1 |
| P8.9 | BIP30 partial-rewind (superseded by P10.1) | superseded |
| P8.10 | BIP30 hotfix-2 (superseded by P10.1) | superseded |

## Priority 9 — Sapling-prover deep audit (Agent-3)

| # | Task | Status |
|---|---|---|
| P9.1 | `g1_scalar_mul` side-channel | done f10b39303 (Agent-3) [test:1.0] |
| P9.2 | `sapling_circuit.c` placeholder UB | done c8f6bb7bc (Agent-3) [test:1.0] |
| P9.3 | Groth16 CS-builder OOM silent drop | done 86ebfc4b5 |
| P9.4 | `fr_fft` non-pow-2 silent no-op | done f5a31b48d (Agent-3) |
| P9.5 | Sapling cache race (pthread_once) | done ff25fc779 |
| P9.6 | `spend_proof` witness length | open |
| P9.7 | `sprout_verify_groth16` underflow + race | open |
| P9.8 | `ensure_generators` exhaustion | open |
| P9.9 | Prover printf cleanup | open |
| P9.10 | MSM cache-side-channel | open (threat-model decision) |
| P9.11 | `zip32_diversifier` `for(;;)` cap | open |

## Priority 10 — Root-cause discipline (CLOSED)

P10.1.1 through P10.1.5 landed. See
[`docs/postmortems/2026-04-19-bip30-stall.md`](docs/postmortems/2026-04-19-bip30-stall.md).

## Priority 11 — MVP CI gates

| # | Task | Status |
|---|---|---|
| P11.1 | MVP #2 — Tor bootstrap <60s | done 63f98909d (Agent-3) |
| P11.3 | MVP #3 — cold-start sync <10 min | done ffd1112e4 (Agent-3) |
| P11.4 | MVP #4 — shielded payment | open — Agent-3 |
| P11.5 | MVP #5 — store e2e | open — Agent-3 |
| **P11.6** | MVP #6 — 7-day soak harness | **open — Agent-3, highest-leverage** |
| P11.7 | MVP #7 — kill-9 chaos recovery | done 8d3d3b23f (Agent-3) |
| P11.8 | MVP #8 — parity diff (pairs with P12.3) | open — Agent-3 |

## Priority 12 — Post-P10.1 hardening + sync UX

| # | Task | Status |
|---|---|---|
| P12.1 | Sapling tree checkpoint | done 8fb7cb623 (Agent-3) |
| **P12.2** | BLOCK_FAILED_CHILD GC (= P14.6) | open — Agent-2 |
| **P12.3** | Continuous parity-diff service | open — Agent-2 |
| **P12.3.1** | `zcl_parity_status` MCP tool | open — Agent-2 |
| P12.4 | `sqlite3` CLI dep → in-tree wal_checkpoint | done d1fb2422e |
| **P12.5** | `coins_map_erase` call-site audit | open — Agent-2 |
| **P12.6** | Structured JSON logging | open — Agent-2 |
| **P12.6.1** | `msgprocessor.c` fprintf cleanup | open — Agent-2 |
| **P12.6.2** | `process_block.c` fprintf cleanup | open — Agent-2 |
| **P12.7** | Block-index height-repair root-cause | open — Agent-2 |
| **P12.8** | `zcl_health` RSS trajectory | open — Agent-2 |
| **P12.8.1** | `zcl_mvp_status` MCP tool | open — Agent-2 |
| **P12.8.2** | `zcl_flush_stats` + `zcl_reorg_diff` tools | open — Agent-2 |

## Priority 13 — Sync UX wave

| # | Task | Status |
|---|---|---|
| **P13.1** | Single-peer sync regression | open — Agent-2 CRITICAL |
| **P13.2** | Header tip oscillation → 480 | open — Agent-2 |
| **P13.3** | `connect_block_local` spam | open — Agent-2 |
| **P13.4** | IBD throughput 5-8× slow | open — Agent-2 |
| **P13.5** | addrman `find_node_by_service` gap | open — Agent-2 |
| **P13.6** | DNS-seed fallback | open — Agent-2 |
| **P13.7** | `main.c` raw sqlite3_step lint | open — Agent-2 |

## Priority 14 — P10.1 reopened: live-node stall

| # | Task | Status |
|---|---|---|
| P14.1 | SAVEPOINT flush dedicated connection | done d67817dd2 |
| P14.2 | End-to-end RED for P14.1 | done d67817dd2 |
| **P14.3** | `zcl_syncdiag` SIGABRT via json_free | open — Agent-2 CRITICAL |
| **P14.4** | Sync FSM flap debounce | open — Agent-2 |
| **P14.5** | `val.block_connected` on commit not receipt | open — Agent-2 |
| **P14.6** | BLOCK_FAILED_CHILD propagation cap | open — Agent-2 CRITICAL |
| P14.7 | Chain stops at 3,081,601 (partial — b3f1903d4) | partial |
| P14.8 | `block_already_seen` short-circuit retry | done 0e4b6ca35 |
| P14.9 | Dual IBD reporter divergence | open (absorbs into P16.3) |
| P14.10 | `SKIP_ALREADY_RUNNING` deferred-activation queue | done 8b5443a8d (Agent-2) [test:1.0 fd23f77a3] |
| P14.11 | `block_index` nBits=0 on restore path | done 5f04aef62 |
| P14.12 | `active_chain` single-entry after restore | done 5f04aef62 |
| P14.13 | `chain_restore_rebuild_active_chain` O(N²) | done a62394130 (Agent-2) [test:1.0 b07284439] |
| **P14.14** | `block_index.skipList[]` on restore path | open — Agent-2 CRITICAL |
| **P14.15** | `nChainTx` + `nSequenceId` backfill | open — Agent-2 |
| **P14.16** | block_index flat-file per-entry CRC32 | open — Agent-2 |

---

## Priority 15 — Discipline upgrades (shining-example wave)

Prerequisite for P16. Without `[[nodiscard]]` enforcement and a
canonical Result type, P16's service refactor reshuffles without
tightening semantics.

| # | Task | File | Severity | Owner | Attribution |
|---|---|---|---|---|---|
| P15.1 | Remove `-Wno-unused-result` + `[[nodiscard]]` sweep (Agent-2 lanes) | `Makefile`, `lib/*/include/` | HIGH | Agent-2 | C23 |
| P15.2 | `struct zcl_result` + `enum zcl_err` canonical | `lib/util/include/util/result.h` (new) | HIGH | Agent-2 | — |
| P15.3 | `[[gnu::cleanup(zcl_free_p)]]` for heap pointers | `lib/net/src/zmsg.c`, `lib/znam/`, `lib/zslp/`, `msg_*.c` | MED | Agent-2 | GCC/Clang |
| P15.4 | `[[nodiscard]]` sweep (Agent-3 lanes) | `lib/{crypto,sapling,keys}/include/`, `lib/core/include/core/random.h` | HIGH | Agent-3 | — |
| P15.5 | Split CLAUDE.md → per-subsystem `agents.md` | repo-wide | MED | Agent-2 + Agent-3 | Erigon per-subsystem agents.md (LGPL-3.0) |
| P15.6 | Ruleguard grep gates in `tools/scripts/check_*.sh` | `Makefile:~543`, `tools/scripts/` | HIGH | Agent-2 | Erigon ruleguard (LGPL-3.0) |

## Priority 16 — Architecture / Rails-way (Erigon-inspired)

**Start gate:** P14 drains + P15.1/P15.2 land.

| # | Task | File | Severity | Owner | Attribution |
|---|---|---|---|---|---|
| P16.1 | `struct zcl_stage` + runner | `lib/sync/` (new) | HIGH | Agent-2 | Erigon stagedsync (LGPL-3.0) |
| P16.2 | Port sync FSM to 9-stage pipeline | `lib/sync/src/stages/*.c` | HIGH | Agent-2 | Erigon default_stages (LGPL-3.0) |
| P16.3 | Per-stage timings via `zcl_status` | `lib/sync/src/runner.c`, `tools/mcp/controllers/chain_controller.c` | MED | Agent-2 | Erigon sync.go::timings (LGPL-3.0) |
| P16.4 | Per-stage `Cfg` struct pattern (27 services) | `app/services/src/*.c` | MED | Agent-2 | Erigon HeadersCfg (LGPL-3.0) |
| P16.5 | `docs/NAMING.md` + grep-gate | `docs/NAMING.md` (new), `tools/scripts/check_naming.sh` | MED | Agent-2 | Erigon kv_interface (LGPL-3.0) |
| P16.6 | Unified `storage_temporal` interface | `lib/storage/include/storage/temporal.h` (new) | MED | Agent-2 | Erigon Temporal DB (LGPL-3.0) |
| P16.7 | `zcl_stream` iterator with `[[gnu::cleanup]]` | `lib/storage/include/storage/stream.h` (new) | MED | Agent-2 | Erigon stream-vs-cursor (LGPL-3.0) |

## Priority 17 — Testing regime

Runs parallel with P16. Agent-3 owns the bulk — fuzz + property-based
+ spectest + contract tests overlap the sapling/wire-parser lanes.

| # | Task | File | Severity | Owner | Attribution |
|---|---|---|---|---|---|
| P17.1 | Wire fuzz harnesses into CI + 5 new ones | `tools/fuzz/*.c`, `Makefile`, `tests/fuzz_corpus/` | HIGH | Agent-3 | libFuzzer/AFL++ |
| P17.2 | Sanitizer matrix (asan/tsan/ubsan/msan) | `Makefile`, CI config | HIGH | Agent-3 | — |
| P17.3 | Property-based test harness | `lib/test/include/test/proptest.h` (new) | MED | Agent-3 | QuickCheck pattern |
| P17.4 | ETL framework for bulk writes | `lib/storage/src/etl.c` (new) | HIGH | Agent-2 | Erigon db/etl/ (LGPL-3.0) |
| P17.5 | Spectest harness vs zclassicd | `tests/spectest/`, `tools/spectest_runner.c` | HIGH | Agent-3 + Agent-2 | Erigon cl/spectest/ (LGPL-3.0) |
| P17.6 | Unwind-is-inverse-of-Forward contract test | `lib/test/src/test_stage_contract.c` (new) | HIGH | Agent-3 | Erigon unwind discipline (LGPL-3.0) |

## Priority 18 — Perf / systems

Last in ordering. Profile after P16 stages exist; don't optimize
architecture you're about to rewrite.

| # | Task | File | Severity | Owner |
|---|---|---|---|---|
| P18.1 | Cache-aware `block_map` (SoA + prefetch) | `lib/util/src/hash_table.c` or `lib/chain/src/block_map.c` | HIGH | Agent-2 |
| P18.2 | io_uring for disk_block_io | `lib/storage/src/disk_block_io.c` | MED | Agent-2 |
| P18.3 | PGO pipeline | `Makefile`, `tools/scripts/record_pgo.sh` (new) | LOW | Agent-2 |
| P18.4 | `zcl_crypto_status` MCP tool (validate AVX-512 IFMA active) | `lib/crypto/src/cpu_features.c` (new), MCP | MED | Agent-3 |

## Priority 19 — Licensing + attribution

| # | Task | Status |
|---|---|---|
| P19.1 | `ATTRIBUTIONS.md` seed + per-row cross-refs | done 2026-04-20 (file created; rows append as they land) |
| P19.2 | LICENSE audit; set zclassic23's own license | open — Coordinator |

---

## Priority 20 — Developer MCP (MCP-for-dev, 2026-04-20)

**The goal:** fresh AI agent sessions orient in 3-5K tokens instead of
15-20K. Today's 60+ MCP tools query the running **node**; P20 adds
tools that query the **repo + its state**.

Research grounding: the 2026 state-of-the-art for AI-native codebases
is LSP-MCP bridges + structured project metadata + per-subsystem
agents.md. See [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md) for inspirations.

**Can start immediately** — doesn't depend on P14/P15/P16. Parallel
with everything else.

| # | Tool | Purpose | Owner |
|---|---|---|---|
| **P20.1** | `zcl_codemap` | Returns `{subsystem: {files, public_symbols, deps}}` for `lib/*/` and `app/*/`. Fast orientation for fresh agents. | Agent-2 |
| **P20.2** | `zcl_roadmap` | Generates from `.ac.yaml` sidecars (P22.2). Returns AGENT.md rows as JSON: `{id, tier, severity, owner, status, sha, acceptance, depends_on}`. Agents query "what's my NOW" programmatically. | Agent-2 |
| **P20.3** | `zcl_lsp_*` family — superseded by P22.3 (integrate `mcp-language-server` via clangd; don't reinvent) | (filed here for dep tracking — implementation is P22.3) | Agent-2 |
| **P20.4** | `zcl_coverage` | `file → {lines_covered, lines_total, test_files: [...]}`. Built from `gcov`/`llvm-cov`. | Agent-3 |
| **P20.5** | `zcl_impact` | `file_or_symbol → transitive reverse-dependency graph`. Uses P22.3 LSP call-hierarchy + type-hierarchy under the hood. | Agent-2 |
| **P20.6** | `zcl_lint_status` | Current `make lint` violations with file:line. Cached; refreshes on file-change mtime. | Agent-2 |
| **P20.7** | `zcl_postmortems` | Structured index of `docs/postmortems/` — `{date, title, root_cause_tag, affected_rows, outcome}`. | Agent-2 |
| **P20.8** | `zcl_stages` | (Post-P16) Staged-sync pipeline graph: `{stage_id, forward_file, unwind_file, prune_file, invariants, last_timings}`. | Agent-2 |
| **P20.9** | `zcl_build_info` | Last-built binary SHA + delta files since. "The running binary doesn't match your tree" warning. | Agent-2 |
| **P20.10** | `zcl_test_map` | `test_file → [files_exercised]` + reverse. Answers "what test would catch this regression?" | Agent-3 |

## Priority 21 — Oversized-file deconstruction

**Research grounding:** 2026 LLM context studies show smaller focused
modules + retrieval beat stuffing large files. File-size budget
enforced by lint (see P22.5).

10 files over 2,000 lines. 5 are controllers (Rails-way violation —
"skinny controllers, fat services"). Rest are oversized test files + the
boot coordinator + `process_block.c` (absorbs into P16.2 stages).

| # | File | Lines | Target split | Owner |
|---|---|---|---|---|
| **P21.1** | `app/controllers/src/sync_controller.c` | 3,456 | Pure dispatch glue (~150 lines); move logic to new services | Agent-2 |
| **P21.2** | `app/controllers/src/explorer_controller.c` | 3,376 | Split by view: `explorer_{blocks,tx,addr,stats,chart}_controller.c` | Agent-2 |
| **P21.3** | `app/controllers/src/blockchain_controller.c` | 2,992 | Split query vs ops; move ops to services | Agent-2 |
| **P21.4** | `app/controllers/src/wallet_diagnostic_controller.c` | 2,474 | Move diagnostics to services; controller becomes skinny | Agent-2 |
| **P21.5** | `app/controllers/src/api_controller.c` | 2,362 | Split by resource: `api_{blocks,tx,wallet,...}_controller.c` | Agent-2 |
| **P21.6** | `lib/net/src/msgprocessor.c` | 3,404 | Pull per-message handlers out of dispatch; land with P12.6.1 | Agent-2 |
| **P21.7** | `lib/test/src/test_sapling.c` | 4,677 | `test_sapling_{crypto,circuit,proof,note,tree,wallet}.c` | Agent-3 |
| **P21.8** | `lib/test/src/test_net.c` | 4,123 | `test_net_{msgprocessor,download,connman,dandelion,swarm}.c` | Agent-3 |
| **P21.9** | `config/src/boot.c` | 2,460 | Per-subsystem boot hooks via boot-registry (like thread_registry); each subsystem owns `*_boot_init()` | Agent-2 |
| **P21.10** | `lib/validation/src/process_block.c` | 2,375 | Absorbed into P16.2 stages — each stage gets its own file under `lib/sync/src/stages/` | Agent-2 (blocks on P16.2) |

**Acceptance per row:** no file over 1,000 lines in the migrated path;
`make test` green; symbol-count unchanged (no behavior change).

## Priority 22 — AI-native scaffolding (2026-04-20)

**Research-informed.** 2026 state of the art: (a) `AGENTS.md` is the
emerging portable standard, (b) LSP-MCP bridges (clangd + MCP) give
agents semantic code intelligence natively, (c) `.ac.yaml` sidecars
give machine-readable acceptance criteria, (d) three-tier context
(hot constitution / specialized sub-agents / cold retrieval corpus).
See [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).

**Can start immediately** — parallel with P14 drain.

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| **P22.1** | `AGENTS.md` at repo root — portable-standard alias for `CLAUDE.md`. Either symlink or dual-maintain. Root file is the 50-line index into per-subsystem `agents.md` (P15.5). | `AGENTS.md` (new), `CLAUDE.md` | HIGH | Agent-2 |
| **P22.2** | `.ac.yaml` sidecar per AGENT.md row. One `ROWS.yml` at repo root OR per-row `docs/rows/P<id>.ac.yaml`. Schema: `{id, tier, severity, owner, status, sha, files, acceptance: [...], depends_on: [...]}`. Generator keeps it in sync with AGENT.md tables (CI-enforced). Feeds `zcl_roadmap` (P20.2). | `docs/rows/*.ac.yaml` (new), `tools/scripts/gen_row_sidecars.py` (new) | HIGH | Agent-2 |
| **P22.3** | **Integrate clangd + LSP-MCP bridge into zclassic23 binary.** Expose as `zcl_lsp_definition`, `zcl_lsp_references`, `zcl_lsp_hover`, `zcl_lsp_call_hierarchy`, `zcl_lsp_diagnostics`. Spawns a clangd subprocess bound to `compile_commands.json` (generated from `Makefile`). Replaces custom P20.3 + P20.5 work — use the production-tested LSP path. | `lib/devinfo/src/lsp_bridge.c` (new), `tools/mcp/controllers/dev_controller.c` (new) | HIGH | Agent-2 |
| **P22.4** | `docs/spec/` — cold-memory RAG-retrievable spec docs per subsystem. One short spec per `lib/*/`: architecture, invariants, known gotchas, on-disk format. Not duplicate of `agents.md` (which is hot-memory operational); this is reference material a RAG retriever can pull into context. | `docs/spec/<subsystem>.md` (new) | MED | Agent-2 (net/validation/storage/wallet/script) + Agent-3 (crypto/sapling/keys) |
| **P22.5** | File-size budget lint gate. No file in `lib/` or `app/` over 1,000 lines. `tools/scripts/check_file_size_budget.sh` wired into `make lint`. Exit-1 on violation. Existing oversized files grandfathered via `tools/scripts/file_size_budget_exemptions.txt` — each exemption listed with an AGENT.md row that closes it (P21.*). | `tools/scripts/check_file_size_budget.sh` (new), `Makefile:~543` | HIGH | Agent-2 |
| **P22.6** | `AGENTS.md` specifies a "fresh-session bootstrap" — the canonical 3-step orientation sequence for a new AI agent: (1) call `zcl_roadmap` for your NOW, (2) call `zcl_codemap` for your lane, (3) read the relevant `agents.md` in your scope. No reading AGENT.md end-to-end. | `AGENTS.md` | MED | Agent-2 |

## Priority 23 — Structural simplification + generative MCP (2026-04-20)

**Completes the MCP triangle.** Today's 60+ MCP tools are
**observational** (query node state). P20 adds **informational** (query
repo + derived metadata). P23 adds **generative** (create scaffolded
code from one call) — so an agent can file a new row, a new service, a
new stage, or a new test from a single tool invocation with the
boilerplate already correct.

Plus two structural simplifications the post-purge architecture
review surfaced: subsystem consolidation audit (28 `lib/*/` is a lot;
several pairs likely fold) and a boot-registry pattern that
supersedes P21.9.

**Can start immediately** where not blocked. P23.5/P23.6/P23.7/P23.8
have explicit predecessors.

| # | Task | File:line | Severity | Owner |
|---|---|---|---|---|
| **P23.1** | Subsystem consolidation audit — 28 `lib/*/` → target ≤20. Candidates: `lib/consensus` ↔ `lib/validation`, `lib/primitives` ↔ `lib/chain` ↔ `lib/coins`, `lib/core` ↔ `lib/support`. Produces `docs/ARCHITECTURE.md` with proposed merges + per-merge impact. Each merge lands as its own follow-up row; P23.1 is the audit-and-plan row. | `docs/ARCHITECTURE.md` (new), repo-wide audit | HIGH | Agent-2 |
| **P23.2** | Boot-registry — `config/src/boot.c` (2,460) → `lib/core/src/boot_registry.c` with per-subsystem self-registration (`ZCL_BOOT_INIT(name, fn, deps)`). Each subsystem owns its `*_boot_init.c` and registers at link time. Supersedes P21.9 (close P21.9 when P23.2 lands). | `lib/core/src/boot_registry.c` (new), per-subsystem `*_boot_init.c` (new), `config/src/boot.c` | HIGH | Agent-2 |
| **P23.3** | Makefile audit + pattern-rule consolidation. Measure per-subsystem object-count + compile-time hotspots; fold redundant rules; target clean `make -j$(nproc)` ≤ 2 min on this host. Acceptance: before/after numbers in the commit message. | `Makefile` | MED | Agent-2 |
| **P23.4** | `zcl_scaffold_mcp_tool(name, category, description)` — generator. One call creates: MCP dispatch entry, controller stub, handler scaffold, AGENT.md row, `.ac.yaml` sidecar (P22.2), RED test skeleton. The completeness of the triangle starts here. | `tools/mcp/controllers/scaffold_controller.c` (new), `tools/mcp/scaffolder/*.c` (new) | HIGH | Agent-2 |
| **P23.5** | `zcl_scaffold_service(name, description)` — generates `app/services/<name>_service.{h,c}` with P16.4-shape `Cfg` struct + RED test skeleton. | `tools/mcp/scaffolder/service.c` (new) | MED | Agent-2 (blocks on P16.4) |
| **P23.6** | `zcl_scaffold_stage(name, forward_desc, unwind_desc)` — generates `lib/sync/src/stages/<name>.c` with Forward/Unwind/Prune triad + P17.6 contract test skeleton. | `tools/mcp/scaffolder/stage.c` (new) | MED | Agent-2 (blocks on P16.1) |
| **P23.7** | `zcl_scaffold_test_from_row(row_id)` — reads the `.ac.yaml` sidecar (P22.2), emits a RED test skeleton that matches the acceptance criteria. Biggest productivity multiplier for `[test:1.0]` discipline. | `tools/mcp/scaffolder/test.c` (new) | HIGH | Agent-3 (blocks on P22.2) |
| **P23.8** | `zcl_explain(file, line)` — combines clangd AST (via P22.3 LSP bridge) with relevant `agents.md` + `docs/spec/` via RAG (P22.4). Answers "what is this function's contract and constraints" in one call. | `tools/mcp/controllers/explain_controller.c` (new) | MED | Agent-2 (blocks on P22.3 + P22.4) |
| **P23.9** | `zcl_commit_plan(intent)` — reads `git diff` + AGENT.md rows in progress, returns a structured commit message (row ID, attribution line, RED-test evidence block). Enforces commit-message discipline that today is manual + easy to skip. | `tools/mcp/controllers/commit_plan_controller.c` (new) | MED | Agent-2 |

---

## Status tracking

Edit tables inline as work lands. Replace `open` with `in-progress` /
`done` and include the commit SHA + `[test:X.X]`. When Agent-2 or
Agent-3 ships a row, the owning agent updates its own row. Rhett
reviews anything depending on it before it lands.

**Postmortems:** [`docs/postmortems/`](docs/postmortems/).
**Architecture diagrams:** [`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md).
**Runbook:** [`docs/RUNBOOK.md`](docs/RUNBOOK.md).
**Borrowed concepts:** [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
