# Agent 3 Task: Wave 14 — Test Coverage & Defensive Coding

## Previous work (DONE)
- Wave 12: Sync watchdog service (auto-recovery, circuit breaker)
- Wave 13: msgprocessor.c split (4604→2938 lines), watchdog wiring, diagnostic counters, getsyncdiag RPC, watchdog escalation

## Current state
Sync stall is FIXED. Node is actively syncing. Time to harden the codebase.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `lib/test/src/test.c` — test registration
- `Makefile` — build rules, lint targets

## Tasks

### 1. Fix the 12 pre-existing test failures

Run `make test` and find the 12 failures. They fall into two categories:

**a. MCP tool count mismatch (5 failures)**: Tests expect 76 tools but we now have 79 (new tools were added). Find the test in `lib/test/src/` that checks tool counts and update the expected values.

**b. wallet_sqlite_open failures (7 failures)**: These crash with `FAIL (wallet_sqlite_open(&ws, db))`. Investigate why — likely the test database setup is missing a schema migration or table creation step. Fix the test setup.

### 2. Migrate bare `return -1` in critical paths

There are 156 bare `return -1` without logging. Focus on the most critical files first:

- `lib/validation/src/process_block.c` — replace bare `return -1` and `return false` with `LOG_FAIL()`
- `lib/net/src/connman.c` — network connection errors should be logged
- `config/src/boot.c` — boot failures must be visible

Count them before and after. Target: reduce from 156 to under 100.

### 3. Add tests for the split message handler files

The new files from the msgprocessor split have no tests:
- `lib/net/src/msg_version.c`
- `lib/net/src/msg_headers.c`
- `lib/net/src/msg_blocks.c`
- `lib/net/src/msg_tx.c`
- `lib/net/src/msg_compact.c`

Add `lib/test/src/test_msg_handlers.c` with tests for any pure/testable functions in these files. Even basic smoke tests that call the public functions with NULL args to verify they don't crash.

### 4. Wire `make lint` to enforce DEFENSIVE_CODING.md

Check the Makefile for existing lint targets. Ensure `make lint` checks:
- No bare `malloc` outside vendor/test (use `zcl_malloc`)
- No bare `return -1` in `tools/mcp/` (use `LOG_ERR`)
- Report violations with file:line

If rules exist but aren't wired, wire them. If missing, add them.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before AND after — verify failure count goes DOWN
- Commit with descriptive messages
- Do NOT touch sync/validation code — that's Agent 2's domain
