# Agent 3 Task: Wave 28 — Soak Results + Fast Sync + Name Registry

## Status
- Soak test should have been running. Tor just enabled on main instance.
- Node deployed fresh — check everything.

## Priority Order
1. **Task 1: Soak test results** — check `soak_test.log` or restart if not running. Report: uptime, height progression, RSS trend, any crashes or stalls.
2. **Task 2: Verify Tor** — node was just restarted with `-tor`. Check `zcl_status` for onion_address. If Tor bootstrapped, the .onion should be visible. Report the address.
3. **Task 3: P2P fast sync test** — test fresh sync from the main instance. Create a temp datadir, run `./zclassic23 -datadir=/tmp/zcl-fastsync-test -fastsync -addnode=127.0.0.1:8033 -noconnect` (only connect to our own node). Does it get UTXO snapshot? How long to sync?
4. **Task 4: ZCL Names end-to-end** — test name registration flow: `zcl_name_register(name="test-agent3")` → check `zcl_name_resolve(name="test-agent3")` → verify it resolves. If registration requires on-chain tx, build and broadcast it.

## See AGENT3.md for details
