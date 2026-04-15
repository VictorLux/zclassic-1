# Agent 3 Task: Wave 28b — Soak + Tor Verify + Fast Sync + Names

## Status
- Your Prometheus + log rotation + Tor flag merged. Node redeployed.
- Soak test should be running. Tor should be bootstrapping.

## Priority Order
1. **Task 1: Check soak test** — `tail -50 soak_test.log` or `cat soak_test.log | tail -50`. Is it running? Report height progression, RSS trend, any alerts. If dead, restart with `nohup tools/soak_test.sh >> soak_test.log 2>&1 &`.
2. **Task 2: Verify Tor onion address** — node just restarted with `-tor`. Check `zcl_status` or `zcl_onion_status` for the .onion address. If `tor_enabled: false` still, check if `-tor` is actually being parsed — grep for the flag parsing in `main.c` or `config/`.
3. **Task 3: Fast sync test** — create `/tmp/zcl-test-fastsync/`, run `./zclassic23 -datadir=/tmp/zcl-test-fastsync -port=18999 -rpcport=18998 -fastsync -addnode=127.0.0.1:8033` for 2 minutes, check if it receives UTXO snapshot. Kill after test. Report what happened.
4. **Task 4: ZCL Names test** — call `zcl_name_list` to see registered names. Call `zcl_name_resolve(name="zclassic")` to test resolution. Report what works.

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 28 task N:` prefix
