# Agent 3 Task: Wave 15 — Fix 12 Test Failures + Log Noise Reduction

## Current state
Node is syncing (slowly). 12 pre-existing test failures need fixing. Log is being spammed with checkpoint and bg_validation noise.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- Run `make test 2>&1 | grep -E 'FAIL|SOME TESTS'` to see the failures

## Tasks

### 1. Fix MCP tool count test failures (5 failures)

The test expects 76 tools but we now have more (new tools added). Find the test:
```bash
grep -rn 'EXPECTED_TOTAL\|expected_total\|76' lib/test/src/test_mcp*.c
```
Update the expected counts to match the current tool surface.

### 2. Fix wallet_sqlite_open test failures (7 failures)

Tests fail with `FAIL (wallet_sqlite_open(&ws, db))`. Find the test:
```bash
grep -rn 'wallet_sqlite_open' lib/test/src/test_wallet*.c
```
Investigate why the open fails — likely missing schema table or migration. Fix the test setup.

### 3. Suppress bg_validation log spam

`bg_validation_service.c:192` logs `read_block_undo: undo pos is 0 for file N` for every block without undo data. This floods the log. Fix:
- Only log this ONCE per file, or only at debug level
- Or skip the undo read entirely when undo pos is 0 (it means no undo data exists)

### 4. Suppress checkpoint log spam  

`checkpoints.c:71` logs `hash_at_height: no checkpoint at height N` for almost every height during bg_validation. Fix:
- Remove the LOG_FAIL from `checkpoints_hash_at_height()` when no checkpoint exists at that height — this is normal, not an error
- Only log when a checkpoint EXISTS and the hash DOESN'T MATCH (actual violation)

### 5. Run make test — verify 0 failures

After fixing tasks 1-4, run `make test` and verify all 12 failures are fixed. Report the final count.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` — target is 0 failures
- Commit with descriptive messages
- Do NOT touch sync/validation/activation code — that's Agent 2's domain
