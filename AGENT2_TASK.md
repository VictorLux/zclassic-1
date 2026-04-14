# Agent 2 Task: Wave 23c — Fix UTXO/Chain Tip Mismatch (CRITICAL)

## Status
- Node STUCK at 2,016,355. Blocks download but can't connect — `bad-txns-inputs-missingorspent`
- Root cause: UTXO set imported from zclassicd at h=3M, but chain tip is h=2M
- Block 2016356's inputs were spent in zclassicd's chain — they don't exist in the 3M UTXO set

## Priority Order
1. **Task 1: Set chain tip to match UTXO height** — read coins_best_block from LDB, find in block_index, set tip
2. **Task 2: Add diagnostic logging** — log UTXO height vs chain tip after import

## See AGENT2.md for full details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23c task N:` prefix
