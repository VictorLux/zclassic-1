# Agent 3 Task: Wave 19 — Service Hardening + Operational Depth

## Status
- Node stuck at 2,016,354 — Agent1 + Agent2 are fixing sync blockers
- Agent3 continues hardening tasks (orthogonal to sync)

## Priority Order
1. **Task 1: sqlite3_step cleanup** — close lint gap (18 violations)
2. **Task 5: Controller error paths** — defensive coding compliance
3. **Task 2: Cookie rotation** — security improvement
4. **Task 3: Key scrubbing** — memory safety for wallet keys
5. **Task 4: MCP tool backfill** — close RPC/MCP gap

## See AGENT3.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make -j$(nproc) && make test && make lint` — must stay at 0 failures
- Commit with `wave 19 task N:` prefix
- Do NOT touch Agent2 files (see boundary list in AGENT3.md)
