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

## PR-1 — Hardware CRC32C in the production event log  ← CLAIM FIRST (READY)

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
- [ ] `ZCL_TEST_ONLY=event ./test_zcl` (event-log fingerprint/round-trip) PASS
- [ ] `./test_parallel --jobs=$(nproc)` PASS
- [ ] A micro-bench or note showing HW vs SW CRC throughput on this box.

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

## PR-3 — Parallel segmented rebuild for cold-import / fast-sync  (spec; Phase 8)

The prototype hit ~2 GB/s (NVMe write floor) by sharding the chain into
independent segments, each with its own io_uring ring + output file — this is
the **Phase 8 segmentation** shape. Wire this into the cold-import / fast-sync
path so a fresh node reaches tip in seconds (benchmark #1: 145s → target 60s,
stretch 30s).

**Subtleties to respect (NOT prototype shortcuts):**
- The prototype skips consensus validation (it only reformats). Production
  cold-import must preserve validation semantics OR only use the fast path for
  the already-trusted SHA3-snapshot range, delta-validating the tail.
- Output as segments aligns with `docs/architecture/phase8-log-compaction-and-retention.md`;
  if a single `event.log` is required, add the offset-fixup concat pass
  (shift each segment's sentinel offsets by its file position).

**Gated on:** PR-2 + Phase 8 segmented-log reader. Bigger; spec to be expanded.

---

## Status

**PR-1 IN PROGRESS (wt2)** — claimed 2026-05-24.
PR-2/PR-3 spec'd, gated.
One commit per task; push direct to main; `./test_parallel` before pushing.

<!-- Worker: append a Completion section with the "Benchmark moved" line. -->
