# Wave 30 Task 5: Redeploy with -tor and verify Zelcore peer visibility

## Problem

The running node doesn't have Tor enabled (health shows `tor_enabled: false`). The deploy service file has `-tor` but the node wasn't redeployed after the latest changes. We need to redeploy and verify we're visible on the Zelcore explorer network page.

## Steps

1. **Verify tests pass first**:
   ```bash
   make -j$(nproc) && make test
   ```
   Ensure 0 FAIL lines.

2. **Deploy**:
   ```bash
   make deploy
   ```
   This builds, setcap, and restarts the linger service.

3. **Wait for boot** (~30s for P2P, ~10s for Tor bootstrap):
   ```bash
   sleep 45
   ```

4. **Check health via MCP or curl**:
   ```bash
   curl -s http://127.0.0.1:18232/ -u "$(cat ~/.zclassic-c23/.cookie)" \
     --data-binary '{"method":"getnetworkinfo"}' -H 'content-type:text/plain;' | python3 -m json.tool
   ```
   Verify:
   - `localaddresses` contains our IP 205.209.104.118:8033
   - Tor is bootstrapped
   - Onion address is present

5. **Check peer count** — should get inbound Zelcore peers within a few minutes:
   ```bash
   curl -s http://127.0.0.1:18232/ -u "$(cat ~/.zclassic-c23/.cookie)" \
     --data-binary '{"method":"getpeerinfo"}' -H 'content-type:text/plain;' | python3 -c "
   import json,sys
   r=json.load(sys.stdin)['result']
   for p in r: print(f\"{p['addr']:30s} {'IN' if p['inbound'] else 'OUT':3s} {p['subver']:30s} h={p['startingheight']}\")
   print(f'Total: {len(r)} peers')
   "
   ```

6. **Document results** — update this file with peer count and whether Tor/onion is working

## Context

- Service file: `deploy/zclassic23.service`
- ALWAYS use `make deploy` — never manual setcap
- After deploy, Sapling tree rebuild takes ~7 minutes
- Node should be at tip and accepting connections within 1 minute
- Previous session had 2 inbound Zelcore peers — should get at least that many again
