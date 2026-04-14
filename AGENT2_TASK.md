# Agent 2 Task: Wave 16 — Fast Header Skip for Known Blocks

## Status
- All tests pass (0 failures!)
- Node is at 2,016,354, syncing headers toward 3,077,000
- Headers advancing at 160 per round trip = ~6,250 rounds for 1M headers
- At 10s per round that's ~17 hours — TOO SLOW

## Problem
The node has 3.3M block index entries with BLOCK_HAVE_DATA. It knows about all the blocks. But the header sync protocol crawls through them 160 at a time because accept_block_header processes each one individually.

## The fix: bulk-skip known headers

When we receive a batch of 160 headers and ALL are already in our index, we should skip ahead to the HIGHEST block in our index that we haven't yet walked past, instead of going 160 at a time.

## Files to read first
- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `lib/net/src/msg_headers.c` — `process_headers()`, the "request more" section at the end
- `app/services/src/header_sync_service.c` — batch evaluation
- `lib/validation/src/process_block.c` — `accept_block_header()`, `find_most_work_chain()`

## Tasks

### 1. Skip ahead when all headers are already known

In `process_headers()` in `msg_headers.c`, after processing the batch: if `newly_added == 0` (all headers already known) AND `pindex_last` is below the highest known header, jump ahead.

Find the highest block in the index with `BLOCK_HAVE_DATA` and a valid pprev chain. Send `getheaders` from THAT block instead of from `pindex_last`. This skips the entire known range in one jump.

```c
if (newly_added == 0 && accepted > 0) {
    /* Find highest block with HAVE_DATA in the block index */
    struct block_index *highest = NULL;
    // ... scan block_map for highest HAVE_DATA entry ...
    if (highest && highest->nHeight > pindex_last->nHeight + 1000) {
        printf("Headers: skipping %d known blocks (h=%d → h=%d)\n",
               highest->nHeight - pindex_last->nHeight,
               pindex_last->nHeight, highest->nHeight);
        push_getheaders_from(mp, node, highest);
    }
}
```

### 2. Or: skip directly to header sync → block download transition

Even simpler: if we have BLOCK_HAVE_DATA for all heights up to some point, and our pindex_best_header covers them, skip straight to SYNC_BLOCKS_DOWNLOAD and start connecting blocks. The header validation already happened when the blocks were imported.

### 3. Fix the getheaders locator stall

The log shows the same locator hash repeating many times before advancing. This means the peer keeps sending us the same 160 headers. The locator should use the LAST header we received (pindex_last), not a stale hash. Verify the locator advances correctly.

### 4. Deploy and verify

After fixing: `make deploy`, check that header sync jumps ahead and the node starts connecting blocks rapidly.

## Rules
- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` — must stay at 0 failures
- `make deploy` to verify
- Commit with descriptive messages
