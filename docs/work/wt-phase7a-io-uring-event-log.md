# Worker Assignment — Phase 7a: io_uring on the event_log hot path

**Worktree:** wt2 OR wt3 (either)
**Branch:** push DIRECT TO MAIN — no PR
**Phase:** 7 (Frontier — OPTIONAL)
**Depends on:** Phase 4a event_log primitive SHIPPED ✅ (`76b3a10b4`); recovery
scan is the safety net for this rewrite.
**Status: DRAFT — DEFER.** Do NOT dispatch until user explicitly approves
Phase 7 work. The current `pwrite` + `fsync` path is correct, deterministic,
and *fast enough*. This spec is here so we can move quickly if/when the user
wants the throughput.

**Owns:**
- EDIT `lib/storage/src/event_log.c` — replace the write path's `pwrite` +
  `fsync` calls with an io_uring submission/completion queue per writer.
- NEW `lib/platform/src/io_uring_wrap.c` + header — a thin C wrapper around
  `liburing` so the event_log doesn't depend on `liburing.h` directly
  (keeps the abstraction reversible).
- EDIT `lib/test/src/test_event_log.c` — add stress test: 1M append-fsync
  iterations, verify throughput + recovery scan still passes.
- EDIT `Makefile` — link against `-luring` (gated behind a `WITH_IO_URING=1`
  variable, default OFF; FAIL if requested but liburing not present).

**MUST NOT touch:**
- `lib/storage/include/storage/event_log.h` — wire format is frozen.
- Any consumer of the event_log (projections, controllers).
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`.
- Wave S, Phase 4 projections, Phase 5/6 work.

---

## Why this matters

The event log is the spinal column of Phase 4. Every write to a projection
flows through it. The current implementation is correct and recovery-safe,
but caps at ~50K appends/sec due to one syscall round-trip per event.

io_uring keeps the same on-disk format (so recovery scans + the existing
audit tooling still work), but submits multiple appends in one batch and
gets a single signal on completion. Microbenchmarks in the kernel literature
suggest 4-8× throughput on this workload.

**This only matters if we're write-bound.** Today we're not. So this is
DEFER until profiling shows the event log is a bottleneck (current
profiling does NOT show this; the bottleneck is signature validation, which
Phase 5a-3 is already attacking).

---

## Design

### Wire format: UNCHANGED

The event log's on-disk format is frozen (header[16] + payload + CRC32C[4] +
sentinel[8]). The only thing that changes is HOW the bytes get to the disk.

### Submission strategy

Each writer thread holds its own io_uring SQ/CQ pair (so we avoid lock
contention on a shared ring). Appends are submitted with `IORING_OP_WRITE` +
`IOSQE_IO_LINK` chained to `IORING_OP_FSYNC` so the sentinel-after-fsync
invariant is preserved per submission.

### Fallback

If `WITH_IO_URING=0` (default in Phase 7a-pilot) OR `io_uring_queue_init`
fails at runtime, the event_log falls back to the existing `pwrite` + `fsync`
path. Two implementations live side-by-side until a user-facing flag
deprecates the legacy path (probably never).

### Risk: kernel CVEs

io_uring has had multiple kernel CVEs (CVE-2023-21400, CVE-2024-1086, etc).
Mitigation:
1. The wrapper `lib/platform/src/io_uring_wrap.c` includes a kernel-version
   check at startup. Refuses to enable io_uring on kernels older than
   `6.6.0` (the version with all known CVEs fixed at time of writing).
2. The MCP tool `zcl_state subsystem=event_log` reports which path is
   active so operators can verify.
3. Default OFF. Operators must explicitly opt in.

---

## Tasks (in order)

### Task 1: Wrapper

NEW `lib/platform/include/platform/io_uring_wrap.h`:

```c
/* Minimal wrapper so the event_log doesn't depend on liburing.h directly. */
typedef struct iouring_writer iouring_writer_t;

