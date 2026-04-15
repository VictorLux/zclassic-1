# Agent 3 Task: Wave 24b — Continue Reliability Work

## Status
- Your Sapling checkpoint + bg_validation investigation + boot timing are merged
- Agent2's triple-wipe fix being tested — node should be at tip soon

## Priority Order
1. **Task 1: Apply bg_validation fix** — your investigation found the issue (shared secp256k1 context or script interpreter state). Implement the fix if clear.
2. **Task 2: Test Sapling tree checkpoint** — verify the periodic checkpoint survives SIGKILL (kill -9 the node, restart, measure rebuild time)
3. **Task 3: Add integration test for LDB import** — test that reimport doesn't wipe data (test the bug Agent2 fixed)

## See AGENT3.md for details

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 24b task N:` prefix
