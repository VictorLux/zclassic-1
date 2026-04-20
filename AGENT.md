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

**Overall: 80 / 130 rows closed (62%) | KPIS estimate: ~48/100**

Row count expanded with the 2026-04-20 full-review wave: P14.14-P14.16
(chain-restore extensions), P13.6/P13.7 (net + lint), P12.3.1/P12.6.1/
P12.6.2/P12.8.1/P12.8.2 (MCP + log cleanup), P7.11 / P9.11, and the
new Priority groups P15-P19 (discipline + architecture + testing + perf
+ attribution — the "shining example" roadmap).

| Tier | Closed / Total | Open rows |
|---|---|---|
| **CRITICAL** | 17 / 28 | P14.13 (Agent-2 NOW), P14.14, P14.10, P14.3, P14.6, P13.1, P9.2 |
| **HIGH** | 31 / 50+ | P9.1, P9.4 (Agent-3 NOW), P12.2, P12.3, P12.3.1, P13.2, P13.4, P13.6, P13.7, P14.4, P14.5, P14.15, P14.16, P15.1, P15.2, P15.4, P15.6, P16.1, P16.2, P17.1, P17.2, P17.4, P17.5, P17.6, P18.1, P19.1 |
| **MED** | 26 / 50+ | P7.10, P7.11, P8.4, P9.6-P9.9, P12.5-P12.8, P12.6.1, P12.6.2, P12.8.2, P13.3, P13.5, P15.3, P15.5, P16.3-P16.7, P17.3, P18.2, P18.4 |
| **LOW** | 2 / 7 | P9.10, P9.11, P12.8, P18.3 |
| (P0 baseline) | 4 / 4 | — |

**Owner state (2026-04-20, post-cleanup):**
- **Agent-2 NOW:** **P14.13** — `chain_restore_rebuild_active_chain` O(N²) boot hang. Then P14 drain → P13/P12/P7/P8 drain → P15 discipline → P16 staged-sync port → P17.4/P17.5 support → P18 perf → P19.1 attribution. Full checklist in [`AGENT-2.md`](AGENT-2.md). ~50 rows, ~3-4 months.
- **Agent-3 NOW:** **P9.4** — `fr_fft` / `fr_fft_parallel` silent no-op. Then P9 drain → P11.4/P11.5/P11.6/P11.8 MVP CI gates → P15.4/P15.5 discipline → P17 testing lead → P18.4 crypto perf. Full checklist in [`AGENT-3.md`](AGENT-3.md). ~25 rows, ~6-10 weeks.
- **Coordinator (Rhett):** canary post-P14.13 deploy; review P15-P18 acceptance; own license decision (P19.2); monitor KPIS.

**Live-node state:** chain pinned at h=3,081,601 (SQLite); legacy
zclassicd at 3,084,847 (gap 3,246 blocks). Service stopped —
P14.13 prevents boot completion. **DO NOT call `zcl_syncdiag`** until
P14.3 lands (crashes the node). `zcl_status` is safe.

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
| P9.1 | `g1_scalar_mul` side-channel | open |
| P9.2 | `sapling_circuit.c` placeholder UB | open |
| P9.3 | Groth16 CS-builder OOM silent drop | done 86ebfc4b5 |
| **P9.4** | `fr_fft` non-pow-2 silent no-op | **Agent-3 NOW** |
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
| **P14.10** | `SKIP_ALREADY_RUNNING` deferred-activation queue | open — Agent-2 CRITICAL |
| P14.11 | `block_index` nBits=0 on restore path | done 5f04aef62 |
| P14.12 | `active_chain` single-entry after restore | done 5f04aef62 |
| **P14.13** | `chain_restore_rebuild_active_chain` O(N²) | **Agent-2 NOW CRITICAL** |
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

**Start gate:** P14 drains + P15.1/P15.2 land. ~6 weeks.

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

## Status tracking

Edit tables inline as work lands. Replace `open` with `in-progress` /
`done` and include the commit SHA + `[test:X.X]`. When Agent-2 or
Agent-3 ships a row, the owning agent updates its own row. Rhett
reviews anything depending on it before it lands.

**Postmortems:** [`docs/postmortems/`](docs/postmortems/).
**Architecture diagrams:** [`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md).
**Runbook:** [`docs/RUNBOOK.md`](docs/RUNBOOK.md).
**Borrowed concepts:** [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
