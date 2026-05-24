# Worker Assignments — Phase 4d: Mempool / Peers / Wallet / ZNAM / Store projections (batch spec)

> Each PR (4d-1..4d-5) follows the same shadow-emit + projection-consume
> pattern as 4b (utxo_projection) and 4c (block_index_projection).
> This doc lists the per-projection DELTAS so a worker can pick up any
> one with minimal duplication.

**Branch pattern:** PUSH DIRECT TO MAIN
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4d
**Depends on:** Phase 4a (event_log primitive) merged.
**Per-PR depends on:** none — each projection is independent.

For each projection PR below:
- Commit 1: add the event payload struct(s) to `event_log_payloads.h`
- Commit 2: add the projection skeleton (`.h` + `.c`) + schema + replay loop
- Commit 3: wire shadow emission alongside the existing direct write
- Commit 4: wire boot replay + diagnostics + MCP enum
- Commit 5: add diff MCP tool (`zcl_<name>_projection_diff`)
- Commit 6: tests + push

The deltas per projection are: which existing module owns the direct
writes today, which event types to introduce, what to project, and
any subsystem-specific subtleties.

---

## 4d-1 — mempool_projection

**Branch:** PUSH DIRECT TO MAIN
**Direct write owner today:** `app/models/src/mempool_model.c` (or wherever `mempool` and `mempool_spends` tables are written)

**Event types to add:**
- `EV_TX_ADMIT_MEMPOOL` (id 3 — already declared in event_log.h enum)
- `EV_TX_REMOVE_MEMPOOL` (id 4 — already declared)

**Event payloads** (add to `event_log_payloads.h`):
```c
struct ev_tx_admit_mempool {
    uint8_t  txid[32];
    int64_t  fee;
    uint32_t size_bytes;
    uint32_t weight;             /* virtual size, post-witness */
    uint32_t admitted_unix;
    uint8_t  priority_class;     /* 0..3 */
    uint8_t  reserved[3];
    /* Raw tx bytes follow (size_bytes) */
};

struct ev_tx_remove_mempool {
    uint8_t  txid[32];
    uint8_t  reason;             /* 1=mined, 2=replaced, 3=expired, 4=conflict */
    uint8_t  reserved[7];
};
```

**What the projection holds:**
- table `mempool` (txid, fee, size, weight, admitted_unix, priority, raw_tx)
- table `mempool_spends` (prevout_txid, prevout_vout, by_txid)

**Subtlety:** mempool is **in-memory only** in the dream end-state.
The 4d-1 PR keeps the SQLite projection on disk for diff-vs-live
verification during 24h soak. The cutover PR (separate) makes it
RAM-only and replays from the event log's last 10K events on boot.

**Diff MCP tool:** `zcl_mempool_projection_diff` — compare projection
to live mempool by (txid count, total fee, total weight) + first
differing txid if any.

---

## 4d-2 — peers_projection

**Branch:** PUSH DIRECT TO MAIN
**Direct write owner today:** `lib/net/src/peers_db.c` (or wherever
the `peers` + `addresses` tables are written)

**Event types to add:**
- `EV_PEER_OBSERVED` (id 7 — already declared)
- `EV_PEER_DROPPED` (id 8 — already declared)

**Event payloads:**
```c
struct ev_peer_observed {
    uint8_t  ip_v4_or_v6[16];    /* IPv6-mapped IPv4 if v4 */
    uint16_t port;
    uint8_t  is_onion;           /* 1 if the addr is .onion */
    uint8_t  reserved;
    uint64_t services_bitmap;
    uint32_t observed_unix;
    int32_t  height_hint;        /* -1 if unknown */
    /* .onion address (62 bytes incl. .onion suffix) follows if is_onion=1 */
};

struct ev_peer_dropped {
    uint8_t  ip_v4_or_v6[16];
    uint16_t port;
    uint8_t  reason;             /* 1=disconnect, 2=banned, 3=stale */
    uint8_t  reserved[5];
};
```

**What the projection holds:**
- table `peers` (key = ip|port; last_seen, services, height_hint, is_onion)
- table `addresses` (.onion + clearnet bundle from `directory.json` advertisements)

