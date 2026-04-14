# Agent 2 Task: Wave 23b — Sync Recovery + Re-enable Features

## Status
- Block download pipeline FIXED (Agent1). Node hitting UTXO mismatch — auto-reimport triggered.
- bg_hash_verify and address backfill SIGSEGVs FIXED in wave 22b but features still disabled.

## Priority Order
1. **Task 1: Improve self-heal logging** — add event_emitf for UTXO recovery, clean up needs_reimport flag
2. **Task 2: Re-enable bg_hash_verify** — find where disabled, turn on
3. **Task 3: Re-enable address backfill** — find where disabled, turn on
4. **Task 4: Update CHECKLIST.md** — mark 3 items as fixed

## See AGENT2.md for full details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23b task N:` prefix
