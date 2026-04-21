# zclassic23 — Master Agent Checklist

Owner: Rhett (coordinator, runs Claude Code in `~/zclassic23`).
Delegates: Agent-2 (see [`AGENT-2.md`](AGENT-2.md), worktree
`~/zclassic23-2`), Agent-3 (see [`AGENT-3.md`](AGENT-3.md), worktree
`~/zclassic23-3`).

**Worker tooling (2026-04-21):** Agent-2 and Agent-3 run as Gemini CLI
instances. Session-start bootstrap: `pwd` → identify lane → read
[`AGENTS.md`](AGENTS.md) → open
`AGENT-2.md` or `AGENT-3.md` → `git pull --rebase` → work the NOW row.
The coordinator remains Claude Code. Cross-tool compatibility is
handled by [`AGENTS.md`](AGENTS.md) (portable) and the lane-file
structure being tool-agnostic. Gemini-specific config: policy file at
`~/.gemini/policies/zclassic23.toml`, MCP config at
`.gemini/settings.json` (committed).

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

## Progress — last updated 2026-04-21

**Overall: 90 / 145 rows closed (62%) | KPIS estimate: ~60/100 (P24.13 landed 2026-04-21 05:54 — Correctness 2→7, sync unblocked, headers at network tip)**

Row count expanded 2026-04-21 with **P24 — coordinator-audit wave**:
P24.1–P24.10 (datadir hygiene, lint rules, `abort()` triage, header/block
invariant, `goto fail` refactor, deploy pre-flight, `zcl_binary_vs_head`
MCP tool, oversized-file backlog, crash-recovery CI). Original
2026-04-20 full-review wave: P14.14-P14.16, P13.6/P13.7,
P12.3.1/P12.6.1/P12.6.2/P12.8.1/P12.8.2, P7.11 / P9.11, P15-P19
(discipline + architecture + testing + perf + attribution).

| Tier | Closed / Total | Open rows |
|---|---|---|
| **CRITICAL** | 26 / 31 | **P24.14** (RPC SIGABRT class — 3 live crashes in session), P13.1 |
| **HIGH** | 32 / 57 | P12.3, P12.3.1, P13.2, P13.4, P13.6, P13.7, P14.4, P14.5, P14.15, P14.16, P15.1, P15.2, P15.4, P15.6, P16.1, P16.2, P17.1, P17.2, P17.4, P17.5, P17.6, P18.1, P19.1, P24.2, P24.3, P24.4, P24.5, P24.7, P24.8, P24.10, **P24.15** |
| **MED** | 27 / 55 | P7.10, P7.11, P8.4, P9.7, P9.8, P9.9, P12.5-P12.8, P12.6.1, P12.6.2, P12.8.2, P13.3, P13.5, P15.3, P15.5, P16.3-P16.7, P17.3, P18.2, P18.4, P24.1, P24.6, P24.9, **P24.16** |
| **LOW** | 2 / 8 | P9.10, P9.11, P12.8, P18.3, **P24.17** |
| (P0 baseline) | 4 / 4 | — |

