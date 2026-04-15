# AGENT3 — Wave 25: Resilience Testing + Validation

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

**NODE IS AT TIP (h=3,078,918) AND HEALTHY.** Your Sapling checkpoint code is merged. Time to test resilience and fix the last validation gap.

---

## Task 1: SIGKILL Recovery Test

Test that SIGKILL recovery works correctly:

1. Note the current block height
2. `kill -9 $(pidof zclassic23)` — hard kill, no clean shutdown
3. `systemctl --user start zclassic23` — restart
4. Wait for RPC, check height — should be at or near where it was
5. Check Sapling tree rebuild time — your periodic checkpoint should make this seconds, not 5 minutes
6. Report: recovery time, blocks lost, Sapling rebuild time

If recovery takes >30 seconds or the Sapling rebuild takes >1 minute, investigate why.

---

## Task 2: Multi-threaded bg_validation Fix

Your wave 24 investigation found the issue. Apply the fix:

Based on your findings, either:
- Create per-thread `secp256k1_context` instances
- Or add proper locking around shared interpreter state
- Or whatever the correct fix is from your investigation

If the fix is applied, test with 2 workers: change the worker count from 1 to 2 and verify no crash during bg_validation.

---

## Task 3: Reorg Safety Test

Test that the node handles a chain reorganization correctly:

1. Read `lib/test/src/test_reorg_safety.c` — what scenarios are tested?
2. Are there tests for disconnect_tip with undo data?
3. Add a test: create a 3-block chain, connect all 3, then disconnect the tip. Verify UTXO set is restored correctly (spent outputs reappear, created outputs disappear).

---

## Task 4: Write Soak Test Script

Create `tools/soak_test.sh` that monitors the node for 72 hours:

```bash
#!/bin/bash
# Soak test: monitor node health every 5 minutes for 72 hours
# Checks: height advancing, RSS stable, peers connected, no crashes
# Logs to: soak_test.log

DURATION_HOURS=72
INTERVAL_SECS=300
```

The script should:
- Log height, RSS, peer count, sync state every 5 minutes
- Alert if height stops advancing for 30 minutes
- Alert if RSS exceeds 4GB
- Alert if peer count drops to 0
- Alert if the process dies (restart it)
- Write a summary at the end: uptime, blocks processed, max RSS, restarts

---

## Build & Test

```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 25 task N: description"
git push origin master
```
