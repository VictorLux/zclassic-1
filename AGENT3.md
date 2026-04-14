# AGENT3 — Wave 20: Fix Reporting Lies & Add Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context: What Agent1 Fixed & What's Still Broken

Agent1 fixed the chain activation deadlock (blocks never connected after LDB import) and added batching to prevent OOM. The node IS connecting blocks now, advancing 500 at a time.

**Still broken:** the RPC and MCP endpoints LIE about sync state. `getblockchaininfo` reports `headers: 2016354` (same as blocks) when the actual best_header is at 3M+. `verificationprogress` is hardcoded to 1.0. Operators and AI agents can't tell the node is behind.

---

## Task 1: Fix getblockchaininfo to Report Real Headers Height

### File: `app/controllers/src/blockchain_controller.c`

Find the `getblockchaininfo` handler and fix:

**Fix 1: headers field**
Currently both `blocks` and `headers` use `active_chain_tip()->nHeight`. The `headers` field must report `pindex_best_header->nHeight`:
```c
// Find where headers is set and change to:
struct block_index *best_hdr = ms->pindex_best_header;  // or however main_state is accessed
int header_height = best_hdr ? best_hdr->nHeight : (tip ? tip->nHeight : 0);
json_push_kv_int(result, "headers", header_height);
```

**Fix 2: verificationprogress**
Currently hardcoded to 1.0. Calculate real progress:
```c
int max_peer_h = connman_max_peer_height(connman);
double progress = 1.0;
if (max_peer_h > 0 && our_h < max_peer_h)
    progress = (double)our_h / (double)max_peer_h;
json_push_kv_real(result, "verificationprogress", progress);
```

You'll need access to `main_state` and `connman`. Look at how `network_controller.c` accesses these — follow the same pattern via the controller context struct.

---

## Task 2: Add Download Pipeline Stats to MCP zcl_status

### File: wherever `zcl_status` MCP tool is implemented (likely `tools/mcp/controllers/ops_controller.c`)

Add download pipeline visibility to the status output:
```json
{
    "download": {
        "queue_size": 0,
        "in_flight": 0,
        "total_requested": 500,
        "total_received": 498,
        "total_timed_out": 2
    },
    "header_height": 3078000,
    "max_peer_height": 3078009,
    "memory_rss_mb": 2048,
    "uptime_secs": 1260
}
```

Implementation:
1. Call `dl_get_stats()` for download pipeline numbers
2. Read `pindex_best_header->nHeight` for header height
3. Call `connman_max_peer_height()` for peer max (check if this function exists — if not, scan peer list)
4. Get RSS from `/proc/self/status` VmRSS line
5. Track boot time with `static time_t boot_time = 0; if (!boot_time) boot_time = time(NULL);`

---

## Task 3: Add Tests for False AT_TIP Detection

### File: `lib/test/src/test_sync_service.c`

Test `syncsvc_note_valid_block()` edge cases:

**Test 1: Should NOT transition to AT_TIP when 1M blocks behind**
```c
// node.starting_height = 2016354 (stale — peer was at same height at handshake)
// new_tip_height = 2016354, best_header_height = 2016354
// But network is at 3078000!
// Result: should NOT set AT_TIP because starting_height is stale
```

**Test 2: Should transition to AT_TIP with recent tip time**
```c
// new_tip_time = now - 60 (block is fresh)
// new_tip_height = 3078000
// Result: SHOULD set AT_TIP via tip_is_recent path
```

**Test 3: headers_caught_up should require headers near peer max**
```c
// best_header_height = 2016354, new_tip_height = 2016354
// node.starting_height = 2016354
// This currently passes headers_caught_up — document the bug
```

---

## Task 4: Add Structured Boot Timing Log

### File: `config/src/boot.c`

Add timing around each boot phase:
```c
struct timespec phase_start;
clock_gettime(CLOCK_MONOTONIC, &phase_start);
// ... phase code ...
int64_t ms = elapsed_ms_since(&phase_start);
printf("[boot] %-30s %lldms\n", "load_block_index", ms);
```

Phases: SQLite open, block index load, height repair, UTXO load, wallet load, P2P start, total.

---

## Task 5: Add Memory RSS to Health Check

### File: `app/services/src/node_health_service.c`

Add RSS monitoring so the health check can warn about high memory:
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

Add to health output: `"memory_rss_mb": rss_kb / 1024`
Add health check: if RSS > 4096 MB, set `degraded_reason = "high_memory_usage"`.

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 20 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `lib/net/src/msg_headers.c` (Agent1)
- `lib/validation/src/process_block.c` (Agent1)
- `lib/net/src/download.c` (Agent2)
- `lib/net/src/msgprocessor.c` (Agent1)
