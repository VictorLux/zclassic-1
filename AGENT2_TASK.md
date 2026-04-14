# Agent 2 Task: Wave 14b — Sync Speed & Block File Discovery

## Previous work (DONE)
- Sync stall FIXED (broken pprev heights, contextual check skip)
- Dynamic IBD download limits (4096 in-flight, 15s timeout)
- Address backfill SIGSEGV fixed
- pread() migration for thread safety

## Current state
Node is at ~2,019,335. Peers are at ~3,077,000. Syncing via P2P. We copied block files from zclassicd but the node didn't discover all the new block data in them — only a few thousand blocks were found.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `config/src/boot_index.c` — block file scanning, `scan_block_files_mark_data()`
- `lib/net/src/msg_headers.c` — block file scan trigger after headers arrive
- `lib/storage/src/disk_block_io.c` — reading blocks from blk*.dat files

## Tasks

### 1. Improve block file scan to find all blocks

The block file scan (`scan_block_files_mark_data`) runs once after first headers arrive. It scans blk*.dat files and marks matching block index entries with `BLOCK_HAVE_DATA`. But it may miss blocks if:
- The block hash doesn't match any entry in the block index (height mismatch)
- The scan stops too early

Review and fix: scan ALL blk*.dat files thoroughly, match blocks by hash, and set `BLOCK_HAVE_DATA` + file position info for every match. Log how many blocks were found vs how many block index entries exist.

### 2. Add reindex-from-block-files RPC

Add RPC command `rescanblockfiles` that triggers a full re-scan of all blk*.dat files, matching them against the block index and setting BLOCK_HAVE_DATA. This is useful after copying block files from zclassicd.

### 3. Monitor sync speed

Add logging every 60s during blocks_download showing:
- Blocks/second throughput
- Estimated time to tip
- Current download pipeline stats (in-flight, queued, timed out)

### 4. Verify bg_hash_verify is working

Check if bg_hash_verification_service is enabled after the pread() migration. If disabled, re-enable in `boot_services.c` and verify no crash.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- `make deploy` to verify on live node
- Commit with descriptive messages
