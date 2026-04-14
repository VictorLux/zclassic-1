# Agent 3 Task: Wave 23 — Fix False AT_TIP + Re-enable Fixed Features

## Status
- Node STUCK at 2,016,355 — Agent2 fixing download pipeline, you fix activation
- bg_hash_verify and address backfill SIGSEGVs were FIXED in wave 22b but features still disabled

## Priority Order
1. **Task 1: Fix false AT_TIP** — chain_activation_controller.c:297, check peer height before declaring at_tip
2. **Task 2: Update CHECKLIST.md** — mark bg_hash_verify + address backfill as fixed
3. **Task 3: Re-enable bg_hash_verify** — find where disabled, turn it back on
4. **Task 4: Re-enable address backfill** — find where disabled, turn it back on

## See AGENT3.md for full details

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23 task N:` prefix
- Do NOT touch header_sync_service.c or download.c (Agent2)
