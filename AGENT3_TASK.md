# Agent 3 Task: Wave 25b — Resilience Testing + Soak Script

## Status
- NODE AT TIP, healthy. Memory fix applied. Sapling checkpoint merged.
- Need to test resilience and write the soak test.

## Priority Order
1. **Task 1: SIGKILL recovery test** — kill -9 the node, restart, measure recovery time and Sapling rebuild
2. **Task 2: Multi-threaded bg_validation fix** — apply fix from your wave 24 investigation
3. **Task 3: Write soak test script** — `tools/soak_test.sh`, monitors node 72 hours
4. **Task 4: Reorg safety test** — test disconnect_tip with undo data

## See AGENT3.md for details
