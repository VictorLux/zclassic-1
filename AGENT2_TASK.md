# Agent 2 Task: Wave 20 — Fix Sync Stall Root Cause

## Status
- Node stuck at 2,016,354, network at 3,078,005 (1M+ blocks behind)
- Two root causes identified:
  1. **False SYNC_AT_TIP** (Agent2's analysis) — headers_caught_up compares to local blocks, not peer height
  2. **Broken pprev chains** (Agent1's analysis) — 441K hash mismatches from LDB import prevent block downloads
- Agent2 handles the SYNC_AT_TIP fix, Agent1 handles the pprev repair

## Priority Order
1. **Task 1: Fix false SYNC_AT_TIP** — stop declaring at-tip when 1M behind peers
2. **Task 2: Watchdog detects stale SYNC_AT_TIP** — safety net
3. **Task 3: Force aggressive headers when behind** — faster header sync
4. **Task 4: Reset stale counts on state transition** — prevent backoff after recovery

## See AGENT2.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make -j$(nproc) && make test && make lint` — must stay at 0 failures
- Commit with `wave 20 task N:` prefix
- Do NOT touch Agent1/Agent3 files (see boundary list in AGENT2.md)
