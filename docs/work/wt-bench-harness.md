# wt-bench-harness — `make bench`: measure performance honestly (Wave B)

## Status

**IN PROGRESS (wt3) — ⚠ BUILD IT IN C, NOT SHELL (orchestrator redirect 2026-05-25).**
Fully independent of the P0 connect_block work.

> **Canonical functionality lives in C, in the binary — shell is thin dev glue
> only.** `zcl_benchmark` (MCP) already exists and `USER_BENCHMARKS.md` already
> specs `zclassic23 -bench-coldstart`. So: implement the 5 primaries as `-bench-*`
> subcommands + extend the `zcl_benchmark` MCP tool, write `bench-history.csv`
> from C, and **DELETE the redundant `tools/bench_*.sh` scripts as you replace
> them** (`bench_running_lag.sh`, `bench_no_stuck.sh`, `bench_fresh_sync.sh`,
> `bench_cold_*.sh`). Net scripts + LOC must go DOWN. "Less is more." The shell
> sections below are superseded — treat them as the behaviour spec, implement in C.

> This is the foundation the "high-performance" goal has been missing. Every perf
> number we've quoted (cold 180s, warm 37.7s, RSS 2.4 GB) came from ad-hoc manual
> runs. No harness, no baseline, no regression gate. Build it. Profile-before-
> optimize ([[feedback_high_perf_engineering_standard]]) is impossible without it.

## Why it's safe to run in parallel with the P0 fix
- Touches benchmark entry points in `main.c`, `zcl_benchmark` metadata,
  `docs/bench-history.csv`, a CI hook, and a small additive `make bench`
  target. **No connect/coins/boot source that wt2 edits.**
- Runs against an **isolated** datadir + ports (e.g. `$ZCL_BENCH_DIR`,
  `-rpcport=28232`). It must **never** touch `~/.zclassic-c23` or the live service.
- Where a benchmark needs a healthy node (the "to-tip" numbers) and the live node
  is still wedged, capture what runs now and mark the rest `baseline pending root
  fix` — the harness is the deliverable, the full baseline follows the P0 fix.

## The 5 primaries (from `docs/USER_BENCHMARKS.md` — that doc is the spec)
| # | Benchmark | Target | Runs now? |
|---|---|---|---|
| 1 | Cold-start to operational (empty datadir → tip) | ≤60s | partial (cold-import time runs; to-tip pending P0) |
| 2 | Warm-start to operational (restart synced → RPC ready) | ≤10s | yes (against an isolated synced datadir) |
| 3 | Stay-in-sync MTBF (unattended, chaos) | ≥30d | harness + short smoke now; full soak later |
| 4 | RAM steady-state (RSS over a soak) | ≤1 GB | yes (sample `/proc/<pid>/status` over time) |
| 5 | Recovery from kill -9 (→ RPC ready) | ≤60s | yes (scripted kill loop, histogram) |

Canonical functionality lives in C. `make bench` is thin glue over
`zclassic23 -bench`; `zcl_benchmark` exposes the same primary list via MCP/RPC.

## Tasks
1. `zclassic23 -bench` — one entry point, runs all 5 primaries against an
   isolated datadir/ports when prerequisites exist, prints a table, appends a row to
   `docs/bench-history.csv` (columns: date, commit, bench#, value, unit, notes).
2. `make bench` target (additive, minimal Makefile diff) → runs `zclassic23 -bench`.
3. CI regression gate: a script that fails if any primary regresses >20% vs the
   last committed `bench-history.csv` row for that bench. Wire into `make ci`.
4. Capture today's baseline rows for the benchmarks that run now; mark the
   to-tip ones `pending P0`. Commit the csv.
5. Doc: 6-line "how to read bench-history.csv" header in the csv or a sibling md.

## Acceptance
- `make bench` runs end-to-end on an isolated datadir, touches nothing in
  `~/.zclassic-c23` or the live systemd service (prove it: show the bench datadir
  path and that the live node's height is unchanged across a bench run).
- `docs/bench-history.csv` has at least the runnable baselines committed.
- The regression gate fails a deliberately-slowed run and passes a normal one.
- `make test_parallel` clean, `make lint`.

## Non-goals
- Optimizing anything yet — this measures. Optimizations come after, gated on a
  baseline so the win is provable (the standard: profile, then optimize, then
  re-measure honestly).
- Touching the live node or any file in wt2's connect/coins/boot scope.

## References
- `docs/USER_BENCHMARKS.md` (the 5-number spec), `docs/BENCHMARKS_LOG.md` (ledger).
- `zclassic23 -bench`, `zclassic23 -bench-regress`, `zcl_benchmark`.
- Memory: [[feedback_high_perf_engineering_standard]], [[feedback_dont_sell_clear_info]] (numbers must be measured, dated, honest).
