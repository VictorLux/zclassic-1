# Agent 3 Task: Wave 13 — Sync Pipeline Hardening

## CRITICAL CONTEXT: Node stuck at height 2,015,124

The node is stuck in `headers_download` state. Agent 2 is diagnosing the header rejection. Your job is to harden the sync pipeline so stalls self-heal and are observable.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `app/services/src/sync_watchdog_service.c` — your previous work
- `lib/net/src/msgprocessor.c` lines 4100-4290 — send loop, stall recovery
- `app/services/src/header_sync_service.c` — header sync logic
- `lib/validation/src/process_block.c` — `accept_block_header()`
- `config/src/boot_services.c` — where watchdog is wired

## Tasks

### 1. Wire the watchdog into the message loop

The watchdog service exists but may not be called from the message processing loop. Verify it's actually being invoked:
- Check `config/src/boot_services.c` — is `sync_watchdog_init()` called at startup?
- Check `lib/net/src/msgprocessor.c` `msg_send_messages()` — is `sync_watchdog_check()` called in the periodic section?
- If NOT wired, wire it: call `sync_watchdog_check(connman, download_mgr, main_state)` in the ~30s periodic check section of `msg_send_messages()`

### 2. Add getsyncwatchdog RPC

The watchdog has `sync_watchdog_get_status()` but no RPC to query it. Add:
- RPC method `getsyncwatchdog` in the RPC dispatch (look at how `getsyncdetail` is registered)
- Returns JSON with: enabled, checks_run, recoveries_triggered, last_recovery_type, current_state, current_state_duration_secs
- Also add it as an MCP tool `zcl_syncwatchdog` so we can query it from Claude Code

### 3. Add sync diagnostic counters

Add observable counters to the header sync pipeline. These help diagnose stalls:
- `getheaders_sent` — total getheaders messages sent
- `headers_received` — total headers message responses
- `headers_accepted` — total individual headers accepted
- `headers_rejected` — total individual headers rejected (with last reject reason)
- Expose via `getsyncdetail` RPC (add fields to the existing response)

### 4. Watchdog escalation: if peer rotation doesn't fix it, try harder

Current watchdog disconnects outbound peers on HEADER_STALL. If that doesn't work after 2 cycles (10 minutes), escalate:
- Clear the entire block index for heights > our chain tip (they may be corrupted/stale from snapshot)
- Reset `pindex_best_header` to chain tip
- Log: `"[watchdog] HEADER_STALL escalation: cleared stale block index entries above h=%d"`
- This forces a clean re-download of all headers above our confirmed chain

### 5. Split msgprocessor.c (secondary, after sync items)

Extract these into separate files:
- `lib/net/src/msg_headers.c` — `process_headers()`, `process_getheaders()`, `push_getheaders*()`, `exec_getheaders_action()`
- `lib/net/src/msg_blocks.c` — `process_block_msg()`, `process_getdata()`, `process_getblocks()`
- Create `lib/net/include/net/msg_internal.h` for shared declarations
- Update Makefile

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- Commit with descriptive messages
- Do NOT touch files unrelated to this task
