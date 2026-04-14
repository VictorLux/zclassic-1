# Agent 2 Task: Wave 22 — Thread Safety & Allocator Hardening

## Status
- Node at tip, synced, 4GB RAM
- 3 thread safety bugs found by audit — root causes of known SIGSEGVs
- 5 files using raw `malloc`/`realloc` bypassing `zcl_*` safe allocator

## Priority Order
1. **Task 1: Fix block_pruning_service.c lock bug** — lock released before unlink → SIGSEGV
2. **Task 2: Protect boot_index.c scans** — P2P thread reads blk*.dat without lock while writer active
3. **Task 3: Replace raw allocators** — connect_block.c, update_coins.c, process_block.c, msg_headers.c
4. **Task 4: Audit all fread/fwrite paths** — document lock coverage

## See AGENT2.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make -j$(nproc) && make test` — must stay at 0 test regressions
- Commit with `wave 22 task N:` prefix
- Do NOT touch Agent3 files (see boundary list in AGENT2.md)
