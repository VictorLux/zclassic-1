# Concurrency UAF: lock-free `phashBlock`/`pprev` walks vs `block_map_grow`

Status: **root-caused + verified by direct code reads (2026-06-03). Fix scoped, NOT
yet implemented** — it touches the consensus-critical chain core and must be a
deliberate, boot-validated pass.

Surfaced by a Wave-D boot smoke-test SIGSEGV (signal 11) in `push_getheaders_from`
on a wedged datadir copy. The crash is **pre-existing**, not the boot refactor.

## The bug (verified)

`struct block_index.phashBlock` (`lib/chain/include/chain/chain.h:81`) is a *pointer*
to the block's hash. On insert it is pointed **into the block_map bucket array**:

- `lib/validation/src/chainstate.c:605` — `bi->phashBlock = block_map_find_hash(&cs->map_block_index, hash)` returns `&m->buckets[slot].hash`.
- The node's own `struct uint256 hashBlock` field (`chain.h:86`) is **never populated** (grep: zero assignments) — so `phashBlock` is the only handle to the hash, and it lives in the bucket array.

`block_map_grow` (`lib/validation/src/chainstate.c:126-153`) **`free(old)`s the bucket
array** (line 143) and then repoints every node's `phashBlock` to the new buckets
(lines 147-150) — all under `m->rwlock`.

Multiple readers walk `pprev` and dereference `*walk->phashBlock` **with no lock**:

- `push_getheaders_from` exponential-locator branch — `lib/net/src/msg_headers.c:753-762` (the crash site, `*walk->phashBlock` at :755).
- `syncsvc_build_locator_from_index` — `app/services/src/header_sync_service.c` (the "safe" fallback; same `walk = walk->pprev` + `*walk->phashBlock`).
- `block_index_get_ancestor` — `lib/chain/src/chain.c:20` (`walk->pskip` / `walk->pprev`).

A concurrent insert (e.g. the reducer Jobs `header_admit_stage` / `reducer_ingest_service` /
`header_probe`, and `process_headers` itself via `accept_block_header → chainstate_insert_block_index`)
triggers `block_map_grow`, which `free()`s the bucket array out from under a lock-free
`*walk->phashBlock` read in another thread → **use-after-free → SIGSEGV**.

### Why it became HOT on this datadir (the §3 link)

The header-sync STALL loop (`lib/net/src/msg_headers.c:475`, "Block index heights may be
corrupted ... header tip=160 < chain tip=3132741") hammers `push_getheaders_from` and
re-accepts headers (growing the map) in a tight retry loop, maximizing the race window.
The in-memory `pprev` chain is stub-terminated near height 160 (partial rebuild —
`block_index_db.c` / `accept_block_header.c` create height-0 stubs for unseen `hashPrev`)
while `block_index_repair_heights` still sets `g_heights_repaired = true`
(`app/services/src/block_index_integrity.c:268`), which flips the gate
(`msg_processor_block_index_heights_repaired`) that **enables** the dangerous lock-free
exponential branch. Fixing that partial-index/gate issue is **§3, owner-gated**, and
stops the stall loop — but does NOT by itself remove the lock-free UAF.

## Why there is no quick patch

Falling back to the "safe" 2-hash locator does **not** help: `syncsvc_build_locator_from_index`
walks `pprev`/`phashBlock` lock-free identically. The whole locator + ancestor class is
unsafe. The fix must be class-level.

## Fix options

**A — node-owned hash (root re-architecture; preferred, fixes the whole class).**
Populate `bi->hashBlock = *hash` and point `bi->phashBlock = &bi->hashBlock` at every
insert; the bucket array keeps its own `.hash` key (unchanged). Then `phashBlock` points
to per-node, never-freed storage, so no walker can ever UAF, and `block_map_grow`'s
repoint loop (chainstate.c:147-150) becomes unnecessary. Value-equivalent for every
reader (`*phashBlock` is the same hash). Sites to change consistently:
`chainstate.c:605` (canonical insert), `app/services/src/chain_restore_executor.c:142`
(anchor repoint), the flat loader (`app/services/src/block_index_loader*.c`), and audit
any other `phashBlock =` / `block_map_find_hash`-into-`phashBlock` site. Verify nothing
relies on `phashBlock == &bucket.hash` identity (only value is used).

**B — lock the read window.** Take `map_block_index` rwlock (rdlock) around each
`pprev`/`phashBlock` walk, or snapshot hashes under the lock. Fixes the class without
changing the struct, but needs a public lock API + careful lock-ordering (deadlock risk)
and adds a shared-read lock to the hot sync path.

**Recommendation:** Option A — it is value-equivalent, removes the hazard structurally
(no lock discipline to get wrong), and shrinks `block_map_grow`. Implement as a focused
pass: change all sites → `make && test_parallel` (0/290) → boot smoke-test on a fresh
copy (the copy reproduces the crash, so a clean boot validates the fix) → commit.

## Validation recipe

`tools/repro_on_copy.sh uaf-fix --port=18299 --p2p-port=18933 --deadline=480 -- -nobgvalidation`
then grep the copy `repro_node.log` for `FATAL SIGNAL 11` / `push_getheaders_from` — the
fix is confirmed when the header-sync loop runs without the SIGSEGV.
