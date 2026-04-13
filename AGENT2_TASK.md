# Agent 2 Task: Wave 13 — Fix the Sync Stall (URGENT)

## CRITICAL: Node stuck at height 2,015,124

The node is stuck in `headers_download` at height 2,015,124 while peers report height 3,077,062. It has 17 peers, the stall detection code from wave 12 is deployed, and yet the node STILL won't advance. This means headers are being received but rejected.

## Diagnosis needed

The stall detection disconnects peers, but the problem isn't the peers — it's that `accept_block_header()` is rejecting valid headers. The node has been stuck for 30+ minutes across restarts.

## Files to read first

- `CLAUDE.md` and `DEFENSIVE_CODING.md` — mandatory coding rules
- `lib/validation/src/process_block.c` — `accept_block_header()` function
- `lib/net/src/msgprocessor.c` lines 1764-1841 — `process_headers()`, especially the reject logging
- `lib/chain/src/pow.c` — difficulty checking
- `CHECKLIST.md` — validation item 4.2 (difficulty), 4.31 (checkpoints)
- `lib/chain/src/checkpoints.c` — checkpoint enforcement

## Tasks

### 1. Diagnose why headers are rejected

Add diagnostic logging to `accept_block_header()` in `process_block.c`:
- Log the FIRST rejected header each batch: hash, height, prev hash, and the exact reject reason
- Log the state of the block index: does `hashPrevBlock` exist in our index? What height does it have?
- This is the critical question: are we rejecting because the prev block isn't in our index (orphan headers), or because validation fails (bad PoW, bad timestamp, checkpoint mismatch)?

### 2. Check for block index corruption

The node was synced to 2,015,124 via snapshot. The block index was loaded from `block_index.bin`. Headers from peers start at the peer's view of our chain (from our locator). If our locator sends stale/wrong hashes, peers send headers we already have, and they get rejected as duplicates.

Check `syncsvc_build_getheaders_locator()` in `header_sync_service.c`:
- Is the locator built correctly from our chain tip?
- Does it include heights up to 2,015,124?
- Could the locator be sending hashes from a fork?

### 3. Check the getheaders→headers round-trip

Add a counter: how many `getheaders` messages sent vs `headers` responses received? If we're sending but getting no responses, peers don't understand our locator. If we're getting responses but all are rejected, the rejection reason tells us what to fix.

Add to `msg_send_messages()` at the periodic getheaders section:
```c
static uint64_t getheaders_sent = 0, headers_responses = 0;
// Log every 60s: "getheaders stats: sent=%llu responses=%llu"
```

### 4. Fix whatever the diagnosis reveals

Based on the diagnostic logging, fix the root cause. Common possibilities:
- Locator hashes don't match any block the peer knows → fix locator construction
- Headers rejected as duplicate (already in index) → skip duplicates silently, request next batch
- Headers rejected due to checkpoint mismatch → check checkpoint heights
- Headers rejected due to missing prev → we have a gap in our index

### 5. Thread safety (secondary, after sync is fixed)

Migrate `disk_block_io.c` from `fseek/fread` to `pread()` for thread safety:
- Replace `g_cached_file` file handle cache with `open/pread/close`
- This fixes the SIGSEGV in bg_hash_verify (CHECKLIST item)
- Add test in `lib/test/src/test_disk_block_io.c`

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- Deploy with `make deploy` after fixing the sync stall so we can verify on the live node
- Commit with descriptive messages
