# Wave 30 Task 7: Redeploy with backoff fix + full verification

## Problem

The addnode exponential backoff (task 4, commit 6c7629d76) isn't deployed yet. Need to rebuild, test, deploy, and do a full end-to-end verification of all wave 30 work.

## Steps

1. **Build and test**:
   ```bash
   make -j$(nproc) && make test 2>&1 | grep -c FAIL
   ```
   Must be 0 FAIL.

2. **Deploy**:
   ```bash
   make deploy
   ```

3. **Wait for boot** (45s for P2P + Tor):
   ```bash
   sleep 45
   ```

4. **Verify all wave 30 fixes are live**:

   a. **Backoff working** — check logs for "backing off" messages:
   ```bash
   journalctl --user -u zclassic23 --since "1 min ago" --no-pager | grep -i backoff
   ```

   b. **External IP advertised** — check localaddresses:
   ```bash
   curl -s http://127.0.0.1:18232/ -u "$(cat ~/.zclassic-c23/.cookie)" \
     --data-binary '{"method":"getnetworkinfo"}' -H 'content-type:text/plain;' \
     | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; print('localaddresses:', r.get('localaddresses',[])); print('connections:', r['connections'])"
   ```

   c. **Tor + onion**:
   ```bash
   curl -s http://127.0.0.1:18232/ -u "$(cat ~/.zclassic-c23/.cookie)" \
     --data-binary '{"method":"getnetworkinfo"}' -H 'content-type:text/plain;' \
     | python3 -c "import json,sys; r=json.load(sys.stdin)['result']; [print(n['name'],n['reachable']) for n in r.get('networks',[])]"
   ```

   d. **Peers** — want at least 2 non-localhost peers:
   ```bash
   curl -s http://127.0.0.1:18232/ -u "$(cat ~/.zclassic-c23/.cookie)" \
     --data-binary '{"method":"getpeerinfo"}' -H 'content-type:text/plain;' \
     | python3 -c "
   import json,sys
   r=json.load(sys.stdin)['result']
   for p in r: print(f\"{p['addr']:30s} {'IN' if p['inbound'] else 'OUT':3s} {p['subver']}\")
   print(f'Total: {len(r)} peers')
   "
   ```

5. **Update this file with results**, commit, push.

## Success Criteria

- 0 test failures
- Tor enabled + onion address present
- External IP in localaddresses
- At least 2 non-localhost peers
- Backoff messages visible in logs for failed addnode attempts
- Node at tip and healthy
