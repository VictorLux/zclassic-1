# Memory Diagnosis — Wave 25 Task 4

**Node:** PID 1618526, 59 threads, 2,615 MB RSS

## Root Cause: glibc malloc arenas (2,160 MB)

glibc allocates a 64MB per-thread malloc arena. With 59 threads, there are
110 arena regions totaling **2,160 MB RSS** — 83% of all memory usage.

Most of these arenas are nearly empty but can't be returned to the OS because
glibc holds them for potential future allocations from each thread.

### Fix Options (pick one)

1. **`MALLOC_ARENA_MAX=2`** in systemd service — limits arenas to 2 regardless
   of thread count. Saves ~2GB instantly. No code change.
   ```ini
   Environment=MALLOC_ARENA_MAX=2
   ```

2. **`mallopt(M_ARENA_MAX, 2)`** in `main()` — same effect, in code.

3. **jemalloc/tcmalloc** — better allocators that don't have this problem.

## Other Memory Consumers

| Component | RSS | Notes |
|-----------|-----|-------|
| glibc malloc arenas (110×64MB) | 2,160 MB | ROOT CAUSE |
| node.db SQLite mmap | 251 MB | Read-only, expected |
| block_index (3.3M × 192B) | ~634 MB | In heap arenas, unavoidable |
| block_map hash table | ~270 MB | In heap arenas |
| Thread stacks (59 × 8MB virtual) | ~0.5 MB | Virtual 472MB, RSS negligible |

Note: block_index and block_map live inside the malloc arenas, so they
overlap with the 2,160 MB arena total.

## nSolution Leak (minor, since boot loads NULL)

`nSolution` (1,344 bytes per block) is allocated in `add_to_block_index()`
(`process_block.c:317`) but never freed. When loaded from disk, it's set to
NULL (`block_index_db.c:288`, `boot_index.c:455`). So the leak only affects
blocks received since last restart — minimal for a node at tip.

Still worth fixing for long-running nodes that receive many headers.

## Projected RSS After Fix

- With `MALLOC_ARENA_MAX=2`: ~800-900 MB (block_index + node.db mmap + small heap)
- Target <1GB: **achievable** with arena limit alone
