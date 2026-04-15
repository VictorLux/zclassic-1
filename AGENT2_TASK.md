# Agent 2 Task: Wave 25c — Results

## Status
- Task 1 (bg_hash_verify): ALREADY ENABLED — gated only by `-nobgvalidation` CLI flag, which the service file does not pass. Code: `config/src/boot_services.c:1382-1390`.
- Task 2 (address_backfill): ALREADY ENABLED — `want_address_backfill` is set true when `addresses_backfilled == 0` in node_db (`config/src/boot.c:2232`). Only deferred during fresh bootstrap receiver mode (correct behavior). Code: `config/src/boot_services.c:1345-1356`.
- Task 3 (nSolution leak): FIXED — `process_block.c:1259` now frees `pindex_new->nSolution` after writing block index to disk. Saves 1344B per block (~4GB for 3M blocks). Serving code in `msg_headers.c` and `msg_blocks.c` already has disk-read fallbacks when nSolution is NULL.

## Details

### Tasks 1 & 2: bg_hash_verify and address_backfill
Exhaustive grep found NO disabled code — no `#if 0`, no `if (false)`, no commented-out calls, no `// DISABLED` markers. Both features are wired into the boot sequence and start correctly when conditions are met. The previous waves may have been looking for an explicit disable that doesn't exist.

### Task 3: nSolution memory fix
- `process_block.c:317` allocates 1344B for Equihash solution per block received via P2P
- `boot_index.c:455` already sets nSolution=NULL for blocks loaded from disk (comment: "saves 1.3KB per entry, 4GB total for 3M entries")
- Fix: free nSolution immediately after persisting to disk at line 1259, matching boot_index.c's pattern
- Both `msg_headers.c:134-146` and `msg_blocks.c:401-411` have fallback paths that read solutions from disk when pointer is NULL
