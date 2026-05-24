# Benchmark Log — measured progress over time

Append-only ledger of the 10 benchmarks in `REFACTOR_STATUS.md`. One row per
measurement. This is the source for the "now" column and the bars on the board
— **measured values, never estimates.**

## How to add a measurement (from Claude Code)

1. Pull live gauges: `zcl_status` (RSS, height, peers, uptime) and `zcl_metrics`.
2. For timing benchmarks, run the harness (only meaningful on a *healthy* node):
   - `#1 cold`  → `make bench-sync` / `tools/bench_cold_start_from_legacy.sh`
   - `#2 warm`  → `zclassic23 -bench-warmstart`
   - `#4 thru`  → `zcl_validationstatus` `blocks_per_sec` during bg-verify
   - `#6 kill-9`→ `tools/bench_no_stuck.sh` recovery histogram
3. Append rows below with today's date + `git rev-parse --short HEAD`.
4. Leave a metric out rather than guess. `—` = not measured this run.
5. Commit. Trend for any metric: `grep "RSS" docs/BENCHMARKS_LOG.md`.

Format: `date | commit | benchmark | value | how measured / notes`

## Measurements

| date | commit | benchmark | value | how / notes |
|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | #3 RSS | 1532 MB | live `zcl_status.memory_rss_mb` (target ≤1000) |
| 2026-05-24 19:15 | 4ea5f5063 | #3 RSS | 1587 MB | +55MB vs ~1h ago; node restarted (uptime 576s) — RSS creeping while stuck |
| 2026-05-24 | be5e90b05 | #9 binary size | **14.6 MB** | `ls` of built binary — docs claim "26 MB"; **discrepancy, verify** (target stay small) |
| 2026-05-24 | be5e90b05 | #1 cold sync | — | not measured (node stuck, no clean restart this session) |
| 2026-05-24 | be5e90b05 | #2 warm restart | — | not measured |
| 2026-05-24 | be5e90b05 | #4 throughput | 0 blk/s | `zcl_validationstatus` — bg-verify idle/complete at local tip |
| 2026-05-24 | be5e90b05 | #6 kill-9 recovery | — | not measured |
| 2026-05-24 | be5e90b05 | #7 MTBF | — | uptime only 990s (recent restart); needs a soak to measure |
| 2026-05-24 | be5e90b05 | #8 operator pages | 6/10 conditions active, 1 critical failing | `zcl_conditions` — self-heal degraded right now |

## Operational snapshot (context for the above, not a benchmark)

| date | commit | height-behind | peers | uptime | errors_total | note |
|---|---|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | 1,905 | 2 | 990s | 16,396 | node stuck: chain-advance blocked, `block_failed_mask_at_tip` failing |
