# Worker Assignment — Phase 3 header_probe PR-2d: delete stale cadence config

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2c blocking pull delete shipped.
**Status: IN PROGRESS (wt3)** — claimed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` by removing the
stale `cadence_secs` config/state/dump surface from header_probe. Scheduling is
owned by the supervised `header_probe_poll` job, so the service should not
carry a legacy cadence field that no longer drives behavior.

## Tasks

1. Delete `cadence_secs` from `struct header_probe_config`.
2. Remove the unused header_probe cadence default, state field, init/reset
   handling, and JSON dump key.
3. Update header_probe tests and stale comments that still refer to the
   service cadence.

## Acceptance

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS.

## Notes

This is a PR-2 service shrink only. It does not change the poll job cadence or
header validation behavior.
