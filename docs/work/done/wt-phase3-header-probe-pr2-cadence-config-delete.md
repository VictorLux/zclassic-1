# Worker Assignment — Phase 3 header_probe PR-2d: delete stale cadence config

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2c blocking pull delete shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

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

## Completion — 2026-05-25

Pushed commits:

- `957ac1b04` — `wt3: claim header probe cadence config delete slice`
- `5e6963493` — `header_probe: delete stale cadence config`

Summary:

- Deleted `cadence_secs` from `struct header_probe_config`.
- Removed the unused header_probe cadence default, global state field,
  init/reset handling, and state dump JSON key.
- Updated header probe tests and the poll job comment so scheduling ownership
  is only described in `header_probe_poll`.
- Reduced `app/services/src/header_probe_service.c` to 767 lines.

Verification:

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS — 0/208 groups failed.

Follow-up:

- Continue PR-2 shrink by trimming remaining header_probe stats/state surface
  or moving legacy-mirror-specific orchestration out of the service.
