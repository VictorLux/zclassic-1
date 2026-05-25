# Worker Assignment — Phase 3 header_probe PR-2c: delete blocking pull API

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2b RPC parser extraction shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` by removing the
public `header_probe_pull_range_blocking()` helper. The only production caller
is `legacy_mirror_sync_service.c`, and it already knows the remote header
height from its `getblockchaininfo` request, so it can own the bounded drain
loop while continuing to call the non-blocking `header_probe_pull_range()`.

## Tasks

1. Move the bounded "drain headers until target or stalled" loop into
   `legacy_mirror_sync_service.c`.
2. Delete `header_probe_pull_range_blocking()` from the header probe public
   header and implementation.
3. Update stale comments that describe header_probe as owning the boot-time
   blocking prelude.

## Acceptance

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=zclassicd_oracle ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS.

## Notes

This is still PR-2 shrink work. It does not rename the service or change the
header validation path; legacy_mirror still reaches headers through
`header_probe_pull_range()`.

## Completion — 2026-05-25

Pushed commits:

- `f5c0aee24` — `wt3: claim header probe blocking delete slice`
- `a174ca65f` — `header_probe: delete blocking pull wrapper`

Summary:

- Deleted `header_probe_pull_range_blocking()` from the header probe public
  header and implementation.
- Added a private bounded header-drain helper to `legacy_mirror_sync_service.c`
  that uses the remote header target already fetched from zclassicd.
- Kept legacy_mirror on `header_probe_pull_range()`, so header validation and
  insertion still use the same service path.
- Reduced `app/services/src/header_probe_service.c` to 774 lines.

Verification:

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=zclassicd_oracle ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS — 0/208 groups failed.

Follow-up:

- Continue PR-2 shrink by trimming remaining header_probe stats/state surface
  or moving more legacy-mirror-specific orchestration out of the service.
