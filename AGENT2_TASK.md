# Agent 2 Task: Wave 22b — Thread Safety Fixes (CRITICAL)

## Status
- Agent3 completed wave 22 tasks (LOG_FAIL, hooks, logging). Tests pass.
- Two CRITICAL thread safety bugs remain — your top priority.

## Priority Order
1. **Task 1: Fix block_pruning_service.c lock bug** — lock released before unlink → SIGSEGV race
2. **Task 2: Protect boot_index.c scans** — P2P thread reads blk*.dat without lock while writer active
3. **Task 3: Audit all fread/fwrite paths** — document lock coverage across codebase

## See AGENT2.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 22 task N:` prefix
- Do NOT touch Agent3 files (see boundary list in AGENT2.md)
