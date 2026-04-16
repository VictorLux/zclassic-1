# Wave 30 Task 9: Add peer count and connection stats to zcl_status MCP tool

## Problem

`zcl_status` shows `"peers": 10` but doesn't break down inbound vs outbound, or show which peers are Zelcore vs ZCL23. With 10 peers now, this info is useful for monitoring.

## What To Do

1. Find the status MCP handler — it's in `tools/mcp/controllers/net_controller.c` or similar. Grep for `zcl_status`.

2. Add connection breakdown to the status response:
   ```json
   "connections": {
     "total": 10,
     "inbound": 5,
     "outbound": 5,
     "zcl23": 0,
     "magicbean": 10
   }
   ```

3. Count peers by checking `inbound` flag and `sub_ver` (contains "ZClassic-C23" for zcl23, "MagicBean" for legacy).

4. The peer data comes from `getpeerinfo` RPC internally. Check how `zcl_peers` gets its data and reuse that approach.

## Also: Add uptime to status

The `uptime_secs` field shows 0. Find where this is populated and fix it. The node tracks start time somewhere — grep for `uptime` or `start_time` in the codebase.

## Build & Test

```bash
make -j$(nproc)
make test    # 0 FAIL
```
