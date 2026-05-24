# Worker Assignment — Integrate rebuild_recent perf wins into the service

**Worktree:** any (wt2 / wt3)
**Branch:** PUSH DIRECT TO MAIN
**Moves benchmarks:** #1 cold sync, #2 warm restart, #6 kill-9 recovery
**Proven prototype:** `tools/rebuild_recent.c` + `docs/BENCHMARKS_LOG.md`
(whole-chain rebuild 34s → 5.6s). Techniques validated there; this assignment
lands them in the production node, one safe PR at a time.

> Standard for this work: profile before optimizing, use the hardware, verify
> byte-exact output, measure honestly (setup vs hot phase, warm vs cold cache).
> See the "High-performance engineering standard" — it's the bar.

---

## PR-1 — Hardware CRC32C in the production event log  (DONE)

`lib/storage/src/event_log.c` computes its per-event CRC with a **software
table** (`crc32c()`, ~0.3-1 GB/s). The prototype proved this was ~HALF the
rebuild time; this CPU (and the target class) has SSE4.2 hardware CRC32C
(`_mm_crc32_u64`, ~10-20 GB/s). Every event-log append and every recovery scan
pays this, so it speeds warm-restart (#2) and all bulk writes.

**Owns:**
- EDIT `lib/storage/src/event_log.c` — keep the table impl as `crc32c_sw`
  (reference); add an SSE4.2 `crc32c_hw` (8 bytes/step via `_mm_crc32_u64`,
  tail via `_mm_crc32_u8`); make `crc32c()` dispatch to HW when available.
  Gate on a runtime CPUID check (`__builtin_cpu_supports("sse4.2")`) with the
  software path as fallback for non-SSE4.2 hosts.
- At init (once), run a **self-check**: assert `crc32c_hw(buf,n) == crc32c_sw`
  over many lengths; fall back to software on any mismatch. Output MUST stay
  byte-identical — the wire format is frozen and projections validate the CRC.
- `#include <nmmintrin.h>`. `-march=native` is already in CFLAGS, but add the
  runtime guard so a portable build still works.

**MUST NOT:** change the wire format, the polynomial, or the init/final-xor
(0xFFFFFFFF). The value must equal the current table output exactly.

**Acceptance:**
- [x] `ZCL_TEST_ONLY=event ./test_zcl` PASS
- [x] `ZCL_TEST_ONLY=event_log ./test_zcl` PASS
- [x] `ZCL_EVENT_LOG_BENCH=1 ZCL_TEST_ONLY=event_log ./test_zcl` PASS
- [x] `./test_parallel --jobs=$(nproc)` PASS
- [x] Micro-bench note: `event_log: crc32c — impl=hardware-sse4.2 sw=0.60 GB/s active=12.99 GB/s (sink=0)` on this box.

---

## PR-2 — io_uring batched-append mode for bulk event-log writes  (spec; Phase 7a)

The durable appender fsyncs **twice per event** — correct for live operation,
catastrophic for bulk ingest (cold-import / replay). The prototype's bulk
writer (canonical wire format, N buffers in flight via `IORING_OP_WRITE`, ONE
fsync at the end) is ~3000× the per-event path.

**Plan:** add an opt-in **bulk-append session** to the event log
(`event_log_begin_bulk()` / `event_log_bulk_append()` / `event_log_end_bulk()`
which fsyncs once). Use it on the cold-import / replay path only; live appends
keep per-event durability. Reference impl: the `uw` writer in
`tools/rebuild_recent.c`. Raw io_uring via `<linux/io_uring.h>` (no liburing
dep). Folds into the existing Phase 7a plan
(`docs/architecture/phase7-frontier.md` §7a) — this is its first real consumer.

**Gated on:** PR-1 merged + a short soak. Worker splits a `wt-phase7a-*.md`.

---

## PR-3 — Parallel io_uring blk*.dat marking in cold-import  ← PROMOTED, profile in hand

**Measured 2026-05-24 (`941b9803d`, live cold-import recovery):** total ~180s,
of which **`blk*.dat` marking = 101s, single-threaded** ("Block file scan:
3126097 marked … in 101s"). That scan reads every legacy block file to populate
`block_index` BLOCK_HAVE_DATA — it is THE bottleneck and it's embarrassingly
parallel. The prototype already proved the fix on this exact data: independent
io_uring segments + hardware CRC hit ~2 GB/s (NVMe floor), whole chain in 5.6s.

**Target:** turn the 101s scan into seconds by sharding it the way
`tools/rebuild_recent.c` shards (own ring + own range per worker, dynamic
schedule, `nthreads*2` segments). Find the scan in the cold-import path
(`-cold-import` → block-file marking; grep the "Block file scan" / "marked …
created" log emit) and parallelize the per-file mark + header-fix.

**Subtleties to respect (NOT prototype shortcuts):**
- The prototype skips consensus validation (it only reformats). The marking pass
  itself doesn't validate signatures, but it MUST preserve the exact
  block_index/BLOCK_HAVE_DATA semantics and orphan-resolution the serial path
  produces (the import log showed an orphan-pprev resolve + ancestry recompute
  pass — those must still run and agree).
- Shards write disjoint block_index rows; reconcile the ancestry/heights pass
  once at the end (it already runs as a post-step — keep it).
- Measure honestly: report the scan phase separately from wallet backfill + mmb
  (the other ~80s — separate optimization, separate PR if pursued).

**Acceptance:**
- [ ] Cold-import produces a byte-identical bootable datadir (same tip hash,
      same utxo_sha3) as the serial path — diff against a serial run.
- [ ] `blk*.dat` marking phase wall-time recorded before/after in BENCHMARKS_LOG.
- [ ] `./test_parallel --jobs=$(nproc)` PASS.

**No longer gated** — this is independent of PR-2 (different code path: cold-import
marking, not the live event-log appender). Claim it.

---

## Status

**PR-1 COMPLETE (wt2)** — claimed and implemented 2026-05-24.
Benchmark moved: production event-log CRC32C now dispatches to SSE4.2 after a
software-reference self-check; measured active CRC throughput moved from
0.60 GB/s software to 12.99 GB/s hardware on this box.
**PR-3 IN PROGRESS (wt2)** — claimed 2026-05-24; live profile in hand (blk*.dat marking = 101s of
~180s cold-import). PR-2 (io_uring bulk-append) still spec'd.
One commit per task; push direct to main; `./test_parallel` before pushing.

<!-- Worker: append a Completion section with the "Benchmark moved" line. -->

## Completion — PR-1 Hardware CRC32C

Benchmark moved: #2 warm restart and #6 kill-9 recovery scans now use
runtime-dispatched SSE4.2 CRC32C when available; wire format and CRC values
remain byte-identical to the software table reference.

Summary:
- Kept the Castagnoli table path as `crc32c_sw()` and added x86 SSE4.2
  `crc32c_hw()` using `_mm_crc32_u64` plus byte tail handling.
- Added `pthread_once` initialization with runtime `__builtin_cpu_supports`
  detection and HW-vs-SW self-check across many payload lengths, falling back
  to software on mismatch.
- Stabilized the event async lifecycle test so the 32-worker parallel gate
  asserts the documented `event_async_stop()` drain guarantee instead of a
  scheduler-timing window.

Micro-bench on this box:
- Software CRC32C: 0.57 GiB/s
- SSE4.2 CRC32C: 12.36 GiB/s
- Focused event-log dispatch check: 0.60 GB/s software, 12.99 GB/s active

Verification:
- `make -j$(nproc) test_zcl test_parallel` PASS
- `ZCL_TEST_ONLY=event ./test_zcl` PASS
- `make lint` PASS
- `./test_parallel --jobs=$(nproc)` PASS — 199/199 groups, 32 workers
