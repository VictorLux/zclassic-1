# Agent 3 Task: Wave 22b — Crash Recovery + Observability

## Status
- Your wave 22 tasks are DONE (LOG_FAIL fix, hooks, logging). Good work.
- Now: crash recovery investigation + observability improvements.

## Priority Order
1. **Task 1: Add memory RSS to health check** — node_health_service.c + ops_controller.c
2. **Task 2: Structured boot timing** — boot.c phase timing with [boot] log lines
3. **Task 3: Investigate bg_hash_verify SIGSEGV** — uses pread (safe) so crash is elsewhere
4. **Task 4: Investigate address backfill SIGSEGV** — SQLite mmap_size pressure?

## See AGENT3.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 22b task N:` prefix
- Do NOT touch Agent2 files (see boundary list in AGENT3.md)
