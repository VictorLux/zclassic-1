# AGENT3 — Wave 19: Testing Depth, P2P Hardening & Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

Wave 18 is done (memory reduction, WAL checkpoint, PID lock, OOM protection, crash recovery events). Wave 19 focuses on test coverage for the new robustness code, P2P message hardening, and making the node's observability production-grade.

---

## Task 1: Test Coverage for Wave 17-18 Robustness Features

Many new features were added across waves 17-18 but some may have thin test coverage. Audit and fill gaps.

### Files to audit:
- `lib/test/src/test_sync_watchdog.c` — Does it test HEADER_LAG detection? Escalating recovery? Progress rate tracking?
- `lib/test/src/test_block_index_integrity.c` — Does it test bulk height repair with realistic scrambled heights?
- `lib/test/src/test_robustness.c` — Does it test PID lock, OOM protection, crash detection?
- `lib/test/src/test_activerecord.c` — Does it test the before_save hooks on utxo/block/wallet_tx?

### What to do:
1. Read each test file
2. For each feature added in waves 17-18, verify there's a test that exercises it
3. Add missing tests. Focus on:
   - Edge cases: what happens when height repair encounters a cycle in pprev?
   - Boundary: watchdog with exactly 0 peers, exactly at timeout boundary
   - Failure modes: before_save hook that rejects, after_save event emission
4. Target: at least 2 new tests per file where gaps exist

---

## Task 2: P2P Message Fuzzing Hardening

Malformed P2P messages shouldn't crash the node. Harden the message handlers.

### Files to investigate:
- `lib/net/src/msg_headers.c` — process_headers()
- `lib/net/src/msg_blocks.c` — process_block() 
- `lib/net/src/msg_version.c` — process_version()
- `lib/net/src/msg_tx.c` — process_tx()

### What to check and fix for each handler:
1. Does it validate stream length before reading? (prevent buffer over-read)
2. Does it handle truncated messages gracefully? (return false, don't crash)
3. Does it disconnect on malformed input rather than crashing?
4. Are all error paths logged?

### Test:
- Add fuzz-style tests in `lib/test/src/test_msg_handlers.c` that feed truncated/garbage data to each handler and verify no crash + clean error

---

## Task 3: MCP Health Endpoint Completeness

The `zcl_health` and `zcl_status` MCP tools should be a one-stop diagnostic.

### Files to investigate:
- `tools/mcp/controllers/ops_controller.c` or wherever `zcl_health` and `zcl_status` are implemented
- `app/services/src/node_health_service.c`

### What to add to health output:
1. **Memory usage**: RSS from `/proc/self/status` (VmRSS line)
2. **Uptime**: seconds since boot
3. **Block index size**: entry count + estimated MB
4. **WAL file size**: size of `node.db-wal` if it exists
5. **Disk free**: available space on datadir partition

### Implementation:
```c
// RSS from /proc/self/status
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

---

## Task 4: Rate-Limit Peer Misbehavior Scoring

The peer scoring system should prevent a single misbehaving peer from consuming resources.

### Files to investigate:
- `lib/net/src/peer_scoring.c` — Current scoring implementation
- `lib/net/include/net/peer_scoring.h` — Score types and thresholds

### What to check and improve:
1. Read the current scoring system
2. Verify that peers reaching the ban threshold are actually disconnected
3. Add rate limiting: if a peer sends >100 rejected messages in 60 seconds, disconnect immediately (don't wait for score threshold)
4. Add logging: `[scoring] peer %s banned: score %d, offences: %s`

### Test:
- Test that rapid-fire offences trigger disconnect before the normal threshold

---

## Task 5: Add Structured Boot Timing Log

The boot sequence should log timing for each phase so we can identify slow spots.

### Files to modify:
- `config/src/boot.c` — Add timing around each boot phase

### What to implement:
```c
// At start of boot:
struct timespec boot_start;
clock_gettime(CLOCK_MONOTONIC, &boot_start);

// After each phase:
int64_t phase_ms = elapsed_ms_since(&boot_start);
printf("[boot] %-30s %lldms\n", "load_block_index", phase_ms);
// Reset for next phase

// At end of boot:
printf("[boot] total boot time: %lldms\n", total_ms);
event_emitf(EV_BOOT_COMPLETE, 0, "total_ms=%lld phases=%d", total_ms, num_phases);
```

Phases to time:
1. SQLite open + schema migration
2. Block index load from cache
3. Block index height repair
4. UTXO set load / verification
5. Wallet load
6. P2P network start
7. Total boot time

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 19: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/services/src/block_index_integrity.c` (Agent2)
- `lib/net/src/msg_headers.c` (Agent2)
- `lib/net/src/download.c` (Agent2)
- `lib/net/src/connman.c` (Agent2)
- `lib/net/src/compact_blocks.c` (Agent2)
