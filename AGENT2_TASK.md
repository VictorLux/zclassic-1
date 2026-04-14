# Agent 2 Task: Wave 19 — Unblock Sync Pipeline

## Status
- All tests pass (0 failures)
- Node stuck at 2,016,354, network at 3,077,951 (1M+ blocks behind)
- Boot-time height repair runs but header sync still too slow
- 7 peers connected

## Root Cause
Height repair works. But header sync crawls at 160 headers/round-trip with a minimal 2-hash locator. With 1M+ headers needed, that's ~17 hours. Plus no fast-path for already-known headers.

## Priority Order
1. **Task 1: getsyncdiag RPC** — visibility first
2. **Task 3: Header fast-path** — skip known header batches (biggest speedup)
3. **Task 2: Exponential locator** — better peer communication
4. **Task 4: Direct-to-blocks** — skip downloading blocks already on disk
5. **Task 5: Watchdog repair** — runtime height repair

## See AGENT2.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make -j$(nproc) && make test && make lint` — must stay at 0 failures
- Commit with `wave 19 task N:` prefix
- Do NOT touch Agent3 files (see boundary list in AGENT2.md)
