# Agent 2 Task: Wave 25b — Features + nSolution Leak

## Status
- NODE AT TIP, healthy. MALLOC_ARENA_MAX=2 applied to service file — expecting ~800MB RSS.
- Your memory diagnosis was spot-on. Tor health check fix merged.
- bg_hash_verify and address backfill still need re-enabling (tasks 1-2 from wave 25)

## Priority Order
1. **Task 1: Re-enable bg_hash_verify** — STILL TODO from wave 25. Find where disabled, turn on.
2. **Task 2: Re-enable address backfill** — STILL TODO from wave 25. Find where disabled, turn on.
3. **Task 3: Fix nSolution leak** — your diagnosis found it: `process_block.c:317` allocates 1344 bytes per block but never frees. Add cleanup for blocks that have been validated.
4. **Task 4: Verify RSS after MALLOC_ARENA_MAX=2** — once node boots, check RSS via `zcl_health`. Report the before/after.

## See AGENT2.md for context