**Owner state (2026-04-21 20:58 — P24.11 DEPLOYED LIVE, both agents mid-flight on new NOW):**
- **Agent-2 NOW: P24.14** — actively mid-implementation in `~/zclassic23-2` (10 files, +132 lines, uncommitted as of 20:55). Pattern: new `rpc_chainstate_guard.{c,h}` chokepoint helper + inline checks in all 5 controllers (transaction, chain_inspect, wallet_diagnostic, wallet_rescan, repair, wallet) + `test_rpc_safety.c` fixture-based RED harness + `check_coins_lookup_nullcheck.sh` lint. Matches the design in AGENT-2.md verbatim. **Warn before kickoff-resetting zclassic23-2** — Agent-2's WIP is tracked-file edits which `git stash push -u` will preserve. Next: **P24.14** (landing now) → **P13.1** (last CRITICAL) → P14 drain (P14.4, P14.5, P14.15, P14.16) → P13/P12/P7/P8 drain → P15-P23. Full checklist in [`AGENT-2.md`](AGENT-2.md).
- **Agent-3 NOW: P11.4** HIGH — shielded-payment CI gate (MVP #4). **Stalled** — untracked `test_shielded_payment_gate.c` (360 lines) untouched since 2026-04-21 06:01 (~15h); Codex-3 process not running (`~/.codex/logs_3.sqlite` absent). A kickoff will wipe the file. Rhett: please kickoff `~/zclassic23-3` to re-arm Agent-3 — file is untracked so a `git stash push -u` before `reset --hard` would preserve it. Post-P11.4 lane: P11.5/P11.8 MVP gates → P15.4/P15.5 → P17 testing lead → P18.4 crypto perf → P20 dev-MCP → P21.7/P21.8 test split → P22.4 spec corpus → P23.7 scaffold_test_from_row → **P24.2 / P24.4**. Full checklist in [`AGENT-3.md`](AGENT-3.md).
- **Coordinator (Rhett):** **P24.11 deployed live 2026-04-21 20:25** (6,800-block regression absorbed via unclean-shutdown UTXO re-import + Sapling tree rebuild at h=476969..3078003; headers caught up in 45min, blocks_download live now). Post-P24.14 deploy: canary the 5 affected RPC controllers, recompute KPI (Robustness 3→6, Observability 5→7). Open coord-lane: P24.16 (wallet.persistence false-unhealthy), P24.1 datadir sweep automation, P24.7 deploy pre-flight. Not blocking on these — routing attention to workers.

**Live-node state (2026-04-21 02:28, ~30 min post-deploy):** mainnet
running fresh binary. 12 peers holding steady at h=3,085,141 (network
tip). **Sync root cause identified via `zcl_events` log scan** —
every inbound header batch fails at `check_block.c:249` with
`bad-diffbits` on header[0]: `GetNextWorkRequired`'s 17-block window
cannot walk across the 193-block post-snapshot inversion, returns
`nProofOfWorkLimit` (weakest-allowed), mismatches peer's real nBits,
rejects entire 160-header batch. `bans_total=0` because we discard
silently rather than punish — good peers are being starved. Filed as
**P24.13 CRITICAL** — Agent-2 reassigned from P14.6. Gap to legacy
zclassicd: ~3,540 blocks, will not close until P24.13 lands. Prior
canary positive signals (peers 3→18, chain-restore clean) remain
valid; they just don't move the tip until P24.13 is fixed.
**`zcl_syncdiag` STILL crashes the node** (P24.11 CRITICAL — corrected via
`nm` symbol resolution 2026-04-21 05:02: real crash is inside `rpc_getsyncdiag`
itself at `+0xCB` / `+0xB`, NOT `rpc_downloadstats` as originally hypothesized).
**`zcl_getrawtransaction` ALSO crashes the node** via `rpc_getrawtransaction+0x4AB`
→ `coins_view_cache_get_coins+0x1B3` SEGV on 193-block inverted-tail heights —
filed as **P24.14 CRITICAL**. Evidence in `~/.zclassic-c23/node.log` offsets
400 / 22458 (P24.11 repro) and 219298 (P24.14 repro).
Orphaned `.corrupt.*` artifacts (6.7 GB) swept to
`~/zcl-backups/corrupt-sweep-20260421/` as P24.1 groundwork.
**SAFE MCP tools (workers use only these):** `zcl_status`, `zcl_getblockcount`,
`zcl_kpi`, `zcl_peers`, `zcl_peer_report`, `zcl_events`, `zcl_logtail`, `zcl_health`.
**UNSAFE until P24.11 + P24.14 land:** `zcl_syncdiag`, any RPC touching
`coins_view_cache` (`gettxout`, `getcoins`, `listunspent` on inverted-tail
heights), any composite RPC that passes a `json_t*` through an error path.

---

## MVP + Hardening KPIs

**MVP target:** "Someone we don't know can run zclassic23 and use it
for a week without intervention." 8 binary criteria — see
[`MVP.md`](MVP.md). **MRS today: ~3 / 8.** MVP achieved at MRS = 8/8
AND HI ≥ 80%.

**KPIS (shining-example score, 0-100):** 10 pillars × 10 pts each.
Release gate: KPIS ≥ 85. Shining-example bar: KPIS = 100.

### KPIS rubric — concrete scoring (each 0-10)

Computed from observable state (live node + repo + row status). Each
pillar scored to the nearest integer. Stale when any condition listed
below is violated — recompute after significant row closures.

| # | Pillar | 0–3 (failing) | 4–6 (working) | 7–9 (strong) | 10 (shining) |
|---|---|---|---|---|---|
| 1 | **Correctness** | Node can't sync to tip OR any CRITICAL consensus row open | At tip, 0 CRITICAL | At tip ≥1 wk, UTXO parity-diff == 0 vs zclassicd | Full mainnet replay from genesis bit-matches |
| 2 | **Robustness** | Crashes >1/day during normal ops | No crash 24h, canary OK | No crash 1wk, all `abort()` triaged (P24.4), ≤10 `assert()` in runtime paths (P24.15) | No crash 30d, chaos-test CI (`make ci-crash`, P24.10) passes nightly |
| 3 | **Performance** | Fresh-sync > 10 min OR RSS > 8 GB at tip OR `blocks_per_sec` < 100 | Fresh-sync < 2 min, RSS < 4 GB at tip, bg-val ≥ 500 b/s | PGO enabled (P18.1), io_uring (P18.2), `zcl_benchmark` regression-gated | Beats zclassicd fresh-sync wall-clock 10× |
| 4 | **Security** | Any CRITICAL in P1-P4 open OR `make lint` silencing warnings | P1-P4 closed, `-Wno-unused-result` removed (P18.6) | Sanitizer matrix in CI (asan/tsan/ubsan/msan), fuzz corpus > 100 seeds (P17.4) | 3rd-party audit passed + open bounty |
| 5 | **Operability** | Service can stop-after-crashes OR `make deploy` ships stale binaries | StartLimit wide (P7.6 ✓), `make deploy` pre-flight catches drift (P24.7) | `zcl_binary_vs_head` (P24.8) + datadir hygiene (P24.1) + log rotation working (P24.17) | Zero-downtime rolling deploys, seamless wallet migration |
| 6 | **Observability** | `zcl_syncdiag` crashes OR `zcl_events` blind spots | `zcl_status`/`zcl_kpi`/`zcl_events` reliable; P24.11 closed | Per-stage timings (P16.2), structured log sink, Grafana board | Distributed-trace ready, can root-cause any prod issue from logs alone |
| 7 | **Code quality** | Raw `sqlite3_step` / `malloc` / `goto fail` unchecked | Lint gates enforce P0.1/P24.2/P24.3, no files >50 kB (P21, P24.9) | `[[gnu::cleanup]]` adoption 80%+ (P15.3), `zcl_result` canonical (P15.2) | Cyclomatic complexity gated, every public fn has `agents.md` entry |
| 8 | **Test discipline** | HI < 50% OR any MVP gate (P11.*) red | HI 50-80%, 6/8 MVP gates green, RED-first since P10.1 | HI 80-95%, all 8 MVP gates green (MRS=8/8), property tests (P17.2) | HI = 100%, spec-test parity vs zclassicd (P17.5), mutation-test coverage >70% |
| 9 | **Documentation** | Missing LICENSE/NOTICE or onboarding broken | LICENSE + NOTICE ✓ (P19.2), AGENTS.md works (P22.1) | Per-subsystem `agents.md` (P15.5), SPDX headers (P24.12), runbook current | `zcl_explain` (P23.8) + generative MCP (P23) operational |
| 10 | **Architecture** | Monolithic, no stage model | Single-binary C23, MCP stable, Rails-way partially adopted | 9-stage pipeline (P16.1) lands, ETL framework (P16.5), contract tests (P17.6) | Erigon-parity throughput, every subsystem has a `zcl_scaffold_*` generator |

### Today's score (2026-04-21 05:33 UTC) — honest assessment

| Pillar | Score | Evidence |
|---|---|---|
| Correctness | **2** | Node stalled 3,670 blocks behind zclassicd tip — P24.13 sync-blocker open; 4 CRITICAL rows open (P24.13, P24.14, P24.11, P13.1) |
| Robustness | **3** | 3 SIGABRTs this session, 45 `assert()` untriaged (P24.15 just filed), wallet persistence false-unhealthy (P24.16) |
| Performance | **6** | Bg-val 529 blocks/s (strong), RSS 2.7 GB at tip (good), fresh-sync ≤60s when working, but PGO (P18.1) + io_uring (P18.2) not yet |
| Security | **8** | P1–P4 all CLOSED (money-loss, P2P attack, MCP, script memory), DEFENSIVE_CODING.md enforced, no open CRITICAL in security lane |
| Operability | **5** | systemd linger service stable, StartLimit widened (P7.6), Tor embedded working, but binary-drift detection (P24.7) + log rotation (P24.17 only manual) still open |
| Observability | **5** | Rich MCP surface (60+ tools), `zcl_kpi` / `zcl_events` / `zcl_logtail` solid; `zcl_syncdiag` crashes (P24.11), no per-stage timings yet (P16.2) |
| Code quality | **5** | Lint gates live (raw sqlite3_step, check_no_secret_printf), Apache-2.0 adopted, but 370 raw malloc + 34 goto-fail + 11 oversized files untriaged |
| Test discipline | **5** | HI ~0.50 per AGENT.md, 89/145 rows closed, RED-first mandatory since P10.1, MVP #6 + #7 green, MVP #4/5/8 red |
| Documentation | **7** | AGENT.md + AGENTS.md + CLAUDE.md + DEFENSIVE_CODING.md + ATTRIBUTIONS.md all current, LICENSE + NOTICE land, per-subsystem agents.md (P15.5) missing, SPDX (P24.12) open |
| Architecture | **6** | Single-binary C23 with embedded Tor + MCP server working, FlyClient + SHA3 UTXO snapshot + Sapling + Equihash solid, stage model (P16) not yet |

**TOTAL: 52 / 100** (previous estimate "~44" was pessimistic — rubric-based score is 52).

### Path to KPIS ≥ 75 (ship-quality)

Three rows move the score +20 points (52 → 72):

1. **P24.13 land + deploy** → Correctness 2→8 (at tip), closes top CRITICAL. **+6 points.** Agent-2 was 30 min from committing before the kickoff reset — design preserved in AGENT-2.md RESUME-HERE block.
2. **P24.11 + P24.14 land** → Robustness 3→7 (no RPC-triggered crashes), Observability 5→7 (`zcl_syncdiag` safe). **+6 points.** Agent-2 owns both post-P24.13.
3. **P24.10 crash-recovery CI + P17.4 fuzz corpus + P17.5 spec-test** → Test discipline 5→8, Security 8→9. **+4 points.**

**Bonus +4:** P24.15 (assert triage), P24.7 (deploy pre-flight), P24.12 (SPDX headers) → lift Robustness / Operability / Documentation by 1 each.

**Total: 52 → 76.** Release gate at 85 requires the full P15-P19 wave + MVP 8/8 + HI ≥ 80%.

### Re-score cadence

Recompute after any CRITICAL closes OR at each end-of-day pulse. Post
the delta in AGENT.md's Progress line. KPIS is a lagging indicator —
use `zcl_kpi` + SWRC + HI for real-time work prioritization.

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
| P9.2 | `sapling_circuit.c` placeholder UB | done 94532c87e (Agent-3) [test:1.0] |
| P9.3 | Groth16 CS-builder OOM silent drop | done 86ebfc4b5 |
| P9.4 | `fr_fft` non-pow-2 silent no-op | done f5a31b48d (Agent-3) |
| P9.5 | Sapling cache race (pthread_once) | done ff25fc779 |
| P9.6 | `spend_proof` witness length | done 2fe801a08 (Agent-3) [test:1.0] |
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
| **P11.4** | MVP #4 — shielded payment | **open — Agent-3, NOW** |
| P11.5 | MVP #5 — store e2e | open — Agent-3 |
| P11.6 | MVP #6 — 7-day soak harness | done 39bb904f3 [test:1.0 4ae4b09db] (Agent-3) |
| P11.7 | MVP #7 — kill-9 chaos recovery | done 8d3d3b23f (Agent-3) |
| P11.8 | MVP #8 — parity diff (pairs with P12.3) | open — Agent-3 |

## Priority 12 — Post-P10.1 hardening + sync UX

| # | Task | Status |
|---|---|---|
| P12.1 | Sapling tree checkpoint | done 8fb7cb623 (Agent-3) |
| **P12.2** | BLOCK_FAILED_CHILD GC (= P14.6) | done via P14.6 5994bc3b1 |
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
| P14.3 | `zcl_syncdiag` SIGABRT via json_free | done 5406beca3 (Agent-2) [test:1.0 63016db95] |
| **P14.4** | Sync FSM flap debounce | open — Agent-2 |
| **P14.5** | `val.block_connected` on commit not receipt | open — Agent-2 |
| P14.6 | BLOCK_FAILED_CHILD propagation cap | done 5994bc3b1 (Agent-2) [test:1.0 de30f389d] |
| P14.7 | Chain stops at 3,081,601 (partial — b3f1903d4) | partial |
| P14.8 | `block_already_seen` short-circuit retry | done 0e4b6ca35 |
| P14.9 | Dual IBD reporter divergence | open (absorbs into P16.3) |
| P14.10 | `SKIP_ALREADY_RUNNING` deferred-activation queue | done 8b5443a8d (Agent-2) [test:1.0 fd23f77a3] |
| P14.11 | `block_index` nBits=0 on restore path | done 5f04aef62 |
| P14.12 | `active_chain` single-entry after restore | done 5f04aef62 |
| P14.13 | `chain_restore_rebuild_active_chain` O(N²) | done a62394130 (Agent-2) [test:1.0 b07284439] |
| P14.14 | `block_index.skipList[]` on restore path | done 9d71841ba (Agent-2) [test:1.0 9f114c251] |
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
| P19.2 | LICENSE audit; set zclassic23's own license | **done 2026-04-21 — Apache-2.0 adopted.** `LICENSE` file installed (canonical Apache-2.0 text from apache.org), `NOTICE` file preserves upstream MIT notices (Bitcoin Core, Zcash, zclassicd) + vendored-dep licenses (Tor BSD-3, SQLite PD, secp256k1 MIT, LevelDB BSD-3, dcrdex Blue Oak) per Apache 2.0 §4(d). `COPYING` redirects to LICENSE+NOTICE. `README.md:217` + `ATTRIBUTIONS.md` updated. Apache-2.0 chosen for: patent grant + retaliation (matters for crypto projects), compatibility with every inherited + vendored license, compatibility with LGPL-3.0 concept-borrows from Erigon (P15/P16/P17). Follow-up row filed as P24.12 (per-file SPDX header sweep). |

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
| **P22.1** | `AGENTS.md` at repo root — portable-standard alias for `CLAUDE.md`. Either symlink or dual-maintain. Root file is the 50-line index into per-subsystem `agents.md` (P15.5). | `AGENTS.md` (new), `CLAUDE.md` | HIGH | **done 2026-04-21 (coord)** — `AGENTS.md` landed at repo root alongside `GEMINI.md`. Portable orientation index pointing at `AGENT.md` / `AGENT-2.md` / `AGENT-3.md` / `CLAUDE.md` / `GEMINI.md`. Drove the Agent-2/Agent-3 transition from Claude Code to Gemini CLI. |
| **P22.2** | `.ac.yaml` sidecar per AGENT.md row. One `ROWS.yml` at repo root OR per-row `docs/rows/P<id>.ac.yaml`. Schema: `{id, tier, severity, owner, status, sha, files, acceptance: [...], depends_on: [...]}`. Generator keeps it in sync with AGENT.md tables (CI-enforced). Feeds `zcl_roadmap` (P20.2). | `docs/rows/*.ac.yaml` (new), `tools/scripts/gen_row_sidecars.py` (new) | HIGH | Agent-2 |
| **P22.3** | **Integrate clangd + LSP-MCP bridge into zclassic23 binary.** Expose as `zcl_lsp_definition`, `zcl_lsp_references`, `zcl_lsp_hover`, `zcl_lsp_call_hierarchy`, `zcl_lsp_diagnostics`. Spawns a clangd subprocess bound to `compile_commands.json` (generated from `Makefile`). Replaces custom P20.3 + P20.5 work — use the production-tested LSP path. | `lib/devinfo/src/lsp_bridge.c` (new), `tools/mcp/controllers/dev_controller.c` (new) | HIGH | Agent-2 |
| **P22.4** | `docs/spec/` — cold-memory RAG-retrievable spec docs per subsystem. One short spec per `lib/*/`: architecture, invariants, known gotchas, on-disk format. Not duplicate of `agents.md` (which is hot-memory operational); this is reference material a RAG retriever can pull into context. | `docs/spec/<subsystem>.md` (new) | MED | Agent-2 (net/validation/storage/wallet/script) + Agent-3 (crypto/sapling/keys) |
| **P22.5** | File-size budget lint gate. No file in `lib/` or `app/` over 1,000 lines. `tools/scripts/check_file_size_budget.sh` wired into `make lint`. Exit-1 on violation. Existing oversized files grandfathered via `tools/scripts/file_size_budget_exemptions.txt` — each exemption listed with an AGENT.md row that closes it (P21.*). | `tools/scripts/check_file_size_budget.sh` (new), `Makefile:~543` | HIGH | Agent-2 |
| **P22.6** | `AGENTS.md` specifies a "fresh-session bootstrap" — the canonical orientation sequence for a new AI agent. | `AGENTS.md`, `GEMINI.md` | MED | **partial 2026-04-21** — `GEMINI.md` documents the `pwd` → lane-file → `git pull` → NOW-row routine. Completion (the original intent: `zcl_roadmap` / `zcl_codemap` / `agents.md` calls) blocks on P20.1 + P20.2 + P15.5 actually landing — those tools don't exist yet. Upgrade to HIGH when P20 rows ship. |

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

## Priority 24 — Coordinator audit wave (2026-04-21)

**Filed 2026-04-21** after the coordinator burned a session diagnosing
why the live node wouldn't sync past 3,081,601 — the root cause was
the main-clone binary was a full day stale (pre-dated every CRITICAL
that landed 2026-04-21). The binary-drift failure mode plus a pass
over the 370 raw `malloc()` + 40 `((unused))` + 34 app-layer
`goto fail;` sites surfaced ten NEW rows that are NOT covered by
P0–P23. None overlap P15/P17/P21 scope.

| # | Task | Files | Pri | Owner |
|---|---|---|---|---|
| **P24.1** | Datadir hygiene: sweep orphaned `block_index.bin.corrupt.*` + `.corrupt_*` dirs into `~/zcl-backups/corrupt-sweep-<ts>/` at boot. First sweep (6.7 GB) completed manually 2026-04-21; this row wires it into startup. | `app/services/src/chain_restore_service.c`, `app/services/src/datadir_sweep_service.c` (new) | MED | Rhett (coord) |
| **P24.2** | Lint rule: ban `__attribute__((unused))` on function parameters named `*_len`, `*_size`, `*_count`, or `*_sz`. This is the exact landmine that hid P9.6 for months (sapling.c:912 had `witness_len __attribute__((unused))` — length available, never used). | `tools/scripts/check_unused_len.sh` (new) + `make lint` wiring | HIGH | Agent-3 |
| **P24.3** | Lint rule: ban raw `malloc()` in `lib/` + `app/` outside an allowlist. CLAUDE.md mandates `zcl_malloc(size, "label")` but 370 raw sites across 80 files — no CI enforcement. Mirror P0.1 `check-raw-sqlite` pattern. | `tools/scripts/check_raw_malloc.sh` (new), allowlist in repo | HIGH | Agent-2 |
| **P24.4** | `abort()` triage — refactor `lib/keys/src/key.c:26,215`, `lib/sapling/src/sapling.c:107`, `lib/sapling/src/note_encryption.c:75` to return `zcl_result` instead. Void-return signatures force `abort()` as the only error path — each one is an uptime landmine. Dep: P15.2 (`zcl_result` type lands first). | `lib/keys/src/key.c`, `lib/sapling/src/sapling.c`, `lib/sapling/src/note_encryption.c` | HIGH | Agent-3 |
| **P24.5** | Header-count / block-count invariant: `chain.headers >= chain.blocks` must always hold. Observed violation live (headers 3,081,408 < blocks 3,081,601). Add invariant assert in sync state machine + RED test that forces inversion and asserts watchdog catches it. Distinct from P13.2 oscillation. | `app/services/src/sync_service.c`, `lib/test/src/test_sync_invariants.c` (new) | HIGH | Agent-2 |
| **P24.6** | `goto fail;` refactor: 34 sites in `sync_controller.c` (22), `coins_view_sqlite.c` (8), `block_index_loader.c` (4). Same pattern P15.3 kills but P15.3 is scoped to wire parsers only. Extend `[[gnu::cleanup]]` adoption to these three files. | `app/controllers/src/sync_controller.c`, `lib/storage/src/coins_view_sqlite.c`, `app/services/src/block_index_loader.c` | MED | Agent-2 |
| **P24.7** | `make deploy` pre-flight: fail-exit if `HEAD` commit time > binary mtime (binary older than source HEAD). Prevents the 2026-04-21 incident where coordinator clone ran a day-stale binary while 22 commits + 4 CRITICALs sat in source. | `Makefile` (`deploy` target), `tools/scripts/check_binary_fresh.sh` (new) | HIGH | Rhett (coord) or Agent-2 |
| **P24.8** | `zcl_binary_vs_head` MCP tool: returns `{binary_mtime, binary_sha, head_sha, commits_behind, drift_seconds}`. Coordinator-facing version of P24.7 — surfaces binary drift in one MCP call. | `tools/mcp/controllers/meta_controller.c` | HIGH | Agent-2 |
| **P24.9** | Oversized-file backlog beyond P21: `test_validation.c` (112 kB), `test_chain.c` (65 kB), `test_sapling_crypto.c` (49 kB), `bn254.c` (75 kB), `circuit_gadgets.c` (62 kB), `snapshot_sync_service.c` (74 kB), `database.c` (56 kB), `wallet.c` (53 kB), `net.c` (51 kB), `connman.c` (51 kB), `fast_sync.c` (50 kB). Expand P22.5 budget-lint exemption list OR split. | various | MED | Agent-2 |
| **P24.10** | Crash-recovery CI gate: `make ci-crash` nightly target. Loop: start → send tx → `kill -9` → restart → assert balance non-zero. Four separate postmortem memories document this class (never_destroy_wallet, utxo_wipe_safety, crash_recovery, sqlite_flush_bug) but no CI asserts it. | `tools/scripts/ci_crash.sh` (new), `lib/test/src/test_crash_recovery_ci.c` (new) | HIGH | Agent-2 |
| **P24.11** | `zcl_syncdiag` **still crashes the node** after P14.3 GREEN — CORRECTED 2026-04-21 05:02 via `nm` symbol resolution of live backtrace. The real crash site is **inside `rpc_getsyncdiag` itself** at offset `+0xCB` (second json_free callsite, not the one P14.3 5406beca3 patched) AND `+0xB` (very early — likely a stack-local `json_t*` missing `= NULL` init that hits the error-path free before any successful assignment). Prior hypothesis that `rpc_downloadstats` was the new crash point was WRONG — symbols prove both json_free frames resolve to `rpc_getsyncdiag+0xCB` / `+0xB`. Fix shape: audit `rpc_getsyncdiag` for (a) every `json_t*` local declared without `= NULL`, (b) every goto-to-fail path that free-returns a possibly-double-freed handle, (c) order-of-free between `wd` / `hdr` / return object. P14.3 fix touched only `rpc_getsyncdiag`'s wd+hdr init; something else in the same function still has the bug. Evidence: `~/.zclassic-c23/node.log` offsets 400 + 22458 both show `json_free+0x43 → rpc_getsyncdiag+0xCB`. Pair with RED test in `test_health.c` that calls `rpc_getsyncdiag` under the specific fault-injection paths exercised by MCP `h_zcl_syncdiag`. | `app/controllers/src/health_controller.c` (rpc_getsyncdiag), `tools/mcp/controllers/ops_controller.c` | CRITICAL | Agent-2 |
| **P24.12** | Per-file SPDX header sweep for Apache-2.0. After P19.2 landed the LICENSE + NOTICE files, every source file in `lib/`, `app/`, `tools/`, `config/` should carry `// SPDX-License-Identifier: Apache-2.0` (or `/* ... */` for `.c`). Mechanical pass — lint gate added afterward to fail-exit if a new file lands without an SPDX header. Vendored code under `vendor/` keeps its original license ID (`BSD-3-Clause`, `MIT`, etc.). | repo-wide source tree + `tools/scripts/check_spdx.sh` (new) | MED | Agent-2 or Rhett |
| **P24.13** | **done b466740d2 [test:1.0 7c540ddfb]** (2026-04-21 05:54 coord; deployed live; header gap closed from 3,862 → 0 in 81s; blocks_download phase entered). Original description: **Sync stall root cause — `bad-diffbits` on every inbound header batch after FlyClient UTXO snapshot.** Observed live 2026-04-21 02:25 on fresh-binary post-deploy: 12 peers at h≈3,085,141, node at tip h=3,081,601, header_height=3,081,408 (193-block inversion). For every incoming `headers size=87041` batch, `check_block.c:249` rejects header[0] with `bad-diffbits`, subsequent headers cascade-fail with `bad-prevblk`. Mechanism: `GetNextWorkRequired` at `check_block.c:241` returns `nProofOfWorkLimit` (weakest allowed) when its 17-block averaging window cannot be fully walked (the comment at `:231-238` explicitly names this case: "fast-sync snapshot tail ... MUST bypass this function entirely"). But the `skip_contextual` gate in `process_block.c` is NOT active for post-snapshot headers_download — every valid header gets the full GetNextWorkRequired check, returns weakest-allowed, peer's actual nBits != weakest → reject. Entire network cannot feed us past the snapshot tail. **Overlaps P14.11 ("Zero `bad-diffbits` lines" was a P14.13 canary-success signal that has regressed) and P14.15 (nBits backfill).** Fix shape: either (a) extend `skip_contextual` to cover the 193-block post-snapshot tail until block_index contiguous with tip, OR (b) backfill block_index entries 3,081,409..3,081,601 during chain-restore so `GetNextWorkRequired`'s 17-block window can walk successfully, OR (c) gate in `connect_block_local` that triggers a header-backfill phase from legacy peer. Pair with a RED test in `test_chain.c` that boots a fake snapshot tip, feeds real mainnet headers, asserts tip advances without `bad-diffbits`. | `lib/validation/src/check_block.c:224-251`, `lib/validation/src/process_block.c` (skip_contextual), `app/services/src/chain_restore_service.c` (nBits backfill range) | **CRITICAL** | Agent-2 |
| **P24.14** | **`coins_view_cache_get_coins` RPC SEGV class — 16 callers across 5 controllers, not just `rpc_getrawtransaction`.** Symbol-resolved 2026-04-21 05:02 via `nm` on live crash backtrace (stack: `rpc_getrawtransaction+0x4AB` → `coins_view_cache_get_coins+0x1B3` → SIGSEGV → SIGABRT). Broader audit 2026-04-21 05:15 found the same NULL-deref pattern affects 16 RPC callsites: `transaction_controller.c` (2), `chain_inspect_controller.c` (1), `wallet_diagnostic_controller.c` (**10** — single largest blast radius), `wallet_rescan_controller.c` (2), `repair_controller.c` (1). ALL can SIGABRT the live node if triggered against inverted-tail heights (3,081,409..3,081,601 — present in `chain.blocks` but NOT in `block_index` per P24.13 inversion). MCP tools at risk: `zcl_getrawtransaction`, `zcl_walletaudit`, `zcl_listunspent`, `zcl_z_listunspent`, `zcl_rescanblockchain` — all must be treated UNSAFE until fix lands. Distinct from P24.11 (`rpc_getsyncdiag` json_free UAF) — shares only that both are RPC→abort on post-P24.13 inverted state. Evidence: `~/.zclassic-c23/node.log` offset 219298. Fix shape (layered): (a) NULL-check + graceful RPC error in `coins_view_cache_get_coins` return path (single chokepoint), (b) audit each of the 16 callers to confirm they propagate the error instead of dereferencing, (c) lint rule `check_coins_lookup_nullcheck.sh` to catch future sites. Pair with a RED test in `test_rpc_safety.c` (new) that feeds inverted-tail state + fuzzes all 16 MCP tools above, asserts zero SIGABRT. | `lib/storage/src/coins_view_cache.c` (get_coins NULL chokepoint), `app/controllers/src/{transaction,chain_inspect,wallet_diagnostic,wallet_rescan,repair}_controller.c`, `lib/test/src/test_rpc_safety.c` (new) | **CRITICAL** | Agent-2 (after P24.13) |
| **P24.15** | **`assert()` audit — 45 sites in `lib/` + `app/` + `tools/`, no current triage.** 2026-04-21 05:15 sweep found 45 `assert()` calls (separate from the 5 `abort()` sites P24.4 targets). Each assertion in a long-running server is an uptime landmine: any unexpected condition → process exit. Pair with `DEFENSIVE_CODING.md` guidance that `assert()` is for invariants impossible to violate in correct code, not for "should never happen but might" cases (those should return `zcl_result` error instead). Output: (a) categorize all 45 into "true invariant" vs "runtime-possible", (b) refactor runtime-possible sites to return errors, (c) add lint rule `check_assert_discipline.sh` that flags new `assert()` additions for review. Dep: P15.2 (`zcl_result` type) for step (b). | repo-wide + `tools/scripts/check_assert_discipline.sh` (new) | HIGH | Agent-3 (extends P24.4) |
| **P24.16** | **`zcl_kpi` wallet.persistence false-unhealthy when wallet is closed.** Observed live 2026-04-21 05:00+: `wallet.persistence.healthy=false`, `canary_ok=false`, `row_count=-1`, `last_error="sqlite handle closed"` — these are the DEFAULT-INIT values when the wallet sqlite has never been opened (seed path `wallet_canary.c:150`). The node is running fine; it's just that the wallet RPC hasn't been invoked since boot. Misleading for anyone auditing `zcl_kpi` — reads as "wallet broken" when it's really "wallet not yet opened." Fix: either (a) omit `wallet.persistence` from `zcl_kpi` response when `wallet.persistence.open=false` and `wallet.txcount==0`, OR (b) emit `wallet.persistence.state="not_opened"` as a three-value enum (`not_opened` / `healthy` / `unhealthy`). Trivial JSON edit in `app/controllers/src/meta_controller.c` where `zcl_kpi` composites its output. | `app/controllers/src/meta_controller.c`, `lib/wallet/src/wallet_canary.c` | MED | Agent-2 or Agent-3 |
| **P24.17** | **`node.log.old` sweep — 9.6 GB frozen log from 2026-04-10 rotting in datadir.** Observed 2026-04-21 05:15. `~/.zclassic-c23/node.log.old` (9.6 GB) + `node.log.old3` (13 MB) — 11 days stale, never compressed. **Manual first sweep completed 2026-04-21 05:23** — both files moved to `~/zcl-backups/log-sweep-20260421/` (reversible; datadir now clean). This row remains open for the boot-hook automation: extend P24.1 datadir-sweep service to archive `node.log.old*` files on startup. Pair with P24.1. | `app/services/src/datadir_sweep_service.c` | LOW | Rhett (coord) — manual sweep done, automation row remains |

**Parallelism:** P24.1 + P24.7 are coordinator-lane (Rhett ships).
P24.2 + P24.4 are Agent-3 lane (crypto/sapling touches). P24.3 +
P24.5 + P24.6 + P24.8 + P24.10 are Agent-2 lane. P24.9 absorbed into
P22.5 exemption list or split per Agent-2 during P21 wave. None
block on P15–P23; the lint rows (P24.2, P24.3) ideally land first so
they gate later commits.

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
