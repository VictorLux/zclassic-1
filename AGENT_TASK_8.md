# Wave 30 Task 8: Deduplicate peer connections

## Problem

We have two connections to the same peer 37.187.76.79 — one inbound (port 36536) and one outbound (port 8033). This wastes a connection slot and bandwidth. The outbound connection logic in connman should skip peers we already have an inbound connection from.

## What To Do

1. In `lib/net/src/connman.c`, the outbound diversity section already has an `already_connected` check that compares `addr` and `port`. But the inbound connection comes from an ephemeral port (36536), not 8033, so the check doesn't catch it.

2. Fix: when checking `already_connected`, compare only the IP address (not port) for inbound peers. If we have an inbound connection from the same IP, skip the outbound attempt.

3. Also check the addnode reconnect section — same issue there. If a peer from our addnode list already connected to us inbound, don't open a second outbound connection.

4. Add a log line when skipping: `printf("Skipping %s — already connected inbound\n", ...)`

## Also: Fix duplicate addnode connection

37.187.76.79 appears in both the addnode list AND got selected by addrman for outbound diversity. The addnode dedup in the outbound section should catch this, but verify it's working. The addnode check compares full addr+port, so it should match 37.187.76.79:8033.

The real issue is: the addnode reconnect fires (makes outbound to :8033), then later an inbound arrives from the same IP. We now have both. The fix is to check for existing connections from the same IP (ignoring port) before connecting.

## Build & Test

```bash
make -j$(nproc)
make test    # 0 FAIL
```
