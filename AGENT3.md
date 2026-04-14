# AGENT3 — Wave 22b: Crash Recovery + Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Your wave 22 tasks are DONE (LOG_FAIL fix, before_save hooks, fprintf→LOG_ERR). Tests pass. Agent2 is fixing thread safety bugs.

Now move to crash recovery and observability — preparing for wave 23 (reliability under stress).

---

## Task 1 (HIGH): Add Memory RSS to Health Check

### File: `app/services/src/node_health_service.c`

Add a function to read RSS from `/proc/self/status`:
```c
static int64_t get_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            int64_t kb = 0;
            sscanf(line + 6, " %lld", (long long *)&kb);
            fclose(f);
            return kb;
        }
    }
    fclose(f);
    return -1;
}
```

Add to the health check output:
- `memory_rss_mb` field (RSS in MB)
- If RSS > 4096 MB, set `degraded_reason = "high_memory_usage"`

### Also update MCP ops_controller.c
In `zcl_status` output, add `memory_rss_mb` and `uptime_secs`.

---

## Task 2 (HIGH): Structured Boot Timing

### File: `config/src/boot.c`

Add timing around each boot phase using `clock_gettime(CLOCK_MONOTONIC)`:

```c
struct timespec t0;
clock_gettime(CLOCK_MONOTONIC, &t0);
// ... phase code ...
struct timespec t1;
clock_gettime(CLOCK_MONOTONIC, &t1);
int64_t ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
printf("[boot] %-30s %lldms\n", "phase_name", (long long)ms);
```

Phases to time:
1. SQLite open + schema migration
2. Block index load from cache/LDB
3. UTXO set load/import
4. Sapling tree load/rebuild
5. Wallet load
6. P2P network start
7. Total boot time

Print a summary line at end: `[boot] total: Xms`

---

## Task 3 (MEDIUM): Investigate bg_hash_verify SIGSEGV

### File: `app/services/src/bg_hash_verification_service.c`

This feature is DISABLED because it crashes at h=20000 when P2P is running. The audit found that bg_hash_verify itself uses `pread()` (thread-safe) — so the crash is NOT from the file handle race.

Investigate:
1. Read the bg_hash_verify thread function thoroughly
2. What data structures does it access? Does it read `block_index` entries that could be modified by P2P?
3. Does it access any shared state (block_map, chain tip, etc.) without locks?
4. Check if `pindex->nFile` or `pindex->nDataPos` could change while bg_hash_verify is reading

Report your findings. If you can identify the race, fix it. If not, document what you found for the next wave.

---

## Task 4 (MEDIUM): Investigate Address Backfill SIGSEGV

### File: `config/src/boot_index.c`

The `backfill_addresses_thread` crashes after ~64K addresses. The code comment says:
> "The old single-query approach... caused SIGSEGV after ~64K addresses due to SQLite sort buffer / mmap memory pressure."

The current batch-cursor approach uses `PRAGMA mmap_size=67108864` + `PRAGMA temp_store=FILE`.

Investigate:
1. Read the backfill thread code
2. Is `mmap_size=67108864` (64MB) too aggressive for a background thread?
3. Could reducing `mmap_size` or removing it fix the crash?
4. Is the SQLite connection properly isolated from the main thread's connection?

If the fix is simple (e.g., remove the mmap_size pragma), do it. Otherwise document findings.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 22b task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/block_pruning_service.c` (Agent2)
- `config/src/boot_index.c` scan/resolve functions (Agent2 — lines 491-733)
- `lib/storage/src/disk_block_io.c` (Agent2)
- Address backfill in `boot_index.c` is yours (lines 297-416) — Agent2 owns the scan functions
