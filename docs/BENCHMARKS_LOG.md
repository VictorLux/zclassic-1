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

## Native rebuild benchmark (`rebuild_recent` tool)

| date | commit | N blocks | rebuild ms | blocks/s | bytes | notes |
|---|---|---|---|---|---|---|
| 2026-05-24 | (tool) | 10 | 339 | 29 | 14,590 | v1, durable event_log appender. **fsync-bound** (fsync×2/event). |
| 2026-05-24 | (tool) | ALL (3,123,618) | 34,180 | 91,387 | 11.25 GB | **io_uring** bulk writer (8 buffers in flight, 1 fsync at end). 5.1M tx, 11.4M utxo-adds, 27.7M events, short_writes=0. 329 MB/s. setup +5.4s (snapshot 1.9s + index 3.5s). ~3000× the v1 write path. **This is the kept version.** |

### Parallelization experiment (NEGATIVE result — reverted)

Tried OpenMP parallel parse+CRC-framing with a single `ordered` writer feeding io_uring. Result: **no speedup; threads hurt.** Whole-chain REBUILD by thread count: 1t=34.2s, 4t=34.4s (flat), 8t=79.8s, 32t=64.8s. Output stayed byte-valid. Conclusion: the rebuild is **not CPU-parse-bound** — it's bound by the serial write path (11 GB memcpy + crc + io in the ordered region) and malloc-arena contention in `block_deserialize` across threads (Amdahl: large serial fraction). Reverted to single-threaded io_uring. Real levers to go below 34s: zero-copy submit of worker buffers (drop a memcpy) + a per-thread block-parse arena/pool (kill malloc contention) — bigger surgery, deferred.

## Operational snapshot (context for the above, not a benchmark)

| date | commit | height-behind | peers | uptime | errors_total | note |
|---|---|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | 1,905 | 2 | 990s | 16,396 | node stuck: chain-advance blocked, `block_failed_mask_at_tip` failing |
