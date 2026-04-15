# Agent 2 Task: Wave 28 — PHGR13 + Atomic Swaps + Store Checkout

## Status
- Compact blocks (BIP 152) and Prometheus metrics DONE
- Node deployed with -tor and Prometheus enabled
- At tip (3,079,098), healthy, 5 peers

## Priority Order
1. **Task 1: PHGR13 Sprout VK format** — last validation gap. `lib/sapling/src/sprout.c` has verification code wired but VK parsing fails. Investigate: what format does the code expect vs what zcash params provide? For Sprout proofs at h<581876.
2. **Task 2: Atomic swap end-to-end test** — ZSWP is wired for ZCL/BTC/LTC/DOGE. Use `zcl_swap_initiate` and `zcl_swap_participate` MCP tools to create test HTLC contracts. Verify the script matches dcrdex format (97-byte P2SH). Write a test if one doesn't exist.
3. **Task 3: Store checkout with shielded payment** — the e-commerce store exists but needs end-to-end checkout: browse products → add to cart → generate z-addr invoice → detect payment → mark paid. Check `app/controllers/src/store_controller.c` for what's implemented vs stubbed.

## See AGENT2.md for context
