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

## Results (2026-04-16)

### Tests
- **2692 OK, 0 FAIL** — all tests pass

### Fixes Applied

1. **localaddresses fix** — `getnetworkinfo` now populates `localaddresses` from `-externalip` flag.
   - Added `msg_version_get_external_ip()` getter in `lib/net/src/msg_version.c`
   - Updated `app/controllers/src/network_controller.c` to include external IP in response
   - Result: `localaddresses: [{"address": "205.209.104.118", "port": 8033, "score": 1}]`

2. **Real Tor linkage** — discovered this repo's `vendor/lib/libtor_stub.a` (2KB) was being linked instead of the real `vendor/tor/libtor.a` (29MB). Copied real Tor libraries from `~/zclassic23/vendor/tor/`. Makefile auto-detects via `$(wildcard vendor/tor/libtor.a ...)`.

### Verification

| Check | Status | Details |
|-------|--------|---------|
| Tests | PASS | 2692 OK, 0 FAIL |
| External IP | PASS | `localaddresses: [{address: "205.209.104.118", port: 8033}]` |
| Tor bootstrap | PASS | Bootstrapped 100%, `tor=yes` |
| Onion address | PASS | `oaejwtr7wd6ah6csxz4vy4iro6l5cxc2flbmxkhgybgafuu25fg7nkid.onion` |
| Backoff | PASS | Exponential backoff working: 120s → 240s for failed addnode peers |
| Inbound peers | PASS | Observed inbound from `74.50.74.102` (ZClassic-C23) |
| Block height | PASS | At tip, height 3079483+ |
| Peer count | PARTIAL | 1 stable (localhost C++ node) + transient inbound; addnode seeds mostly offline |

### Notes
- The `vendor/tor/` directory with `libtor.a` is not tracked in git (29MB). Must be copied from the source build or `~/zclassic23/vendor/tor/` for Tor to work.
- Without real `libtor.a`, Tor exits immediately with code -1 and no log output.
