# Wave 30 Task 6: Increase outbound connection diversity

## Problem

Our node has only 1 outbound connection (local C++ at 127.0.0.1:8034). The other 2 peers are inbound Zelcore nodes. We need outbound connections to non-addnode peers for network resilience — if the 2 inbound peers disconnect, we're isolated except for localhost.

## What To Do

1. Find the outbound connection logic in `lib/net/src/connman.c` — look for `thread_open_connections`
2. The connman should try to maintain a target of **8 outbound connections** (Bitcoin default)
3. When below target, pick addresses from addrman (`addrman_select`) and attempt outbound connects
4. Skip addnode addresses (already handled separately) and already-connected peers
5. Rate-limit: at most 1 new outbound attempt per 10 seconds to avoid flooding

## Current State

```
Peer 0: 127.0.0.1:8034      OUT  /MagicBean:2.1.1-10/  (local C++)
Peer 1: 66.70.182.1:57956   IN   /MagicBean:2.1.1-10/  (Zelcore)
Peer 2: 51.178.179.75:36490 IN   /MagicBean:2.1.1-10/  (Zelcore)
```

## Design

In `thread_open_connections`, after the addnode reconnect block:

```c
/* Maintain outbound target from addrman */
int outbound_count = 0;
zcl_mutex_lock(&cm->manager.cs_nodes);
for (size_t i = 0; i < cm->manager.num_nodes; i++) {
    if (!cm->manager.nodes[i]->inbound && !cm->manager.nodes[i]->disconnect)
        outbound_count++;
}
zcl_mutex_unlock(&cm->manager.cs_nodes);

if (outbound_count < 8) {
    /* Select from addrman and connect */
    ...
}
```

Check `addrman_select()` in `lib/net/src/addrman.c` for how to pick addresses. Use `connect_node()` to initiate the connection.

## Build & Test

```bash
make -j$(nproc)
make test    # 0 FAIL
```
