# Agent 2 Task: Wave 15 — Make activate_best_chain work after LDB reimport

## CRITICAL PROBLEM

We have 3,328,077 blocks with `BLOCK_HAVE_DATA` in the block index. The chain tip is stuck at 2,016,355. `rescanblockfiles` found 150,114 blocks above the tip and triggered `activation_request_connect`, but the chain tip didn't advance.

The block at height 2,016,356 exists in the index with `BLOCK_HAVE_DATA` but `activate_best_chain` won't connect it.

## Likely causes (investigate in this order)

1. **nChainTx == 0**: `find_most_work_chain` requires `nChainTx > 0` to consider a block for activation. LDB-imported blocks may not have `nChainTx` set. Previous commit `80cf863ae` tried to fix this but may not cover all cases.

2. **UTXO mismatch**: The coins tip hash might not match `block.hashPrevBlock` for block 2,016,356. The LDB reimport set `coins_best_block` but it may not match the block at our tip.

3. **pprev chain broken**: The block at 2,016,356 may have a broken `pprev` that doesn't point to our tip, so it's not on the "best chain" from activate's perspective.

## Files to read

- `CLAUDE.md` and `DEFENSIVE_CODING.md`
- `lib/validation/src/process_block.c` — `activate_best_chain()`, `find_most_work_chain()`
- `lib/validation/src/connect_block.c` — `connect_tip()`, the actual block connection
- `config/src/boot_index.c` — where nChainTx is propagated at boot
- `app/controllers/src/repair_controller.c` — the `rescanblockfiles` RPC that triggers activation
- `~/.zclassic-c23/node.log` — look for any error from `activate_best_chain`

## Tasks

### 1. Diagnose why activate_best_chain doesn't advance

Add diagnostic logging to `activate_best_chain` and `find_most_work_chain`:
- Log the candidate block (hash, height, nChainWork, nChainTx, nStatus)
- If no candidate found, log why
- If candidate found but connect_tip fails, log the failure reason
- Deploy and trigger with `rescanblockfiles` RPC, check log

### 2. Fix nChainTx propagation

In `boot_index.c`, after the block index is loaded, propagate `nChainTx` for all blocks with `BLOCK_HAVE_DATA`:
```c
/* For LDB-imported blocks, if nChainTx is 0 but BLOCK_HAVE_DATA,
 * set nChainTx = nTx (or 1 if nTx unknown). Without this,
 * find_most_work_chain skips them. */
```
This may need multiple passes since nChainTx = pprev->nChainTx + nTx.

### 3. Verify coins_best_block matches tip

At boot after LDB import, check that `coins_best_block` hash resolves to a block in the index and matches the active chain tip. If not, fix it.

### 4. Suppress checkpoint log spam

`checkpoints_hash_at_height()` logs for EVERY height without a checkpoint. Fix it to only log when a checkpoint EXISTS and the hash doesn't match (i.e., log violations, not non-events).

### 5. Deploy and verify

After fixing: `make deploy`, trigger `rescanblockfiles` via RPC, verify height advances past 2,016,355.

## Rules

- Follow `DEFENSIVE_CODING.md`: use `LOG_FAIL()`, `zcl_malloc()`, `log_macros.h`
- Run `make test` before committing
- `make deploy` to verify on live node
- Commit with descriptive messages
