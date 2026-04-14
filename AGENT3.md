# AGENT3 — Wave 21: Testing Infrastructure & Observability

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

The sync pipeline has been the source of multiple stuck-node bugs. Agent1 is fixing the activation and chain selection code. Your focus is on **test coverage** that catches these classes of bugs, **observability** so we can diagnose them faster, and **defensive patterns** that prevent them.

Read `DEFENSIVE_CODING.md` first.

---

## Task 1: Test Suite for Chain Activation Edge Cases

The chain activation path (`activate_best_chain` in process_block.c) has had 3 bugs in the last 24 hours. Add tests that would have caught them.

### File: `lib/test/src/test_chain_activation_controller.c` (or similar)

**Test cases to add:**

a) **Batched activation**: Create a chain with 1000+ blocks to connect. Verify `activate_best_chain` connects in batches (not all at once) and flushes between batches.

b) **Broken pprev doesn't reject chain**: Create block_index entries where pprev is NULL above height 0 but nChainTx > 0. Verify `find_most_work_chain` still selects them (this was the regression from Agent2's guard).

c) **Fork point with broken pprev**: When fork-finding can't walk pprev to find common ancestor, verify it uses tip as fork point (not NULL which causes genesis reset).

d) **BLOCK_HAVE_DATA but file missing**: Set BLOCK_HAVE_DATA on a block whose blk file doesn't exist. Verify `connect_tip` clears the flag and returns a meaningful error.

---

## Task 2: Add Memory RSS to Health Check + zcl_status

### File: `app/services/src/node_health_service.c`

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

Add to health output:
- `memory_rss_mb`: RSS in MB
- Health check: if RSS > 4096 MB, set `degraded_reason = "high_memory_usage"`

Also add to MCP `zcl_status` output (in ops_controller.c):
- `memory_rss_mb`
- `uptime_secs` (track with `static time_t boot_time`)

---

## Task 3: Structured Boot Timing Log

### File: `config/src/boot.c`

Add timing around each boot phase using `clock_gettime(CLOCK_MONOTONIC)`:

```c
struct timespec phase_start;
clock_gettime(CLOCK_MONOTONIC, &phase_start);
// ... phase code ...
int64_t ms = /* elapsed since phase_start */;
printf("[boot] %-30s %lldms\n", "load_block_index", (long long)ms);
```

Phases to time:
1. SQLite open + schema migration
2. Block index load from cache/LDB
3. Height repair
4. UTXO set load
5. Wallet load
6. P2P network start
7. Total boot time

Emit at end: `event_emitf(EV_BOOT_COMPLETE, 0, "total_ms=%lld", total_ms);`

---

## Task 4: Add Test Helpers for Sync Pipeline Testing

Create reusable test helpers that make it easy to set up sync scenarios.

### File: `lib/test/include/test/test_sync_helpers.h` (new)

```c
/* Create a mock block_index chain of given length with proper pprev links */
struct block_index *test_create_chain(int length, struct block_map *map);

/* Create a block_index entry with BLOCK_HAVE_DATA set */
struct block_index *test_create_block_with_data(int height, struct block_map *map);

/* Create a block_index entry WITHOUT pprev (simulating LDB import) */
struct block_index *test_create_orphan_block(int height, struct block_map *map);

/* Set up a download_manager with N queued blocks */
void test_setup_download_queue(struct download_manager *dm, int count);

/* Create a minimal main_state for testing activation */
struct main_state *test_create_main_state(int tip_height);
```

These helpers eliminate the boilerplate that makes sync tests hard to write, which is why we have so few of them.

---

## Task 5: Verify and Fix make lint Coverage

Run `make lint` and check what it actually catches. Add missing rules from DEFENSIVE_CODING.md:

1. `check-raw-sqlite`: verify it catches all raw `sqlite3_step` in app/ code
2. `check-silent-errors`: verify it catches bare `return -1;` without logging
3. `check-malloc`: verify it catches raw malloc/calloc without zcl_ prefix
4. Add `check-before-save`: verify all model files have `DEFINE_MODEL_CALLBACKS` (new rule)

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
make lint 2>&1 | tail -10
git add <specific files> && git commit -m "wave 21 task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `lib/validation/src/process_block.c` (Agent1)
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/services/src/header_sync_service.c` (Agent2)
- `lib/net/src/msg_headers.c` (Agent1)
- `lib/net/src/download.c` (Agent2)
- `app/models/src/*.c` (Agent2 — hooks)
