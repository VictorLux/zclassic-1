# AGENT3 — Wave 26: Stress Testing + File Protocol

**Working directory:** `~/zclassic23-3`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Node at tip, healthy. Your soak script and SIGKILL recovery fix are merged. Now: stress test the node and implement the file protocol Phase 3.

---

## Task 1: Start the Soak Test

Run the soak test script you wrote:
```bash
nohup tools/soak_test.sh > soak_test.log 2>&1 &
```

Let it run. Check back periodically. If it catches issues, report them.

---

## Task 2: Re-test Multi-threaded bg_validation with 2 Workers

Your investigation concluded the script interpreter is thread-safe and the crash might be fixed by the cs_main locking from wave 22b.

1. Find where the worker count is set (likely `bg_validation_service.c`, look for `num_workers` or `MAX_WORKERS`)
2. Change it from 1 to 2
3. Build and deploy
4. Monitor for 5 minutes — check for crashes via `systemctl --user status zclassic23`
5. If stable, commit. If it crashes, revert and report the crash details.

---

## Task 3: Implement File Service Retry (ZCL Market)

### File: `lib/net/src/file_service.c`, line 1228

TODO: "retry failed chunks on next connect"

When a file chunk download fails, the current code gives up. Add retry logic:

1. Track failed chunks in a retry queue (chunk_hash + peer_id + attempt_count)
2. On peer connect, check if any failed chunks can be retried from the new peer
3. Max 3 retries per chunk, then mark as permanently failed
4. Log retries: `printf("[file] retrying chunk %s attempt %d\n", ...)`

---

## Task 4: P2P File Protocol Phase 3

### File: `lib/net/src/msgprocessor.c`, lines 1159, 1203, 1228

Three TODOs for the file chunk protocol:

1. **Line 1159:** "read actual file chunks and hash them" — when receiving a `zfile_chunk` message, verify the chunk data against the manifest hash
2. **Line 1203:** "verify against download session, advance state" — match received chunk to the active download, mark as complete
3. **Line 1228:** "unlock chunks for this peer's download" — when a peer disconnects, release any chunks assigned to it so other peers can download them

Read the surrounding code to understand the wire format and data structures, then implement.

---

## Build & Test

```bash
git pull origin master
make -j$(nproc) && make test
git add <files> && git commit -m "wave 26 task N: description"
git push origin master
```
