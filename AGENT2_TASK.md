# Agent 2 Task: Wave 24b — Re-enable Features + Polish

## Status
- Your triple-wipe fix is deployed and being tested RIGHT NOW
- If it works, the node will be at tip (~3,078,000) for the first time since April 7

## Priority Order (after confirming import works)
1. **Task 1: Re-enable bg_hash_verify** — SIGSEGV fixed wave 22b, find where disabled, turn on
2. **Task 2: Re-enable address backfill** — SIGSEGV fixed wave 22b, find where disabled, turn on  
3. **Task 3: Update CHECKLIST.md** — mark all fixed items, update remaining list

## See AGENT2.md for details

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 24b task N:` prefix
