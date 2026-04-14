# AGENT — Wave 20: Fix Sync Stall (Coordinator)

## Current Mission

Make `zclassic23` a reliable, secure, fast full node that syncs to tip.

**CRITICAL:** Node stuck at 2,016,354, network tip ~3,078,009.

---

## Wave 20 — Agent1 Tasks: msgprocessor Fix + Integration

**Working directory:** `~/zclassic23`
**Run `make -j$(nproc) && make test && make lint` before every push.**

---

## Context

Root cause: node falsely enters `SYNC_AT_TIP` when headers==blocks but both are 1M behind peers. See AGENT2.md for the full chain. Agent1 handles msgprocessor, connman, and integration testing.

---

## Task 1 (DO FIRST): Push Wave 20 Plans to Master

```bash
cd ~/zclassic23
git pull origin master
# Copy the AGENT2.md and AGENT3.md from the other worktrees
cp ~/zclassic23-2/AGENT2.md .
cp ~/zclassic23-3/AGENT3.md .
git add AGENT.md AGENT2.md AGENT3.md
git commit -m "wave 20: fix sync stall — false SYNC_AT_TIP root cause"
git push origin master
```

---

## Task 2: Add `connman_max_peer_height()` if Missing

### Files: `lib/net/src/connman.c` + `lib/net/include/net/connman.h`

Agent2 and Agent3 both need this. Check if it exists:
```bash
grep -rn 'connman_max_peer_height' lib/net/
```

If missing, add:
```c
int connman_max_peer_height(const struct connman *cm)
{
    if (!cm) return -1;
    int max_h = -1;
    for (int i = 0; i < cm->manager.num_nodes; i++) {
        struct p2p_node *n = &cm->manager.nodes[i];
        if (n->starting_height > max_h)
            max_h = n->starting_height;
    }
    return max_h;
}
```

---

## Task 3: Fix msgprocessor Send Loop — Ensure Getheaders at AT_TIP

### File: `lib/net/src/msgprocessor.c`

Around line ~2370-2500, the send loop calls `syncsvc_plan_periodic_getheaders()` but it may not be reached when at `SYNC_AT_TIP`.

**Investigation:**
1. Read lines 2370-2500 of msgprocessor.c
2. Trace: does `syncsvc_begin_peer_sync()` (line ~2373) gate the periodic path?
3. If `should_sync` starts false and the periodic path is inside `if (should_sync)`, it never fires

**Fix:** Move `syncsvc_plan_periodic_getheaders()` outside any `should_sync` gate. It must fire for all outbound peers. The interval function already handles the cadence (120s at tip, 10s in IBD).

---

## Task 4: Integration Verification After Agents Merge

After Agent2 pushes the `block_sync_service.c` and `sync_watchdog_service.c` fixes:

1. `git pull && make -j$(nproc) && make test`
2. `make deploy`
3. Wait 120s, then check:
   ```bash
   ./tools/zcl-rpc getblockchaininfo   # headers should now differ from blocks
   ./tools/zcl-rpc getpeerinfo          # peers should be syncing
   ```
4. Monitor height advancement every 60s for 5 minutes
5. If still stuck, check logs for `[watchdog]` and `[headers]` lines

---

## Boundary: Files You MUST NOT Touch
- `app/services/src/sync_watchdog_service.c` (Agent2)
- `app/services/src/block_sync_service.c` (Agent2)
- `app/services/src/header_sync_service.c` (Agent2/Agent3)
- `app/controllers/src/blockchain_controller.c` (Agent3)
- `tools/mcp/controllers/ops_controller.c` (Agent3)