**Subtlety:** peers data is fast-changing. The projection should batch
writes (every 100 events or every 5s) to avoid SQLite churn.

**Diff MCP tool:** `zcl_peers_projection_diff` — compare projection
to `peers_db_iter()` results by row count + sample 10 random keys.

---

## 4d-3 — wallet_view_projection

**Branch:** PUSH DIRECT TO MAIN
**Direct write owner today:** `app/services/src/wallet_service.c` +
`app/models/src/wallet_model.c`

**Event types to add:**
- `EV_WALLET_KEY_ADD` (id 9 — already declared)
- `EV_WALLET_TX_SEEN` (id 10 — already declared)
- NEW `EV_WALLET_ADDR_DERIVED` — derived address for an existing key
- NEW `EV_WALLET_NOTE_DECRYPTED` — a shielded note was decrypted

**Event payloads:** see header (omitted for brevity in this batch
spec; the worker writes them following the same shape as 4d-1/4d-2).

**What the projection holds:**
- table `wallet_view` (read-only mirror of wallet_canary +
  wallet_transactions + wallet_utxos + wallet_sapling_notes)

**CRITICAL — what the projection does NOT hold:**
- `wallet_keys` (private keys)
- `wallet_sapling_keys` (private spending keys)
- `wallet_seed` (HD seed material)

**These stay in `~/.zclassic-c23/wallet_secret.dat`** — a separate
file, encrypted at rest, NEVER touched by the event log. Per the
Phase 4 doc: "secrets don't go into audit trails."

**Subtlety:** the wallet projection is read-only for the wallet RPC
handlers. The spend path still goes through the existing
wallet_service.c using `wallet_secret.dat`.

**Diff MCP tool:** `zcl_wallet_projection_diff` — compare balance,
tx count, note count, utxo count between projection and live wallet
service.

---

## 4d-4 — znam_projection

**Branch:** PUSH DIRECT TO MAIN
**Direct write owner today:** `app/services/src/znam_service.c` +
`app/models/src/znam_model.c`

**Event types to add (NEW):**
- `EV_ZNAM_REGISTER` — name registered
- `EV_ZNAM_UPDATE` — name updated (address records, text records)
- `EV_ZNAM_TRANSFER` — name transferred to new owner
- `EV_ZNAM_RENEW` — name renewed (lease extended)
- `EV_ZNAM_EXPIRE` — name expired (lease lapsed; emitted by the
  pruning Job, not by an external action)

Allocate these as IDs 12..16 in `enum event_type`.

**Event payloads:** standard pattern — owner pubkey, name, address
record dict, text record dict, expiry block.

**What the projection holds:**
- table `znam_names` (name → owner_pubkey, expiry_height, registered_at)
- table `znam_addr_records` (name + chain_id → address)
- table `znam_text_records` (name + key → value)

**Subtlety:** znam is consensus-controlled (registered via
OP_RETURN). The projection consumes events emitted by the block
admission path AFTER the OP_RETURN parser identifies a ZNAM op. The
event log is the canonical historical view of every name action.

**Diff MCP tool:** `zcl_znam_projection_diff`.

---

## 4d-5 — small projections (zmsg + zslp + zswp + store)

**Branch:** PUSH DIRECT TO MAIN

This is a BATCHED PR covering four small projections, because each is
~50 LOC and not worth a separate PR. Same shape as the others, just
collapsed.

**Subsystems:**

- **zmsg** (encrypted messages): `EV_ZMSG_SENT`, `EV_ZMSG_DELIVERED`.
  Tables: `zmsg_messages`.

- **zslp** (token protocol): `EV_ZSLP_TOKEN_CREATED`,
  `EV_ZSLP_TRANSFER`. Tables: `zslp_tokens`, `zslp_balances`,
  `zslp_transfers`.

- **zswp** (atomic swaps): `EV_ZSWP_CONTRACT_OBSERVED`,
  `EV_ZSWP_CONTRACT_RESOLVED`. Tables: `zswp_contracts`.

- **store** (e-commerce marketplace + file market): `EV_STORE_OFFER`,
  `EV_STORE_ORDER`, `EV_STORE_FULFILLMENT`. Tables: `products`,
  `orders`, `file_offers`, `file_services`.

**Allocate event IDs 17..27 for these.**

