# Agent 3 Task: Wave 23c — Fallback Sync + Error Recovery

## Status
- Node stuck: UTXO at h=3M, chain at h=2M. Agent2 fixing primary path.
- Need fallback path if coins_best_block can't be resolved.

## Priority Order
1. **Task 1: UTXO wipe + replay fallback** — if coins_best_block not found, wipe and replay from genesis
2. **Task 2: Improve activate_best_chain error recovery** — better messages + disconnect-tip fallback
3. **Task 3: Update CHECKLIST.md** — mark fixed items, add new remaining

## See AGENT3.md for full details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23c task N:` prefix
