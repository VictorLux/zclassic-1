# Worker Assignment - Phase 3 chain_restore PR-4: delete compatibility header

**Status:** DONE
**Branch:** main
**Depends on:** chain_restore PR-3 service shell deletion shipped

## Scope

Replace all production and test includes of `chain_restore_service.h` with the
focused chain_restore headers, move the planner state/input/plan types into
`chain_restore_planner.h`, and delete the compatibility umbrella header.

## Completion

`app/services/include/services/chain_restore_service.h` is deleted. No source
file includes it, and the lib-layering baseline no longer carries the old
cross-layer include entries for it.

## Verification

- `make -j$(nproc) test_zcl test_parallel` - PASS
