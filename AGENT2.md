# AGENT2 — Wave 25: Production Hardening

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

**NODE IS AT TIP (h=3,078,918) AND HEALTHY.** The sync pipeline, UTXO import, and crash recovery are all working. Time to harden for production.

---

## Task 1: Re-enable bg_hash_verify

The SIGSEGV was fixed in wave 22b (cs_main lock + field snapshotting in `bg_hash_verification_service.c`).

Find where it's disabled:
```bash
grep -rn 'bg_hash_verify\|bg_hash_verification\|nobghash\|hash_verify_disabled\|skip.*hash.*verif' app/ config/ lib/ --include='*.c' --include='*.h'
```

If disabled by a hardcoded bool, commented-out call, or conditional that's always false — re-enable it. If gated by `-nobgvalidation`, that's fine (user-controlled).

---

## Task 2: Re-enable Address Backfill

The SIGSEGV was fixed in wave 22b (mmap_size=0 in `boot_index.c`).

Find where it's disabled:
```bash
grep -rn 'backfill_address\|address_backfill\|nobackfill\|skip.*backfill\|backfill_disabled' app/ config/ lib/ --include='*.c' --include='*.h'
```

Re-enable if hardcoded off.

---

## Task 3: Tor Onion Service Health

The health check shows `tor_ready: false` and `onion_service_ready: false`. The node runs with `-tor` on the test instance but the main service doesn't have `-tor`.

Check: is the main service supposed to have Tor? Look at the service file:
```bash
cat ~/.config/systemd/user/zclassic23.service
```

If `-tor` is missing and should be there, add it. If Tor is correctly disabled for the main instance, update the health check to not flag `tor_ready: false` as degraded when Tor isn't enabled.

---

## Task 4: Memory Usage — 2.5GB is High

The node is using 2,499 MB RSS. The target from the checklist is <1GB. Investigate:

1. Where is the memory going? The block_index has ~3.3M entries at 192 bytes = ~634MB. What's the other 1.8GB?
2. Check if `nSolution` heap ptrs are being freed for blocks that have been validated
3. Check if the coins cache is growing unbounded
4. Profile with `/proc/self/smaps` or add a memory breakdown to `zcl_status`

Don't optimize yet — just diagnose and report what's using memory.

---

## Build & Test

```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 25 task N: description"
git push origin master
```
