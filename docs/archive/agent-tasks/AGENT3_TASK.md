# Agent 3 Task: Wave 29 — Fast Sync Fix + Soak + End-to-End Tests

## Status
- Your Tor port-collision fix merged. Onion service is live.
- Soak test: 49 checks passed, RSS stable, at tip. Good.
- Fast sync finding: "invalid-solution rejections" during header sync after UTXO snapshot. This needs fixing.
- Node at tip (3,079,215), healthy, 3 peers, Tor working.

## Priority Order

### Task 1: Fix Fast Sync Equihash Rejection (MANDATORY, DO FIRST)

You reported that fast sync receives the UTXO manifest (1.34M UTXOs) but then header sync stalls with "invalid-solution" rejections. This is the critical bug blocking fast sync.

**Investigate:**
1. Read `lib/net/src/fast_sync.c` — how does post-snapshot header sync work?
2. Read `lib/validation/src/check_block.c:86-88` — `check_equihash_solution()` is called with `check_pow=true`
3. The issue is likely: headers received during fast sync don't include `nSolution` data (1344 bytes), or the solution bytes aren't being transmitted in the compact header format
4. Check `lib/net/src/msg_headers.c` — how are headers serialized/deserialized? Is `nSolution` included?
5. Check if FlyClient sampled headers carry full solutions or just hashes

**Likely fix:** Either:
- (a) Ensure `nSolution` is transmitted with headers during post-snapshot sync, OR
- (b) Skip equihash verification for headers below the assume-valid checkpoint (we already trust them via FlyClient PoW sampling), OR
- (c) A combination — verify FlyClient samples fully, skip the rest

**Test:** Run fast sync again after fix:
```bash
rm -rf /tmp/zcl-test-fastsync
mkdir /tmp/zcl-test-fastsync
timeout 180 ./zclassic23 -datadir=/tmp/zcl-test-fastsync -port=18999 -rpcport=18998 -fastsync -addnode=127.0.0.1:8033 2>&1 | tail -50
```

Report: did headers sync past the snapshot height? Did blocks connect?

### Task 2: Restart Soak Test

The soak_test.log is gone (node restarted). Restart it:
```bash
nohup tools/soak_test.sh >> soak_test.log 2>&1 &
```

Check back after 10 minutes, report height + RSS + any alerts.

### Task 3: End-to-End Name Registration Test

ZCL Names RPCs work but no names exist on chain. Write a test:
1. In `lib/test/src/`, add tests for the ZNAM OP_RETURN format
2. Test that `znam_build_register_opreturn("testname", t_addr, z_addr)` produces valid OP_RETURN data
3. Test that `znam_parse_opreturn()` can decode what `znam_build_register_opreturn()` produces
4. Roundtrip: build → parse → verify fields match

Look at `lib/znam/src/znam.c` for the functions.

### Task 4: Atomic Swap — End-to-End Simulation

Agent2 wrote HTLC tests (21 passing). Extend with a full simulation:
1. In `lib/test/src/test_htlc.c`, add a test that simulates both sides of a swap
2. Initiator creates HTLC with secret → Participant creates counter-HTLC with same hash → Initiator redeems participant's HTLC (revealing secret) → Participant extracts secret from initiator's redeem tx → Participant redeems initiator's HTLC
3. Verify: both sides end up with the correct funds (script validation passes)

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 29 task N:` prefix
