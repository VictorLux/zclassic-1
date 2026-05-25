# Worker Assignment — Phase 3 header_probe PR-2e: delete dump config echo

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2d cadence config delete shipped.
**Status: IN PROGRESS (wt3)** — claimed 2026-05-25.

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
