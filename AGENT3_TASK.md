# Agent 3 Task: Wave 24 — Sapling Persistence + Validation

## Status
- LDB import works but post-import wipes destroy the data (Agent2 fixing)
- Work on reliability items while Agent2 fixes the import

## Priority Order
1. **Task 1: Sapling tree persistence** — add periodic checkpoint to prevent 5-min rebuild after SIGKILL
2. **Task 2: Multi-threaded bg_validation** — investigate script interpreter thread safety
3. **Task 3: Boot timing completeness** — verify all phases are timed

## See AGENT3.md for full details
