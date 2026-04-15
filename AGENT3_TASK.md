# Agent 3 Task: Wave 27 — Soak Results + Tor + Log Rotation + Fast Sync Test

## Status
- Your wave 26 was excellent — bg_validation 2 workers, file protocol Phase 3, soak running
- Node at tip, healthy

## Priority Order
1. **Task 1: Soak test status** — `tail -50 soak_test.log`. Report: how long has it been running, any alerts, height progression, RSS trend, peer count stability. If it's not running, restart it.
2. **Task 2: Enable Tor on main instance** — edit `~/.config/systemd/user/zclassic23.service`, add `-tor` flag. `systemctl --user daemon-reload && systemctl --user restart zclassic23`. Verify .onion address appears in `zcl_status` after ~30s.
3. **Task 3: Log rotation** — `~/.zclassic-c23/node.log` grows forever. Either add a logrotate config file at `/home/rhett/.config/logrotate/zclassic23.conf` or implement built-in rotation in the node (rename + reopen when >100MB).
4. **Task 4: P2P fast sync end-to-end test** — The test instance at port 8035 can be used as a peer. Test: can a fresh zclassic23 node sync from the test instance via FlyClient + SHA3 UTXO snapshot? Use `-fastsync -addnode=127.0.0.1:8033`.

## See AGENT3.md for details
