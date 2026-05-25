# Worker Assignment — Phase 3 header_probe PR-3: rename core module

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2 core shrink shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Complete the header_probe dissolve rename slice by deleting the old
`header_probe_service.{h,c}` filenames and moving the smaller core to
`services/header_probe.h` plus `app/services/src/header_probe.c`. This keeps
the existing public function names and behavior, but removes the stale
mega-module filename now that the implementation is at the PR-2 target size.

## Tasks

1. Rename `app/services/include/services/header_probe_service.h` to
   `app/services/include/services/header_probe.h`.
2. Rename `app/services/src/header_probe_service.c` to
   `app/services/src/header_probe.c`.
3. Update production, job, condition, diagnostics, boot, and focused test
   includes to the new header.
4. Rename the focused test entry point away from the old service filename.
5. Remove stale source comments that point to the deleted filename.

## Acceptance

- No production or test source includes `services/header_probe_service.h`.
- No active source references `header_probe_service`.
- `app/services/src/header_probe_service.c` no longer exists.
- `make -j$(nproc) test_zcl test_parallel` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is a rename/delete slice only. It does not change header validation,
legacy RPC fetch behavior, poll cadence, diagnostics fields, or header admit
mailbox publication.

## Completion

- Renamed `app/services/include/services/header_probe_service.h` to
  `app/services/include/services/header_probe.h`.
- Renamed `app/services/src/header_probe_service.c` to
  `app/services/src/header_probe.c`.
- Updated production, job, condition, diagnostics, boot, and focused test
  includes to the new header.
- Renamed the focused test entry point to `test_header_probe()`.
- Confirmed active source no longer references `header_probe_service`.

## Verification

- `make -j$(nproc) test_zcl test_parallel` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS (`0/208 groups failed`).
