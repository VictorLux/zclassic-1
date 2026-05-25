# Worker Assignment — Phase 3 header_probe PR-2e: delete dump config echo

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2d cadence config delete shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` by removing
header_probe's config echo from `header_probe_dump_state_json()`. Runtime
configuration stays internal to the service; dumpstate should expose
operational state and counters that help diagnose header catch-up.

## Tasks

1. Remove duplicated `running` from header_probe dump output; `initialized`
   already describes the service lifecycle state.
2. Remove RPC/config echo keys from header_probe dump output:
   `rpc_host`, `rpc_port`, `have_user`, `have_password`, `batch_size`, and
   `lag_threshold`.
3. Add a focused header_probe dump contract test for the remaining operational
   keys and absence of config echo keys.

## Acceptance

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS.

## Notes

This is a PR-2 state-surface cleanup only. It does not change header probing,
RPC credentials, polling cadence, or header validation behavior.

## Completion — 2026-05-25

Pushed commits:

- `2114bc326` — `wt3: claim header probe dump config delete slice`
- `8bef07f9f` — `header_probe: narrow dump state surface`

Summary:

- Removed duplicated `running` and RPC/config echo keys from
  `header_probe_dump_state_json()`.
- Kept dumpstate focused on `initialized`, counters, and observed header tips.
- Made direct `header_probe_dump_state_json()` calls initialize their output
  object before adding keys.
- Added a focused header_probe dump contract test covering the remaining keys
  and removed config echo fields.
- Kept `app/services/src/header_probe_service.c` at 744 lines after adding the
  direct-dump initialization.

Verification:

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS — 0/208 groups failed.

Follow-up:

- Continue PR-2 shrink by moving legacy-mirror-only header observation out of
  the public header_probe stats surface or extracting remaining core helpers.
