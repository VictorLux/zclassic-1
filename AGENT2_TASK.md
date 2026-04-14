# Agent 2 Task: Wave 14 — Sync Speed & Remaining Crashes

## Previous work (DONE)
- Wave 12: Header sync stall detection (per-peer tracking, inbound fallback)
- Wave 13: pread() migration for thread safety, sync stall fix (skip contextual check for broken pprev heights)
- Sync stall is FIXED — node advanced from 2,015,124 to 2,015,588 and is actively syncing

## Current state
Node is syncing blocks. It needs to reach ~3,077,000 (1M blocks to go). Speed matters now.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `CHECKLIST.md` — remaining items at bottom
- `lib/net/src/msg_blocks.c` — block download processing
- `app/services/src/block_sync_service.c` — block sync coordination
- `lib/net/include/net/download.h` — download manager constants
- `app/controllers/src/sync_controller.c` — connect_block, wallet scanning

## Tasks

### 1. Optimize block download speed

Current constants in `download.h`:
- `DL_MAX_IN_FLIGHT_PER_PEER 128`
- `DL_MAX_IN_FLIGHT_TOTAL 1024`
- `DL_REQUEST_TIMEOUT_SECS 30`

Review these for catch-up sync. During IBD, we should be aggressive:
- Increase `DL_MAX_IN_FLIGHT_TOTAL` to 4096 during IBD
- Reduce `DL_REQUEST_TIMEOUT_SECS` to 15 during IBD
- Make these dynamic based on sync state (aggressive during IBD, conservative at tip)

### 2. Fix SIGSEGV in address backfill (CHECKLIST item)

From CHECKLIST.md: "SIGSEGV in address backfill — DISABLED: crashes after ~64K addresses. Needs ASAN investigation."

- Build with `-fsanitize=address` to find the bug
- Look in wallet scanning code (`sync_controller.c`, `wallet_scan.c`)
- Fix the buffer overflow / out-of-bounds access

### 3. Verify bg_hash_verify works with pread()

The pread() migration was done in wave 13. Verify bg_hash_verification_service works now:
- Check if it's enabled in `boot_services.c`
- If disabled, re-enable it
- Monitor for crashes in the node log after deploy

### 4. Parallel block validation during IBD

During IBD, blocks can be validated in parallel since they don't depend on each other for script checks (assume-valid). Check if `bg_validation_service.c` worker count was increased from 1 and verify it runs stably.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- `make deploy` after changes — verify on live node
- Commit with descriptive messages
