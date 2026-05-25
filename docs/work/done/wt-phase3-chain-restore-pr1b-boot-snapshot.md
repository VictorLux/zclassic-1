# Worker Assignment — Phase 3 chain_restore PR-1b: extract boot snapshot state

**Worktree:** wt3 / main
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** chain_restore PR-1 planner extract
**Status:** IN PROGRESS — 2026-05-25

## Scope

Extract the boot snapshot diagnostic state out of the planner/service pair
without changing the public chain_restore API. This is a preparatory
chain_restore dissolve slice: the planner should not own diagnostic global
storage, and `chain_restore_service.c` should only update the snapshot via a
separate boundary.

## Owns

- NEW `app/services/include/services/chain_restore_boot_snapshot.h`
- NEW `app/services/src/chain_restore_boot_snapshot.c`
- EDIT `app/services/include/services/chain_restore_service.h`
- EDIT `app/services/include/services/chain_restore_planner.h`
- EDIT `app/services/src/chain_restore_planner.c`
- EDIT `app/services/src/chain_restore_service.c`

## Acceptance

- Existing callers continue to include `chain_restore_service.h`.
- `chain_restore_plan()` still records the last plan result in the boot
  snapshot.
- `chain_integrity_check_post_restore()` and backfill/CSR/snapshot import
  paths still record into the same snapshot.
- `chain_restore_dump_state_json()` remains registered as the `boot`
  dumpstate provider.
- `ZCL_TEST_ONLY=chain_restore_planner ./test_zcl` PASS.
- `ZCL_TEST_ONLY=chain_restore ./test_zcl` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before push.
