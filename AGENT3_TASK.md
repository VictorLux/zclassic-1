# Agent 3 Task: Wave 14b — Fix Pre-existing Test Failures + Defensive Coding

## Previous work (DONE)
- Sync watchdog service + wiring into msg loop
- Diagnostic counters + getsyncdiag RPC
- Watchdog escalation for persistent header stalls
- msgprocessor.c split (4604→2938 lines)

## Current state
12 pre-existing test failures. Node is syncing. Time to harden.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `lib/test/src/test.c` — test registration
- Run `make test 2>&1 | grep FAIL` to see failures

## Tasks

### 1. Fix MCP tool count test failures (5 failures)

Run `make test` and find the test that checks tool counts. It expects 76 tools but we now have 79 (new tools added in recent waves). Update the expected counts:
- Total: find current count via `grep -c 'tool' ...` or reading the test
- Per-domain counts (ops, wallet) also need updating

These are in `lib/test/src/test_mcp_*.c` — find the exact file and fix.

### 2. Fix wallet_sqlite_open test failures (7 failures)

Tests fail with `FAIL (wallet_sqlite_open(&ws, db))`. This means the wallet SQLite layer can't open. Investigate:
- Is the schema wrong? Missing table? Wrong migration?
- Does the test create its own database or use a shared one?
- Fix the test setup or the schema

### 3. Migrate bare return -1 in critical paths

Focus on these files (highest impact):
- `lib/validation/src/process_block.c` — replace bare `return false` with `LOG_FAIL()`
- `lib/net/src/connman.c` — network errors should be logged
- `config/src/boot.c` — boot failures must be visible

Target: reduce bare returns by 30+.

### 4. Wire make lint

Ensure `make lint` runs these checks:
- No bare `malloc` outside vendor/test
- No bare `return -1` in MCP handlers
Add missing rules if needed.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before AND after — failure count must go DOWN
- Commit with descriptive messages
- Do NOT touch sync/validation code
