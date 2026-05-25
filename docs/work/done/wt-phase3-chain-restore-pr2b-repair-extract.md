# Worker Assignment - Phase 3 chain_restore PR-2b: extract repair

**Status:** DONE
**Branch:** main
**Depends on:** chain_restore PR-2a executor extraction shipped

## Scope

Move the post-restore repair surface out of `chain_restore_service.c` into
`chain_restore_repair.{h,c}` without changing existing callers. Keep
`chain_restore_service.h` as the compatibility include while the final
service shell remains.

## Files

- NEW `app/services/include/services/chain_restore_repair.h`
- NEW `app/services/src/chain_restore_repair.c`
- EDIT `app/services/include/services/chain_restore_service.h`
- EDIT `app/services/src/chain_restore_service.c`
- EDIT `docs/REFACTOR_STATUS.md`
- EDIT `docs/dissolve/chain_restore_service.md`

## Completion

Moved the active-chain rebuild, nBits backfill, consensus-backed checks,
failed-flag cleanup, nearest-backed-ancestor helpers, and finalize flow into
`chain_restore_repair.{h,c}`. `chain_restore_service.c` now retains only:

- `chain_restore_validate`
- `chain_integrity_check_post_restore`
- `boot_should_activate_chain`

`chain_restore_service.c` is 174 LOC after this extraction.

## Verification

- `make -j$(nproc) test_zcl test_parallel` - PASS
