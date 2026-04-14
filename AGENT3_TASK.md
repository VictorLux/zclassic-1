# Agent 3 Task: Wave 22 — LOG_FAIL Spam + Before-Save Hooks + Logging

## Status
- `make test` shows "1 failures" but real failure is hidden under ~1M LOG_FAIL lines from fast_sync PoW solve loop
- 3 models missing before_save hooks
- ~10 service error paths use bare `fprintf` instead of `LOG_ERR`

## Priority Order
1. **Task 1: Fix LOG_FAIL spam in fast_sync_verify_pow** — remove LOG_FAIL from normal nonce-fail path (DO FIRST)
2. **Task 2: Wire before_save hooks** — mempool_entry, tx_index, wallet_tx
3. **Task 3: fprintf→LOG_ERR migration** — 5 service files
4. **Task 4: Find and fix the real test failure** — after Task 1 cleans output

## See AGENT3.md for full task details

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make -j$(nproc) && make test` — must stay at 0 test regressions
- Commit with `wave 22 task N:` prefix
- Do NOT touch Agent2 files (see boundary list in AGENT3.md)
