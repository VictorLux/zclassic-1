# Agent 2 Task: Wave 23 — Fix Block Download Stall (CRITICAL)

## Status
- Node STUCK at height 2,016,355 — headers arrive but blocks never requested
- Root cause: two bugs in header_sync_service.c prevent block queuing

## Priority Order
1. **Bug 1: collect_needed_blocks walk dies at pprev==NULL** — header_sync_service.c:515, count=0 → no getdata
2. **Bug 2: should_begin_blocks_download requires SYNC_HEADERS_DOWNLOAD** — header_sync_service.c:435, blocks not queued in SYNC_BLOCKS_DOWNLOAD state

## See AGENT2.md for full details with line numbers and exact fixes

## Rules
- Follow `DEFENSIVE_CODING.md`
- Run `make -j$(nproc) && make test` — 0 failures required
- Commit with `wave 23 task N:` prefix
- Do NOT touch chain_activation_controller.c (Agent3) or process_block.c (Agent1)