**Subtlety:** each subsystem's RPC layer continues to read its
existing SQLite table (which becomes the projection). The event log
is the audit trail.

**Diff MCP tool:** `zcl_<name>_projection_diff` per subsystem (4 tools).

---

## Per-PR acceptance gate (all of 4d-1..4d-5)

1. `make test_parallel` PASS.
2. New `zcl_<name>_projection_diff` MCP tool returns
   `match: true` on a freshly built node.
3. Live 24h with shadow mode: zero divergence events in
   `zcl_state subsystem=<name>_projection`.

If gate 3 fails: investigate the emitter or projection. The shadow
mode lets us catch issues without consensus risk.

---

## After 4d-1..4d-5 ship + soak: 4d-cutover PRs

For each projection, a separate one-line cutover PR disables the
direct SQLite write inside the corresponding service/model file.
After cutover:
- `app/models/src/mempool_model.c` write path → DELETED
- `lib/net/src/peers_db.c` write path → DELETED
- `app/models/src/wallet_model.c` view-table writes → DELETED
- `app/services/src/znam_service.c` direct writes → DELETED
- ZMSG / ZSLP / ZSWP / store model writes → DELETED

The event log is now the single write path for all of these
subsystems' derived state.

---

## After 4d full cutover: 4e — delete legacy block files

Per the plan doc § 4e, the final Phase 4 PR replaces
`blocks/blk*.dat` (6.3 GB of binary block bodies) with
`EV_BLOCK_BODY` events in the event log. This deletes:
- `lib/storage/src/blocks_mmap_reader.c`
- `lib/storage/src/blocks_index_legacy_reader.c`
- `lib/storage/src/disk_block_io.c`

Plus a one-time migration tool to import the existing `blocks/` into
the event log. The migration itself is replayable from the existing
LevelDB block_index pointers + the on-disk `blk*.dat` files.

Drafted as its own assignment when 4d cutover gets close.

---

## Status

**4d-1 DONE (wt2)** — completed 2026-05-24 for
`mempool_projection`.

**4d-2 DONE (wt2)** — completed 2026-05-24 for
`peers_projection`.

The other 4d projections remain available for independent workers.

Recommend dispatch order when 4a is done:
1. 4d-4 (znam — moderate; consensus-derived but well-scoped)
2. 4d-3 (wallet — most careful; secrets stay in wallet_secret.dat)
3. 4d-5 (small batch — last; mostly mechanical)

<!-- Each worker assignment for a specific projection appends a Completion section. -->

## Completion — 4d-1 mempool_projection (wt2, 2026-05-24)

Implemented:
- Added `EV_TX_ADMIT_MEMPOOL` / `EV_TX_REMOVE_MEMPOOL` payload helpers
  and a SQLite-backed `mempool_projection` replay primitive.
- Shadow-emits mempool admit/remove events from the legacy mempool model
  without blocking the existing direct-write path.
- Projection stores `mempool` plus `mempool_spends` derived from raw
  transaction bytes when parseable.
- Added `zcl_mempool_projection_diff` through diagnostics RPC + MCP.
- Diff now compares count, total fee, total weight, and sampled tx
  aggregates against the legacy table.
- Legacy `db_mempool_clear()` now shadow-emits remove events so reorg
  clears do not leave the projection stale.
- Added focused unit coverage and updated MCP surface-count coverage.

Verification:
- `make -j$(nproc)`
- `ZCL_TEST_ONLY=mempool_projection ./test_zcl`
- `./test_parallel` PASS:
  `ALL TESTS PASSED — 0/196 groups failed (109.0s wall, 32 workers)`

## Completion — 4d-2 peers_projection (wt2, 2026-05-24)

Commits:
- `db2cfa800` wt2: mark peers projection in progress
- `91aa65c1c` peers_projection: add shadow replay primitive
- `5dc442a81` peers_projection: wire shadow peer events
- `48e78d801` peers_projection: add diff MCP tool
- `58ab883a5` validation: avoid app service include
- `ecf5ea8b6` mcp_e2e: add diagnostics stale witness

Implemented:
- Added `EV_PEER_OBSERVED` / `EV_PEER_DROPPED` payload helpers and a
  SQLite-backed `peers_projection` replay primitive.
