# Wave 30 Task 4: Add exponential backoff to addnode retry logic

## Problem

When outbound addnode connections fail (peer full, DoS ban, etc.), connman retries every ~60 seconds forever. This hammers peers and keeps MagicBean DoS bans alive indefinitely (see AGENT_TASK_3.md findings).

## What To Do

1. Find the addnode retry logic in `lib/net/src/connman.c` — look for `ThreadOpenAddedConnections` or similar
2. Add exponential backoff: after each failed attempt to the same peer, double the retry interval (60s → 120s → 240s → max 1800s)
3. Reset backoff to 60s on successful connection
4. Track per-peer backoff state (simple array or hash of addr → next_retry_time)

## Design Constraints

- Don't change the data structures more than necessary
- Keep it simple — a timestamp per addnode entry is enough
- On first failure: wait 120s. On second: 240s. Cap at 1800s (30 min).
- On success: reset to 0
- Log when backing off: `printf("Peer %s: backing off %ds after failed connect\n", ...)`

## Build & Test

```bash
make -j$(nproc)
make test    # 0 FAIL
```

## Context

- connman.c manages all P2P connections
- addnode peers are specified via `-addnode=IP:PORT` CLI flag
- The retry loop runs in a dedicated thread
- Current behavior: blind 60s retry regardless of failure history
