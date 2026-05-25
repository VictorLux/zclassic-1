# Worker Assignment - Phase 3 header_probe PR-3a: drop service suffix

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2f legacy header client extraction shipped.
**Status: DONE (wt3)** - claimed and completed 2026-05-25.

## Scope

Continue the header_probe dissolve by retiring the stale `_service` file and
include names now that the module has shrunk below the PR-2 core target and no
longer owns background service scheduling. Keep the existing C API function
names stable for callers, but make the source/header names match the current
shape: a small header probe primitive driven by the supervised poll Job.

## Tasks

1. Rename `app/services/src/header_probe_service.c` to
   `app/services/src/header_probe.c`.
2. Rename `app/services/include/services/header_probe_service.h` to
   `app/services/include/services/header_probe.h` and update include sites.
3. Remove stale `_service.c` comments/baseline entries that only existed for
   the old long-running service shape.

## Acceptance

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is a rename/surface cleanup only. It does not change RPC transport,
consensus validation, poll cadence, dump JSON, or mailbox publication.

## Completion

- Renamed `app/services/src/header_probe_service.c` to
  `app/services/src/header_probe.c`.
- Renamed `app/services/include/services/header_probe_service.h` to
  `app/services/include/services/header_probe.h` and updated include sites.
- Removed the stale header_probe supervisor baseline entry; the module no
  longer owns long-running service scheduling.
- Kept `app/services/src/header_probe.c` at 378 lines.

## Verification

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS (existing raw-controller-SQL WARN list only).
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS (`0/208 groups failed`).
