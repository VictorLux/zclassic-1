# Agent 3 Task: Wave 13 — Split msgprocessor.c & Defensive Coding Enforcement

## Previous work (DONE)
Wave 12: Sync watchdog service — auto-detect and recover from sync stalls. All merged.

## Problem

`msgprocessor.c` is 4,604 lines with 139 functions, including a 779-line function (`handle_zcl23_sync`) and a 543-line function (`msg_send_messages`). It has zero tests. It handles ALL P2P message types in one file. This makes it fragile, hard to test, and hard to reason about.

Additionally, there are 156 bare `return -1` without logging across the codebase, violating DEFENSIVE_CODING.md.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `lib/net/src/msgprocessor.c` — the file to split
- `lib/net/include/net/msgprocessor.h` — current header
- `Makefile` — how source files are compiled (add new .c files here)

## Tasks

### 1. Split msgprocessor.c into focused files

Create these new files in `lib/net/src/`, moving the relevant functions:

**a. `msg_version.c`** — Version/verack handshake
- `process_version()` (~136 lines)
- `process_verack()`
- Version-related helpers

**b. `msg_headers.c`** — Header sync messages
- `process_headers()` (~368 lines)
- `process_getheaders()`
- `push_getheaders()`, `push_getheaders_from()`
- `exec_getheaders_action()`
- Header stall tracking globals

**c. `msg_blocks.c`** — Block handling
- `process_block_msg()` (~260 lines)
- `process_getdata()` (~102 lines)
- `process_getblocks()`
- Block-related helpers

**d. `msg_tx.c`** — Transaction relay
- `process_tx_msg()` (~114 lines)
- `process_inv()`
- `process_mempool()`
- Dandelion++ state and helpers

**e. `msg_compact.c`** — Compact blocks (BIP152)
- `process_cmpctblock()` (~118 lines)
- `process_blocktxn()`
- `process_getblocktxn()`

Keep in `msgprocessor.c`:
- `msg_process_messages()` — the dispatcher (~104 lines)
- `msg_send_messages()` — the send loop (stays here for now, it touches everything)
- The message dispatch table
- Shared state and init functions

### 2. Create shared header for split files

Create `lib/net/include/net/msg_internal.h` with:
- Forward declarations shared between the split files
- Access to `msg_processor` struct, `msg_get_height()`, etc.
- The split files include this instead of duplicating declarations

### 3. Update Makefile

Add the new .c files to the build. Search for `msgprocessor.c` in the Makefile and add the new files alongside it.

### 4. Migrate bare return -1 in MCP handlers

In `tools/mcp/controllers/*.c`, find all bare `return -1;` without `LOG_ERR` and replace with `LOG_ERR("mcp", "description of what failed")`. These are the MCP handlers that agents interact with — errors should be visible.

Count before starting (should be ~20-30 across the MCP controller files).

### 5. Verify

- `make -j$(nproc)` must succeed with zero new warnings
- `make test` must pass with zero new failures
- The split should be pure refactoring — no behavior changes

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Do NOT change any logic — this is purely a file reorganization + error logging migration
- Each split file gets the same copyright header as msgprocessor.c
- Commit with descriptive messages
- Do NOT touch files unrelated to this task
