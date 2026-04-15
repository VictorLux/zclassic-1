# Agent 2 Task: Wave 24 — Boot Resilience + Re-enable Features

## Status
- UTXO wipe+replay running (your wave 23c fallback code triggered)
- Need to fix the primary path so future LDB imports don't hit this

## Priority Order
1. **Task 1: Fix coins_best_block resolution** — when hash not in index, use highest HAVE_DATA block
2. **Task 2: Re-enable bg_hash_verify** — SIGSEGV fixed wave 22b, find and re-enable
3. **Task 3: Re-enable address backfill** — SIGSEGV fixed wave 22b, find and re-enable

## See AGENT2.md for full details
