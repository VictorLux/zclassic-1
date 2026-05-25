# Worker Assignment — Phase 3 header_probe PR-2f: extract legacy header RPC client

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-2e stats API delete shipped.
**Status: DONE (wt3)** — claimed and completed 2026-05-25.

## Scope

Continue shrinking `app/services/src/header_probe_service.c` toward the PR-2
core target by extracting legacy zclassicd header-fetching RPC helpers into
`lib/rpc`. The service should own header probe orchestration, counters,
validation, and mailbox publication; the reusable RPC module should own
`getblockcount`, `getblockhash`, `getblockheader(false)`, batching, hex decode,
and block-header deserialization.

## Tasks

1. Add a small `legacy_header_client` API under `lib/rpc`.
2. Move the private request builders, batch RPC helpers, and header hex
   deserialization out of `header_probe_service.c`.
3. Rewire `header_probe_pull_range()` and `header_probe_tick_once()` through
   the new helper API without changing validation or mailbox behavior.

## Acceptance

- `make -j$(nproc) test_zcl test_parallel` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=rpc ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is still PR-2 shrink work. It does not rename the service, change
consensus validation, change the supervised poll cadence, or alter header admit
mailbox publication.

## Completion

- Added `lib/rpc/include/rpc/legacy_header_client.h` and
  `lib/rpc/src/legacy_header_client.c`.
- Moved legacy `getblockcount`, `getblockhash`, `getblockheader(false)`,
  batched header fetching, hex decode, and block-header deserialization out of
  `header_probe_service.c`.
- Rewired `header_probe_pull_range()` and `header_probe_tick_once()` through
  the helper while keeping local consensus validation and header-admit
  publication in the service.
- Reduced `app/services/src/header_probe_service.c` to 392 lines.

## Verification

- `make -j$(nproc) test_zcl test_parallel` PASS.
- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `ZCL_TEST_ONLY=rpc ./test_zcl` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS (`0/208 groups failed`).
