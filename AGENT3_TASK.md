# Agent 3 Task: Wave 26b — Monitor Soak + Prometheus + Tor

## Status
- Your wave 26 delivery was excellent — all 4 tasks done. bg_validation now runs 2 workers.
- Soak test running. Node at tip.

## Priority Order
1. **Task 1: Check soak test** — `tail -20 soak_test.log`. Is it running? Any alerts? Report status.
2. **Task 2: Prometheus /metrics endpoint** — Add a `/metrics` HTTP endpoint that outputs Prometheus-format metrics. Key metrics: `zcl_block_height`, `zcl_peer_count`, `zcl_rss_mb`, `zcl_utxo_count`, `zcl_sync_state`, `zcl_uptime_seconds`. Wire it into the existing HTTPS server or REST API.
3. **Task 3: Enable Tor on main instance** — Add `-tor` to the systemd service file at `~/.config/systemd/user/zclassic23.service`. Verify the node gets a .onion address after restart.
4. **Task 4: Log rotation** — stdout goes to `~/.zclassic-c23/node.log` which grows forever. Add either logrotate config or built-in rotation (rename + reopen when >100MB).

## See AGENT3.md for details
