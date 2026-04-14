# AGENT2 — Wave 23b: Sync Pipeline + Reliability

**Working directory:** `~/zclassic23-2`
**Coordinator:** Agent1 (~/zclassic23)
**Workflow:** `git pull origin master` before starting, `git push origin master` when done with each task.
**Run `make -j$(nproc) && make test` before every push.**

---

## Context

Wave 23 progress:
- Agent1 fixed `find_most_work_chain` (accept BLOCK_HAVE_DATA with nChainTx==0) and `syncsvc_should_begin_blocks_download` (allow in BLOCKS_DOWNLOAD state)
- Agent3 fixed false AT_TIP in activation controller
- Node now sends getdata but hits `bad-txns-inputs-missingorspent` at h=2016356 — UTXO reimport triggered
- Headers fully synced to 3,078,027. Blocks stuck at 2,016,355 pending UTXO fix.

Your job: harden the sync pipeline so it recovers from UTXO mismatches without manual intervention, and enable the two features fixed in wave 22b.

---

## Task 1: Improve Self-Heal for bad-txns-inputs-missingorspent

### File: `lib/validation/src/process_block.c` (lines ~985-1065)

The self-heal mechanism tries to recover missing UTXOs from the tx index. But it currently requires `g_active_block_tree != NULL` (LevelDB tx index). Check:

1. Is `g_active_block_tree` set when `-txindex` is enabled? Verify in boot.c
2. If self-heal fails, it marks the block as BLOCK_FAILED. After 3 failures, writes `needs_reimport`. This is too slow — it takes 3 activation cycles. Add a log line when self-heal fires: `event_emitf(EV_SELF_HEAL, ...)` so we can see it in the event log
3. After successful UTXO reimport (boot.c reads `needs_reimport`), delete the flag file

---

## Task 2: Re-enable bg_hash_verify

The SIGSEGV was fixed in wave 22b (cs_main lock + field snapshotting in `bg_hash_verification_service.c`).

Search for where bg_hash_verify is disabled:
```bash
grep -rn 'bg_hash_verify\|bg_hash_verification\|nobghash' app/ config/ lib/ --include='*.c' --include='*.h'
```

If it's disabled by a commented-out call or a hardcoded flag, re-enable it. If gated by `-nobgvalidation`, leave it user-controlled.

---

## Task 3: Re-enable Address Backfill

The SIGSEGV was fixed in wave 22b (mmap_size=0 in `boot_index.c`).

Search for where backfill is disabled:
```bash
grep -rn 'backfill_address\|address_backfill\|nobackfill' app/ config/ lib/ --include='*.c' --include='*.h'
```

Re-enable it if disabled.

---

## Task 4: Update CHECKLIST.md

Mark these items as FIXED in section 6 "Remaining":
- SIGSEGV in bg_hash_verify — FIXED wave 22b (cs_main lock)
- SIGSEGV in address backfill — FIXED wave 22b (mmap_size=0)
- Block download stalling — FIXED wave 23 (HAVE_DATA in chain selection + state gate)

Add new remaining item:
- bad-txns-inputs-missingorspent at h=2016356 — UTXO snapshot mismatch after restart, auto-reimport triggered

---

## Build & Test

After EACH task:
```bash
git pull origin master
make -j$(nproc) 2>&1 | tail -20
make test 2>&1 | tail -10
git add <specific files> && git commit -m "wave 23b task N: description"
git push origin master
```

## Boundary: Files You MUST NOT Touch
- `app/services/src/header_sync_service.c` (Agent1)
- `app/services/src/chain_activation_controller.c` (Agent3)