iouring_writer_t *iouring_writer_open(int fd, size_t queue_depth);
int  iouring_writer_append_fsync(iouring_writer_t *w, const void *buf,
                                 size_t len, off_t off);
void iouring_writer_close(iouring_writer_t *w);

/* Returns true iff io_uring is available AND the kernel is >= 6.6.0. */
bool iouring_supported(void);
```

NEW `lib/platform/src/io_uring_wrap.c` — implements the above. Kernel
version check via `uname(2)` parse. Queue init via `io_uring_queue_init`.
Append via SQE chain (WRITE + FSYNC linked).

**Acceptance:** unit test `lib/test/src/test_io_uring_wrap.c` that opens a
temp file, appends 10K events, closes, re-opens with the legacy path,
verifies all 10K are readable + CRC valid.

### Task 2: event_log opt-in

EDIT `lib/storage/include/storage/event_log.h` — add one field to the
opener:

```c
struct event_log_open_opts {
    const char *path;
    bool use_io_uring;       /* default false */
    size_t io_uring_depth;   /* default 256 */
};
event_log_t *event_log_open_v2(const struct event_log_open_opts *opts);
```

The existing `event_log_open(path)` stays as a thin wrapper calling
`event_log_open_v2({.path = path, .use_io_uring = false})`. Callers don't
have to change.

### Task 3: Implementation in event_log.c

Add a function pointer `int (*append_fn)(event_log_t*, const void*, size_t,
off_t)` in the event_log struct. Production path stays as
`pwrite_fsync_path`; io_uring path is `iouring_path` (set if
`opts->use_io_uring && iouring_supported()`).

### Task 4: Boot wiring

EDIT `config/src/boot_services.c`:
- Add CLI flag `-event-log-io-uring` (default OFF).
- Read `IOURING_DEPTH` env var (default 256).
- Pass to `event_log_open_v2`.

### Task 5: zcl_state dumper

Add a field to `event_log_dump_state_json`: `"write_path": "pwrite" |
"io_uring"`. Operators check via `zcl_state subsystem=event_log`.

### Task 6: Stress test

NEW `lib/test/src/test_event_log_io_uring.c` — only compiled when
`WITH_IO_URING=1`. 1M append-fsync iterations, measure throughput,
assert > 100K/sec (legacy floor is 50K/sec). Recovery scan must pass.

### Task 7: Documentation

EDIT `docs/architecture/phase7-frontier.md` — flip section 7a status from
"draft" to "shipped"; add throughput numbers.

EDIT `DEFENSIVE_CODING.md` — add note: "If using io_uring path, kernel
must be ≥ 6.6.0. The runtime check refuses to enable on older kernels."

### Task 8: Verify + push

```bash
make -j$(nproc)
WITH_IO_URING=1 make -j$(nproc)   # double-build
make lint
./test_parallel --jobs=$(nproc)
WITH_IO_URING=1 ./test_parallel --jobs=$(nproc)
git pull --rebase origin main
git push origin main
```

Append Completion section to this file with throughput numbers.

---

## Acceptance

- Legacy path: identical behavior (no test regressions).
- io_uring path: ≥ 100K appends/sec sustained on a tmpfs file.
- Recovery scan passes on a partial-trailing-write torture run for both
  paths.
- Kernel < 6.6.0: `iouring_supported()` returns false; opening with
  `use_io_uring = true` falls back with a `LOG_WARN` line.

---

## What this does NOT do

- Does NOT touch the on-disk wire format.
- Does NOT change the recovery contract.
- Does NOT make io_uring the default — opt-in via flag.
- Does NOT alter consumer code (projections, etc).

---

## Commit cadence

One commit per task. Push after Task 6.

---

## Status

**DRAFT — DEFER.** Do NOT claim until user explicitly approves Phase 7
dispatch. Estimated benefit: 2-5× event log throughput, useful if/when
we become ingest-bound.
