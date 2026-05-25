# Worker Assignment - Phase 3 chain_restore PR-3: delete service shell

**Status:** DONE
**Branch:** main
**Depends on:** chain_restore PR-2b repair extraction shipped

## Scope

Remove the remaining `chain_restore_service.c` implementation shell by moving
its last responsibilities into focused modules:

- `chain_restore_integrity.{h,c}` for validation and post-restore integrity.
- `chain_restore_boot_activation.{h,c}` for the boot activation decision.

Keep `chain_restore_service.h` as a compatibility umbrella for the current
call sites. The next slice can replace those includes with specific headers
and delete the umbrella.

## Completion

`app/services/src/chain_restore_service.c` is deleted. The public repair,
executor, integrity, boot activation, planner, and boot snapshot APIs now live
in their focused headers and implementations.

## Verification

- `make -j$(nproc) test_zcl test_parallel` - PASS
