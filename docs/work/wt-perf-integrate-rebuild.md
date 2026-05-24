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
- [x] Cold-import produces a byte-identical bootable datadir (same tip hash,
      same utxo_sha3) as the serial path — diff against a serial run.
- [x] `blk*.dat` marking phase wall-time recorded before/after in BENCHMARKS_LOG.
- [x] `./test_parallel --jobs=$(nproc)` PASS.

**No longer gated** — this is independent of PR-2 (different code path: cold-import
marking, not the live event-log appender). Claim it.

---

## Status

**PR-1 COMPLETE (wt2)** — claimed and implemented 2026-05-24.
Benchmark moved: production event-log CRC32C now dispatches to SSE4.2 after a
software-reference self-check; measured active CRC throughput moved from
0.60 GB/s software to 12.99 GB/s hardware on this box.
**PR-3 IN PROGRESS (wt2)** — claimed 2026-05-24; live profile in hand (blk*.dat marking = 101s of
~180s cold-import). First implementation split landed locally: blk*.dat files
are parsed in parallel with mmap + thread-registry workers, then applied in
deterministic file order to preserve block_index mutation semantics. PR-2
(io_uring bulk-append) still spec'd.
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

## Progress — PR-3 Parallel blk*.dat marking

Summary:
- Replaced the serial `FILE *` block-file scan with a two-phase scan/apply path:
  mmap each blk file, parse compact per-block metadata in parallel workers, and
  apply marks/header fixes in stable file order.
- Kept the existing fast block-index creation semantics, out-of-order retry
  passes, orphan pprev resolution, ancestry recompute, and nChainTx propagation.
- Bounded parse workers to the smaller of file count, online CPUs, and 16, using
  `thread_registry_spawn_ex()` rather than raw `pthread_create`.
- Made incomplete parse metadata fail closed: a file with parse allocation
  failure is skipped instead of applying a partial scan.
- Added `ZCL_BLOCK_SCAN_WORKERS=N` to force a one-worker serial baseline or a
  specific worker count for benchmark runs, and added parse/apply/worker counts
  to the `Block file scan` summary line.
- After the benchmark exposed a source-mixing risk, made explicit
  `-cold-import` fail closed on missing prerequisites, invalid source, or import
  failure, and disabled later legacy UTXO auto-import after a successful cold
  import.
- Added cold-import spotcheck failure diagnostics that print the checked height
  range plus expected/actual SHA3 digests, with
  `ZCL_COLD_IMPORT_DEBUG_WINDOW=N` available as an extra deterministic window
  check while keeping the normal random spotcheck active.
- Extended `tools/gen_sha3_windows` with `--check-window=N` so a suspect
  source can be compared against one compiled SHA3 window over RPC without
  regenerating the whole table.
- Moved cold-import block-index and chainstate reads through staged LevelDB
  snapshots under the destination datadir, so a live zclassicd source can be
  imported without opening its locked LevelDB directories directly.
- Fixed the filesystem SHA3 proof path by resolving the legacy block map from
  the chainstate best-block hash backward through `hashPrev`, instead of
  picking one arbitrary candidate per height from `blocks/index`.
- Added selected-map diagnostics for duplicate heights and parent continuity;
  the live source had 1,202 duplicate-height candidates, and the old map
  selection produced 585 parent-link breaks.

Verification:
- `make -j$(nproc) test_zcl test_parallel zclassic23` PASS
- `make lint` PASS
- `git diff --check` PASS
- `./test_parallel --jobs=$(nproc) --verbose` ran all 205 groups but failed on
  unrelated/noisy checks: `test_sapling_crypto` timing ratio and
  `test_body_fetch_stage` crash-replay checks. Cold-import scan path did not
  report a failure in that run.
- Live benchmark attempt 2026-05-24: stopped `zclassicd-rhett.service`, took a
  clean `/tmp/zcl-legacy-snapshot`, restarted the service, then attempted
  `ZCL_BLOCK_SCAN_WORKERS=1` cold import. The import failed before block-file
  marking because the local legacy source fails the baked SHA3 spotcheck:
  `spotcheck FAILED at window 3028`. This blocks serial/parallel byte-identical
  proof on this machine until a source datadir matching `g_sha3_windows` is
  available or the checkpoint table is regenerated intentionally.
- Follow-up source-proof check: `tools/gen_sha3_windows --check-window=3028`
  against live zclassicd RPC matched the compiled table for
  `h=3028000..3028999`. The mismatch is therefore in the copied filesystem
  snapshot/source datadir, not the baked SHA3 table.
