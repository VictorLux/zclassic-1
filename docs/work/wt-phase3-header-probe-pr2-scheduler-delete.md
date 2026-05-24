# Worker Assignment — Phase 3 header_probe PR-2a: delete legacy scheduler

**Worktree:** wt2
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-1 (`header_probe_poll` Job) shipped.
**Status: IN PROGRESS (wt2)** — claimed 2026-05-24.

## Why This Slice

`header_probe_poll` is now registered under the net supervisor during boot,
so `header_probe_service.c` no longer needs its old heartbeat-ring scheduler
API. Removing the deprecated scheduler is the first low-risk shrink in
`docs/dissolve/header_probe_service.md` PR-2 before larger peer-scoring and
file-rename work.

## Owns

- EDIT `app/services/include/services/header_probe_service.h`
- EDIT `app/services/src/header_probe_service.c`
- EDIT `lib/test/src/test_header_probe_service.c`
- UPDATE this file with completion notes.

## Tasks

1. Delete the public `header_probe_start()` / `header_probe_stop()` API and
   the private heartbeat callback wrapper from `header_probe_service.c`.
2. Keep `header_probe_tick_once()` unchanged; it is the body used by the
   supervised `header_probe_poll` Job.
3. Replace the old start/stop unit coverage with a direct `tick_once` test
   that proves under-lag polling stays quiet.
4. Verify:
   - `make -j$(nproc) test_zcl test_parallel`
   - `ZCL_TEST_ONLY=header_probe ./test_zcl`
   - `ZCL_TEST_ONLY=header_probe_poll ./test_zcl`
   - `./test_parallel --jobs=$(nproc)`
   - `make lint`

## Must Not Touch

- `header_probe_pull_range_blocking()`; `legacy_mirror_sync_service` still
  calls it.
- RPC request/response parsing or validation semantics.
- Peer scoring migration; that belongs to a later PR-2 slice.

## Status

**IN PROGRESS (wt2)** — claimed 2026-05-24.

<!-- Worker: append Completion below when done. -->
