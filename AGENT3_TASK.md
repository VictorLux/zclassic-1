# Agent 3 Task: Wire Watchdog + Sync Diagnostics

## Context

Agent 2 is fixing the sync stall root cause (broken pprev heights causing contextual_check_block_header to reject headers). Your job is to make sure this class of bug is observable and self-healing in the future.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `app/services/src/sync_watchdog_service.c` — your previous work
- `lib/net/src/msgprocessor.c` — the send loop where watchdog should be called
- `config/src/boot_services.c` — service startup
- `~/.zclassic-c23/node.log` — live evidence of the current stall

## Tasks

### 1. Wire sync_watchdog_check() into the message loop

Check if `sync_watchdog_check()` is actually being called. Look in `msgprocessor.c` `msg_send_messages()` around the ~30s periodic section. If NOT wired:

```c
/* In the periodic check section of msg_send_messages, after stall detection */
{
    struct connman *cm = get_connman();  /* or however connman is accessed */
    struct download_manager *dm = get_download_mgr();
    sync_watchdog_check(cm, dm, mp->main_state);
}
```

Also verify `sync_watchdog_init()` is called in `boot_services.c`.

### 2. Add sync diagnostic counters

Add counters to track the header sync pipeline. Put them in `msg_headers.c`:

```c
static _Atomic uint64_t g_headers_batches_received = 0;
static _Atomic uint64_t g_headers_total_accepted = 0;
static _Atomic uint64_t g_headers_total_rejected = 0;
static _Atomic uint64_t g_headers_newly_added = 0;
static _Atomic uint64_t g_headers_already_known = 0;
```

Update `process_headers()` to increment these. Expose them via a function `msg_headers_get_stats()` that the RPC/MCP layer can call.

### 3. Add getsyncdiag RPC

Add RPC method `getsyncdiag` that returns:
```json
{
  "watchdog": { "enabled": true, "checks_run": 42, "recoveries": 1, ... },
  "headers": {
    "batches_received": 150,
    "total_accepted": 23000,
    "total_rejected": 345,
    "newly_added": 22000,
    "already_known": 1000
  },
  "sync_state": "headers_download",
  "sync_state_duration_secs": 120,
  "chain_height": 2015124,
  "best_header_height": 2015200
}
```

Wire it into the RPC dispatch. Look at how existing RPC methods like `getsyncdetail` are registered.

### 4. Watchdog escalation for persistent header stalls

If the watchdog detects HEADER_STALL and peer rotation doesn't fix it after 2 cycles (600s total), escalate:
- Log the reject reason from the last rejected header
- If reject reason contains "equihash" or "solution-size", log: `"[watchdog] ESCALATION: height corruption detected, headers rejected with wrong-era validation rules"`
- This gives operators clear diagnostic info

### 5. Tests

Add tests for:
- Diagnostic counter increments
- Watchdog wiring verification (init called, check produces results)
- Put in `lib/test/src/test_sync_watchdog.c` (extend existing)

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- Commit with descriptive messages
- Do NOT touch `process_block.c` or the header acceptance logic — that's Agent 2's job