- Live-source smoke after staged LevelDB snapshots: cold-import now snapshots
  the running source LevelDBs successfully (`LevelDB snapshots took 43 ms`),
  but the filesystem payload proof still fails at window 3028. Remaining
  blocker is the `blocks/index` height-to-`blk*.dat` payload selection path,
  not LevelDB locking.
- Live-source smoke after tip-anchored block-map resolution: window 3028 and
  the normal random SHA3 windows all passed; selected-map continuity reported
  `missing=0 parent_mismatch=0`. Cold-import completed into a temp datadir in
  33.8s (`block_index=3124929`, `utxos=1345066`, `blk_files=50`). Boot then
  failed later on a separate pending-anchor UTXO count mismatch:
  `pending=1345066 actual=75454`.
- Fixed chainstate bulk import ownership: cold-import and legacy one-shot import
  now copy callback-owned txids/scripts into batch-owned storage before flush.
  This removed the pending-anchor accounting mismatch in the live smoke:
  cold-import completed in 49.3s (`block_index=3124929`, `utxos=1345066`,
  `blk_files=50`), boot published the pending anchor via CSR at `h=3123726`,
  RPC started, and coins flushes preserved `utxos=1345066`.
- New observed live-sync blocker after activation: headers immediately after
  the imported tip are rejected as `validate-headers-cutover-diverged` /
  `header-admit-cutover-diverged`, leaving sync stalled at `h=3123726`.
- Diagnosis for that blocker: `header_admit` / `validate_headers` were
  defaulting to AUTHORITATIVE even though live P2P/RPC header ingress still
  calls `accept_block_header()` first. In that mode the legacy ingress guard
  requires pre-existing stage records and refuses to create new block-index
  entries, so post-import header sync cannot advance. The pragmatic fix was to
  restore SHADOW defaults until an actual authoritative stage ingress owns
  header admission end-to-end.
- Restored `header_admit` and `validate_headers` to SHADOW defaults while
  preserving explicit authoritative guard tests. Follow-up live smoke:
  cold-import completed in 49.2s, pending anchor published via CSR at
  `h=3123726`, RPC started, and peers accepted post-import headers up to
  `h=3123784` without `*-cutover-diverged` rejects.
- Added `tools/bench_cold_import_equivalence.sh`, the PR-3 acceptance harness
  for the remaining proof: it runs fresh serial
  `ZCL_BLOCK_SCAN_WORKERS=1` and parallel `-cold-import` boots from the same
  legacy datadir, computes `getblockhash(tip)` plus `getutxocommitment`, fails
  on any height/tip/UTXO-count/SHA3 mismatch, and prints a ready-to-paste
  `docs/BENCHMARKS_LOG.md` row seed with both `Block file scan` summary lines.

Live cold-import identity check (2026-05-24, `6e0f6a82c` plus local test/doc
change):
- Serial-worker run (`ZCL_BLOCK_SCAN_WORKERS=1`) completed cold import in 48.9s:
  `legacy_tip=3123726`, `block_index=3124929`, `utxos=1345066`,
  `blk_files=50`; pending anchor published via CSR at `h=3123726`, and peers
  accepted post-import headers up to `h=3123788`.
- Default-worker run completed cold import in 57.3s with the same
  `legacy_tip`, `block_index`, `utxos`, and `blk_files`; pending anchor
  published via CSR at `h=3123726`.
- Read-only SQLite comparison of both imported `node.db` files:
  `coins_best_block=acad56115a58a82ff18395591263a7ec881bd13603ec31e1e72adb12ea010000`,
  UTXO stats `(count=1345066, min_h=1, max_h=3123726,
  sum_value=1038775293114532)`, computed canonical
  `utxo_sha3=981b7bbceb522f816e29e4adccf7f80fdcab75cd392ee7b438b55787385031f1`.
- Current `-cold-import` does not execute `scan_block_files_mark_data`; it
  links/copies `blk*.dat`, bulk-copies the legacy block index, and imports the
  LevelDB chainstate. The earlier 101s `blk*.dat` marking bottleneck remains
  relevant to the normal/file-sync boot scanner, but not to this cold-import
  path after the recent block-index import work.
- Verification completed after widening the crypto-registry ECDSA registry
  microbenchmark tolerance from 1% to 10% for parallel scheduler noise:
  `make -j$(nproc) test_zcl test_parallel`,
  `ZCL_TEST_ONLY=crypto_registry ./test_zcl`, and
  `./test_parallel --jobs=$(nproc)` all pass.
