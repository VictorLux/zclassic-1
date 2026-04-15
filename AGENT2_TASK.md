# Agent 2 Task: Wave 28b — PHGR13 + Swap Tests + Store

## Status
- Prometheus metrics, compact blocks, log rotation all merged
- Node redeployed with latest code. At tip (3,079,099).

## Priority Order
1. **Task 1: PHGR13 Sprout VK format** — last validation gap. Read `lib/sapling/src/sprout.c`, find the VK loading code. Compare expected format against zcash params files. The VK data may need byte-swapping or field reordering. For Sprout proofs at h<581876. Even a "this is what's wrong and how to fix it" report is valuable.
2. **Task 2: Atomic swap test** — write a test in `lib/test/src/` that creates an HTLC contract script, verifies it matches the 97-byte dcrdex format, and tests secret extraction from a redeem transaction. Use the existing swap code in `lib/script/src/htlc.c`.
3. **Task 3: Store checkout flow** — read `app/controllers/src/store_controller.c`. What's implemented vs stubbed? Is there a payment detection callback? Can a z-addr invoice be generated? Report what exists and what's missing.

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 28 task N:` prefix
