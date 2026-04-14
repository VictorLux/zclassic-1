# Agent 2 Task: FIX THE SYNC STALL (CRITICAL, DO THIS FIRST)

## Root Cause (DIAGNOSED — see node.log evidence below)

The node is stuck at height 2,015,124. The log shows:

```
contextual_check_block_header failed for header at height 2
contextual_check_block_header failed for header at height 1
contextual_check_block_header failed for header at height 5
contextual_check_block_header failed for header at height 79
HEADER REJECT[2]: reason=bad-equihash-solution-size
Peer 140.174.189.17:8033: accepted 137/160 headers (header tip=2, chain tip=2015124, peer=3077146)
STALL DETECTED: accepted 137 headers but header tip=2 < chain tip=2015124
```

**What's happening**: After snapshot sync + LDB import, `block_map` entries have completely broken `pprev` chains. The pprev pointers trace back to genesis instead of the real chain. When a NEW header arrives whose `hashPrevBlock` is in the index, `pindex_prev->nHeight` computes to 1, 2, 3 etc. instead of ~2,015,125. Then `contextual_check_block_header()` applies pre-Sapling rules (wrong Equihash solution size) and rejects the header.

137/160 headers are accepted (already in index, no contextual check). 23 are NEW but rejected because prev height is wrong. The node never advances.

## The Fix

In `lib/validation/src/process_block.c`, function `accept_block_header()`, around line 606:

**Option A (recommended)**: Skip `contextual_check_block_header` for new headers during IBD when `pindex_prev` is already in our index and our chain tip is high (>100K). The headers will get full validation when we connect the blocks. This mirrors Bitcoin Core's behavior — header-first sync trusts header chain structure, full validation happens at block connection.

```c
/* Skip contextual header check during IBD when block index has
 * scrambled heights from snapshot/LDB import. The pprev chains are
 * broken, so pindex_prev->nHeight is unreliable. Full validation
 * happens later in connect_block(). */
int tip_h = active_chain_height(&ms->chain_active);
bool skip_contextual = (tip_h > 100000 && pindex_prev &&
                         pindex_prev->nHeight < tip_h - 1000);
if (pindex_prev && !skip_contextual &&
    !contextual_check_block_header(header, state, params, pindex_prev,
                                    ms->fCheckpointsEnabled))
    LOG_FAIL("validation", "contextual_check_block_header failed...");
```

**Option B (more conservative)**: Fix the pprev chains at boot time in `boot_index.c` so heights are correct before any headers arrive. But this is harder — the pprev chains may have cycles or disconnected segments.

## Files to read

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `lib/validation/src/process_block.c` — `accept_block_header()` starting at line 508
- `~/.zclassic-c23/node.log` — live evidence of the bug (read last 100 lines)
- `config/src/boot_index.c` — where block index is loaded and heights computed

## After fixing the stall

1. `make -j$(nproc) && make deploy` — deploy to live node
2. Check `~/.zclassic-c23/node.log` — verify headers are now accepted
3. Check height via RPC: `./tools/zcl-rpc getblockcount` — should advance past 2,015,124
4. Commit with descriptive message

## Secondary task: thread safety

After the sync stall is FIXED and VERIFIED on the live node:
- Verify `disk_block_io.c` pread migration is working (from previous wave)
- Check bg_validation is running with multiple workers

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- `make deploy` after fixing the stall — we need to verify on the live node
- Commit with descriptive messages
