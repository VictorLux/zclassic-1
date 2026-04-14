# Agent 3 Task: Wave 23b — Sapling Persistence + Validation Investigation

## Status
- Your wave 23 AT_TIP fix is merged. Block download pipeline working.
- Remaining checklist items need investigation.

## Priority Order
1. **Task 1: Sapling tree persistence** — stop 5-min rebuild after SIGKILL (add periodic flush)
2. **Task 2: Multi-threaded bg_validation crash** — investigate script interpreter thread safety
3. **Task 3: PHGR13 VK format** — investigate, document if non-blocking

## See AGENT3.md for full details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23b task N:` prefix
