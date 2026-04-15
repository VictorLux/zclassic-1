# Agent 2 Task: Wave 27 — Compact Blocks + Prometheus + PHGR13

## Status
- Node at tip (3,079,047), healthy, soak test running
- ZSLP send implemented, nSolution leak fixed, features confirmed enabled

## Priority Order
1. **Task 1: Compact blocks (BIP 152)** — `lib/net/src/msg_compact.c:251` TODO: "match against pending compact block reconstruction." Implement compact block relay — when a peer sends `cmpctblock`, reconstruct the full block from mempool txns + request missing via `getblocktxn`. Faster block relay, less bandwidth.
2. **Task 2: PHGR13 Sprout VK format** — last validation gap. `lib/sapling/src/sprout.c` has the code wired but VK parsing fails. Investigate the VK format — compare against zcash params files. This is for Sprout proofs at h<581876.
3. **Task 3: Prometheus /metrics endpoint** — if Agent3 hasn't done this yet, add a `/metrics` HTTP endpoint with Prometheus-format output: `zcl_block_height`, `zcl_peer_count`, `zcl_rss_mb`, `zcl_utxo_count`, `zcl_uptime_seconds`.

## See AGENT2.md for details