- Shadow-emits peer observe/drop events from legacy peer writes without
  blocking the existing direct-write path.
- Opens/catches up the projection during phase 4 storage-shadow boot and
  exposes diagnostics state.
- Added `zcl_peers_projection_diff` through diagnostics RPC + MCP.
- Added focused unit coverage and MCP stale-binary guard coverage for
  diagnostics controller surface changes.

Verification:
- `make -j$(nproc) test_zcl zclassic23`
- `ZCL_TEST_ONLY=peers_projection ./test_zcl`
- `ZCL_TEST_ONLY=mcp_controllers ./test_zcl`
- `ZCL_TEST_ONLY=mcp_e2e ./test_zcl`
- `make lint`
- `./test_parallel --jobs=$(nproc)` PASS on rerun:
  `ALL TESTS PASSED — 0/195 groups failed`

## Completion — 4d-4 znam_projection (2026-05-24)

Commits:
- `f52313f02` Phase 4d-4 Task 1: EV_ZNAM_* event payload schemas
- `60b33dbd1` Phase 4d-4 Task 2: znam_projection skeleton + catch_up replay
- `57804149c` Phase 4d-4 Task 3: shadow-emit ZNAM events from legacy DB writes
- `519aa2383` Phase 4d-4 Task 4: wire znam_projection boot replay + diagnostics
- `958d15839` Phase 4d-4 Task 5: zcl_znam_projection_diff MCP tool
- `4986a4684` Phase 4d-4 Task 5b: add znamprojectiondiff RPC handler + bump counts
- `ee1c5c7b1` (test_znam_projection — 30 cases across 5 events, picked up by
  the Phase 4b completion commit's omnibus add of the same files)

Implemented:
- Added five new event_log_type slots (12-16: EV_ZNAM_REGISTER /
  UPDATE / TRANSFER / RENEW / EXPIRE) with length-prefixed wire
  formats in `event_log_payloads.h`. Pure addition — every existing
  consumer untouched.
- SQLite-backed `znam_projection` primitive in `lib/storage` mirroring
  the `peers_projection` shape: open / close / catch_up / find /
  addr_get / text_get / count accessors, all using `INSERT OR REPLACE`
  for idempotent replay.
- Shadow-emits `EV_ZNAM_REGISTER` on every `db_znam_save` and
  `EV_ZNAM_UPDATE` (text + addr action types) on every
  `db_znam_text_save` / `db_znam_addr_save`. Legacy SQLite writes
  remain authoritative; emit failures log via `obs-ok` stderr.
- Opens the projection in the Phase 4 storage-shadow boot, runs
  catch_up at startup, attaches the global event log, and closes in
  shutdown ordering matching peers/utxo/block_index.
- Registered `znam_projection` in the diagnostics `g_dumpers` table so
  `zcl_state subsystem=znam_projection` returns counters via MCP. The
  enum_csv auto-derives — no further wiring required.
- Added `zcl_znam_projection_diff` MCP tool wrapping a new
  `znamprojectiondiff` RPC handler that compares the projection's
  name/addr/text row counts against the legacy `znam_names` /
  `znam_addr_records` / `znam_text_records` tables and returns a
  single `first_diff` string when they disagree.
- Added `test_znam_projection` (30 cases) covering payload roundtrip
  for all 5 events, open/close clean lifecycle, register replay,
  addr+text update record creation, transfer+renew+expire lifecycle
  with cascade delete, and projection state persistence across
  event-log reopen. Wired into test.c (run + ZCL_TEST_ONLY block),
  test_helpers.h, and test_parallel.c.

Verification:
- `make -j$(nproc)` clean.
- `make lint` PASS (WARN-mode raw-sqlite-in-controllers count
  unchanged from baseline aside from the new diff handler's 3 read-only
  COUNT(*) calls).
- `ZCL_TEST_ONLY=znam_projection ./test_zcl` → 0 failures (30/30 cases).
- `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` → 0 failures with new
  EXPECTED_TOTAL=98 / EXPECTED_OPS=37.
- `./test_parallel --jobs=$(nproc)` → ALL TESTS PASSED — 0/197 groups
  failed (106s wall, 32 workers).
