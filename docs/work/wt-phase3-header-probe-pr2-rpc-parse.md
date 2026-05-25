# Worker Assignment — Phase 3 header_probe PR-2b: share JSON-RPC result parsing

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** `wt-phase3-header-probe-pr2.md` transport slice shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` by moving the
generic JSON-RPC response parsing helpers into `lib/rpc/legacy_rpc_client`.
The header probe service should keep only header-specific orchestration,
validation, and stats.

## Tasks

1. Add shared helpers to `lib/rpc/include/rpc/legacy_rpc_client.h` and
   `lib/rpc/src/legacy_rpc_client.c`:
   - parse a string result
   - parse an integer result
   - parse an array of string results
2. Replace `header_probe_service.c`'s local JSON-RPC result parsers with the
   shared helpers.
3. Verify focused header probe tests, RPC/client-adjacent tests, lint, and
   full parallel.

## Acceptance

- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=peer_scoring ./test_zcl` PASS.
- `make -j$(nproc)` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is still PR-2 shrink work. It does not rename the service, remove the
blocking pull API, or change header validation behavior.

## Completion — 2026-05-25

Pushed commits:

- `86428faa4` — `wt3: claim header probe rpc parse slice`
- `c0966304e` — `header_probe: share rpc result parsers`

Summary:

- Added typed shared JSON-RPC result parsers to `legacy_rpc_client` for int,
  string, and string-array batch results.
- Replaced header_probe's private JSON-RPC result parsers with the shared
  helpers.
- Reduced `app/services/src/header_probe_service.c` to 842 lines, from 963
  before the PR-2b slice.

Verification:

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=peer_scoring ./test_zcl` ran the broad suite; the
  `peer_scoring` section passed, but the broad process exited with existing
  unrelated failures in postmortem signal-child assertions and wallet
  projection model-shadow checks.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS — 0/208 groups failed.

Follow-up:

- Continue PR-2 shrink work by moving any remaining header-specific stats or
  scheduling glue out of `header_probe_service.c`, then retire the blocking
  pull API once callers are async.

Follow-up verification after adding narrow `rpc` and `peer_scoring`
selectors:

- `make -j$(nproc) test_zcl test_parallel` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=peer_scoring ./test_zcl` PASS.
- `ZCL_TEST_ONLY=rpc ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS.
