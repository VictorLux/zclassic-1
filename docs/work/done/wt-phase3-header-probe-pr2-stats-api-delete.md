# Worker Assignment — Phase 3 header_probe PR-2e: delete public stats snapshot API

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2d cadence config delete shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` and its public
header by deleting the standalone `header_probe_stats` snapshot API. The
service already exposes the same operator state through
`header_probe_dump_state_json`, so tests and nearby service glue should read
that existing state surface instead of depending on a second public stats
contract.

## Tasks

1. Delete `struct header_probe_stats` and `header_probe_stats_snapshot()` from
   `app/services/include/services/header_probe_service.h`.
2. Remove the snapshot function implementation and have
   `header_probe_dump_state_json()` read atomics directly.
3. Move `legacy_mirror_sync_service` and focused tests to
   `header_probe_dump_state_json()`.

## Acceptance

- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=rpc ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is a PR-2 public-surface shrink only. It does not change RPC transport,
header validation, header admission publication, or the poll job cadence.

## Completion

- Deleted the public `header_probe_stats` struct and
  `header_probe_stats_snapshot()` API.
- Kept `header_probe_dump_state_json()` as the single state surface and made
  it initialize its output object before appending fields.
- Moved `legacy_mirror_sync_service` and focused tests to read the existing
  JSON state fields.

## Verification

- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=rpc ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS (`0/208 groups failed`).
