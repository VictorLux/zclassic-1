# Worker Assignment - Phase 3 chain_restore PR-2a: extract executor

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** chain_restore PR-1 planner extraction shipped.
**Status: IN PROGRESS (wt3)** - claimed 2026-05-25.

## Scope

Continue the chain_restore dissolve by moving the execution/apply surface out
of `chain_restore_service.c` into a dedicated executor module. This keeps the
public `chain_restore_service.h` API stable while separating the mutable
restore path from validation, boot snapshots, and post-restore repair helpers.

## Tasks

1. Add `app/services/src/chain_restore_executor.c`.
2. Move `chain_restore_create_anchor()`, `chain_restore_execute()`, and the
   private CSR commit helpers into the executor module.
3. Leave `chain_restore_service.c` owning validation, dumpstate, boot snapshot,
   integrity checks, and finalization.
4. Keep behavior and public declarations unchanged.

## Acceptance

- `make -j$(nproc)` PASS.
- `ZCL_TEST_ONLY=chain_restore ./test_zcl` PASS.
- `ZCL_TEST_ONLY=chain_restore_planner ./test_zcl` PASS.
- `make lint` PASS.
- `git diff --check` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is a code-motion slice only. It does not add resumable reorg jobs or
change anchor semantics, CSR commits, disk backfill, or integrity validation.
