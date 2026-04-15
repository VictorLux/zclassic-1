# Agent 2 Task: Wave 26b — ZSLP Send + Leak Fix + Compact Blocks

## Status
- bg_hash_verify and address backfill are ALREADY ENABLED AND RUNNING. They start automatically in boot_services.c:1382-1390 and boot_services.c:315. Not gated by any flag. This was never broken — validation status shows `state: complete`. STOP looking for this.
- Node at tip (3,079,032), healthy, soak test running

## Priority Order
1. **Task 1: ZSLP on-chain SEND** — implement the TODO at `zslp_controller.c:248`. Build OP_RETURN with lokad_id + tx_type + token_id + amounts. Look at `name_controller.c` for the OP_RETURN transaction pattern.
2. **Task 2: nSolution memory leak** — `process_block.c:317` allocates 1344B/block, never freed. Free nSolution after connect_block succeeds in connect_tip.
3. **Task 3: Compact blocks (BIP 152)** — `msg_compact.c:251` TODO: "match against pending compact block reconstruction." Implement compact block relay for faster block propagation.

## See AGENT2.md for details
