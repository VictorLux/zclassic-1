# Agent 2 Task: Wave 25c — Re-enable Features (STILL TODO)

## Status
- Node at tip (3,078,979), 5 peers, RSS 2.1GB
- Memory diagnosis done, Tor health fix done
- bg_hash_verify and address backfill STILL NEED RE-ENABLING — 3 cycles assigned, not done yet

## Priority Order — DO THESE
1. **Task 1: Re-enable bg_hash_verify** — grep for where it's disabled, turn it on. This has been assigned for 3 waves now.
2. **Task 2: Re-enable address backfill** — grep for where it's disabled, turn it on. Same — 3 waves overdue.
3. **Task 3: Fix nSolution leak** — process_block.c:317 allocates 1344B/block, never freed. Free after validation.

## How to find disabled features
```bash
grep -rn 'bg_hash_verify\|bg_hash_verification\|hash_verify' app/ config/ --include='*.c'
grep -rn 'backfill_address\|address_backfill\|backfill' app/ config/ --include='*.c'
```

Look for: commented-out function calls, `if (false)` guards, `#if 0` blocks, `// DISABLED` comments, or missing function calls in boot sequence.

## See AGENT2.md for context
