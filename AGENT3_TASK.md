# Agent 3 Task: Wave 26 — Stress Test + File Protocol

## Status
- Node at tip, healthy. Soak script ready.

## Priority Order
1. **Task 1: Start soak test** — `nohup tools/soak_test.sh &`
2. **Task 2: Re-test bg_validation with 2 workers** — change count, deploy, monitor
3. **Task 3: File service retry** — file_service.c:1228 TODO
4. **Task 4: P2P file protocol Phase 3** — msgprocessor.c:1159,1203,1228 TODOs

## See AGENT3.md for details
