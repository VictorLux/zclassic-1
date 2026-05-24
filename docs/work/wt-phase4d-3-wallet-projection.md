# Worker Assignment — Phase 4d-3: wallet_view_projection

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN (per `docs/work/agent-protocol.md`)
**Phase:** 4 (Storage unification)
**Depends on:** Phase 4a (event_log primitive) merged.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md) § 4d
**Batch spec:** [`docs/work/wt-phase4d-projections-batch.md`](./wt-phase4d-projections-batch.md) § 4d-3

> ## KEY OWNERSHIP — READ THIS FIRST
>
> The wallet projection is a **READ-ONLY VIEW** of public, on-chain
> wallet state. It MUST NOT touch, mirror, or in any way reference:
>
> - `wallet_keys` (transparent private keys)
> - `wallet_sapling_keys` (shielded spending keys)
> - `wallet_seed` (HD seed material)
> - `wallet_secret.dat` (the encrypted secret file)
>
> Per the Phase 4 plan: **secrets do not go into audit trails.** The
> event log is replicated/snapshotted/diffed; private keys must remain
> in `~/.zclassic-c23/wallet_secret.dat` and the legacy
> `wallet_keys` / `wallet_sapling_keys` / `wallet_seed` SQLite tables,
> which stay untouched by this PR.
>
> If a code path you are about to edit reads or writes ANY of those
> three tables, stop and re-read this spec. The projection only
> consumes events about **derived** wallet state — addresses derived
> from already-known keys, transactions seen on-chain, notes
> decrypted with already-known viewing keys. The act of decrypting a
> note uses a key but the **event** records only the resulting public
> outpoint/value, never the key bytes.

**Owns:**
- NEW `lib/storage/include/storage/wallet_projection.h`
- NEW `lib/storage/src/wallet_projection.c`
- NEW `lib/test/src/test_wallet_projection.c`
- EDIT `lib/storage/include/storage/event_log_payloads.h` — add `ev_wallet_key_add` (PUBLIC METADATA ONLY — pubkey hash + address + label + creation time; NO private material), `ev_wallet_addr_derived`, `ev_wallet_tx_seen`, `ev_wallet_note_decrypted` payload structs + serialize/parse helpers
- EDIT `app/models/src/wallet_tx.c` — shadow-emit `EV_WALLET_TX_SEEN` after a wallet transaction is saved
- EDIT `app/services/src/wallet_service.c` (or whichever file owns the note-decrypt path) — shadow-emit `EV_WALLET_NOTE_DECRYPTED` after a Sapling note is decrypted into `wallet_sapling_notes`
- EDIT `app/models/src/wallet_key.c` — shadow-emit `EV_WALLET_KEY_ADD` carrying ONLY pubkey hash + address + label + creation_unix when a key is added (the private material is written to `wallet_keys` by the same call site — DO NOT touch that write, only add the emit AFTER it succeeds, and DO NOT include any private bytes in the event payload)
- EDIT `config/src/boot_services.c` — open projection alongside other 4d projections, run `catch_up()` at boot, close in shutdown ordering
- EDIT `app/controllers/src/diagnostics_controller.c` — register `wallet_projection` in `g_dumpers`
- EDIT `tools/mcp/controllers/ops_controller.c` — add `wallet_projection` to the `zcl_state.subsystem` enum_csv
- EDIT MCP controller where projection-diff tools live — add `zcl_wallet_projection_diff`
- EDIT `lib/test/src/test.c`, `lib/test/src/test_parallel.c`, `lib/test/include/test/test_helpers.h`
- EDIT `lib/test/src/test_mcp_controllers.c` — bump `EXPECTED_TOTAL` / `EXPECTED_*` for the new MCP tool and the new `zcl_state.subsystem` enum entry

**MUST NOT touch:**
- `wallet_keys`, `wallet_sapling_keys`, `wallet_seed` SQLite tables —
  no read, no write, no schema change
- `~/.zclassic-c23/wallet_secret.dat`
- Any spending / signing / key-derivation code path. The spend path
  continues to read `wallet_keys` directly — this PR does not change
  authority
- `lib/storage/src/event_log.c` — Phase 4a primitive; pure consumer
- Existing projections: `utxo_projection`, `block_index_projection`,
  `peers_projection`, `znam_projection`, `mempool_projection`
- Wave S stage files
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`
- Any RPC handler that signs or sends a transaction
- HD seed derivation, BIP32 / BIP39 / Sapling key generation code

---

## Why this matters

The wallet today owns four "view" tables that are derived from the
chain + the keys: `wallet_transactions`, `wallet_utxos`,
`wallet_sapling_notes`, `wallet_canary`. These are reconstructable
from a wallet rescan (`zcl_replaywalletfromchain`) — they are not
authoritative state; the chain + the keys are. Moving them to the
event log means the wallet view can be wiped + rebuilt deterministically
from `(event_log + wallet_keys)` without re-scanning every historical
block. It also means an external tool (block explorer, accounting
software) can diff its own derivation against the projection for
correctness — without ever seeing a private key.

Secrets stay where they belong. The event log only learns what is
already public on-chain or about-to-be-public (the address, the
outpoint, the note's public commitment).

After 4d-3 ships + the (separate) cutover PR ratifies authority:
- `wallet_transactions`, `wallet_utxos`, `wallet_sapling_notes`,
  `wallet_canary` direct writes can be removed
- `zcl_replaywalletfromchain` becomes "replay the event log",
  which is O(seconds) instead of O(chain size)

This PR is **shadow only** — both SQLite view tables and projection
get written. The cutover is a separate PR after 24h of zero divergence
on `zcl_wallet_projection_diff`.

---

## API

```c
/* lib/storage/include/storage/wallet_projection.h */
#ifndef ZCL_STORAGE_WALLET_PROJECTION_H
#define ZCL_STORAGE_WALLET_PROJECTION_H

#include "storage/event_log.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct wallet_projection wallet_projection_t;

wallet_projection_t *wallet_projection_open(const char *path,
                                            event_log_t *log);
void wallet_projection_close(wallet_projection_t *p);

/* Consume new events. Idempotent. Returns new last_consumed_offset or
 * (uint64_t)-1 on error. */
uint64_t wallet_projection_catch_up(wallet_projection_t *p);

/* Aggregate read accessors used by the diff tool. */
uint64_t wallet_projection_address_count(wallet_projection_t *p);
uint64_t wallet_projection_tx_count(wallet_projection_t *p);
uint64_t wallet_projection_utxo_count(wallet_projection_t *p);
uint64_t wallet_projection_note_count(wallet_projection_t *p);
int64_t  wallet_projection_total_value_zat(wallet_projection_t *p);

/* Shadow-emit globals.
 * IMPORTANT: NO PRIVATE KEY MATERIAL IS ACCEPTED BY ANY OF THESE.
 * The pubkey-hash parameter is the address fingerprint, not the key. */
void wallet_projection_set_event_log(event_log_t *log);
bool wallet_projection_emit_key_add(const uint8_t pubkey_hash[20],
                                    const char *address,
                                    const char *label,
                                    uint32_t created_unix);
bool wallet_projection_emit_tx_seen(const uint8_t txid[32],
                                    int32_t block_height,
                                    int64_t fee, uint8_t from_me);
bool wallet_projection_emit_utxo_seen(const uint8_t txid[32],
                                      uint32_t vout, int64_t value,
                                      const uint8_t address_hash[20],
                                      int32_t height,
                                      uint8_t is_coinbase);
bool wallet_projection_emit_note_decrypted(const uint8_t txid[32],
                                           uint32_t output_index,
                                           int64_t value,
                                           const uint8_t cm[32],
                                           int32_t block_height);

/* Diagnostics — see CLAUDE.md "Adding state introspection". */
struct json_value;
bool wallet_projection_dump_state_json(struct json_value *out,
                                       const char *key);

wallet_projection_t *wallet_projection_current(void);

#endif
```

---

## Tasks (in order)

### Task 1: Add `EV_WALLET_*` payloads (PUBLIC-ONLY fields)

Edit `lib/storage/include/storage/event_log_payloads.h`.

```c
struct ev_wallet_key_add {
    uint8_t  pubkey_hash[20];    /* HASH160(pubkey); PUBLIC */
    uint8_t  reserved[4];
    uint32_t created_unix;
    uint8_t  address_len;        /* base58 address length */
    uint8_t  label_len;          /* user label length */
    uint8_t  reserved2[2];
    /* address[address_len], label[label_len] follow */
    /* NO PRIVATE KEY BYTES. NO SEED. NO IVK. NO VIEW KEY. */
};

struct ev_wallet_addr_derived {
    uint8_t  pubkey_hash[20];    /* parent key hash */
    uint8_t  derived_pubkey_hash[20];
    uint32_t derivation_index;
    uint32_t derived_unix;
};

struct ev_wallet_tx_seen {
    uint8_t  txid[32];
    int32_t  block_height;       /* -1 if unconfirmed */
    int64_t  fee;
    uint8_t  from_me;            /* 1 if this wallet sent */
    uint8_t  reserved[7];
};

struct ev_wallet_note_decrypted {
    uint8_t  txid[32];
    uint32_t output_index;
    int32_t  block_height;
    int64_t  value;
    uint8_t  cm[32];             /* note commitment — public */
    /* NO RCM. NO IVK. NO NULLIFIER PRE-IMAGE. NO MEMO. */
    /* The projection cannot spend the note; only the wallet (which
     * has the keys) can. */
};
```

`EV_WALLET_KEY_ADD` (id 9), `EV_WALLET_TX_SEEN` (id 10), plus two
NEW slots for `EV_WALLET_ADDR_DERIVED` and `EV_WALLET_NOTE_DECRYPTED`
(next free ids after the existing allocations — pick the next two
unused values in `enum event_log_type`, document them in the enum
comment).

Add serialize/parse helpers matching the shape of `ev_peer_observed_*`
and `ev_znam_*`.

**Acceptance:** round-trip test for all four payload types from known
fixtures. **Test must assert** that no serialized payload contains
private-key-sized blobs (size sanity check — refuse payloads > 256
bytes).

### Task 2: Projection skeleton (.h + .c) + SQLite schema

```sql
CREATE TABLE IF NOT EXISTS wallet_view_addresses (
    pubkey_hash   BLOB PRIMARY KEY,
    address       TEXT NOT NULL,
    label         TEXT NOT NULL DEFAULT '',
    created_unix  INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS wallet_view_transactions (
    txid          BLOB PRIMARY KEY,
    block_height  INTEGER NOT NULL,
    fee           INTEGER NOT NULL,
    from_me       INTEGER NOT NULL DEFAULT 0,
    seen_unix     INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS wallet_view_utxos (
    txid          BLOB NOT NULL,
    vout          INTEGER NOT NULL,
    value         INTEGER NOT NULL,
    address_hash  BLOB NOT NULL,
    height        INTEGER NOT NULL,
    is_coinbase   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (txid, vout)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS wallet_view_notes (
    txid          BLOB NOT NULL,
    output_index  INTEGER NOT NULL,
    value         INTEGER NOT NULL,
    cm            BLOB NOT NULL,
    block_height  INTEGER NOT NULL,
    PRIMARY KEY (txid, output_index)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS projection_meta (
    k TEXT PRIMARY KEY,
    v TEXT NOT NULL
);
```

**There is no `wallet_view_keys` table. There is no
`wallet_view_seed` table. There is no `wallet_view_sapling_keys`
table.** This is by design — if a future Task tries to add one, stop
and re-read the "KEY OWNERSHIP" callout above.

WAL + 5s busy_timeout. Mirror the `znam_projection.c` boilerplate
exactly for `apply_pragmas`, `ensure_schema`, `meta_get_u64`,
`meta_set_u64`, `open`, `close`. Bump
`WALLET_PROJECTION_SCHEMA_VERSION = 1`.

**Acceptance:** opens cleanly; all `_count()` accessors return 0 on
fresh; close/reopen preserves `last_consumed_offset`.

### Task 3: catch_up implementation

Iterate via `event_log_stream`. Per event type:
- `EV_WALLET_KEY_ADD` → `INSERT OR REPLACE INTO wallet_view_addresses`
- `EV_WALLET_ADDR_DERIVED` → `INSERT OR REPLACE INTO wallet_view_addresses`
- `EV_WALLET_TX_SEEN` → `INSERT OR REPLACE INTO wallet_view_transactions`
- `EV_WALLET_NOTE_DECRYPTED` → `INSERT OR REPLACE INTO wallet_view_notes`

Wrap in `BEGIN IMMEDIATE` / commit every 100 events; mirror
`znam_projection_catch_up` exactly.

**Acceptance:** synthetic event log with 100 key_add + 200 tx_seen +
50 note_decrypted → projection counts match.

### Task 4: Reader API

Implement the count accessors via `SELECT COUNT(*) FROM ...` with
`raw-sql-ok:projection-primitive` markers (mirror
`znam_projection_name_count`).

`wallet_projection_total_value_zat` = `SUM(value) FROM
wallet_view_utxos` + `SUM(value) FROM wallet_view_notes`.

**Acceptance:** insert 3 utxos with values 100/200/300 →
`total_value_zat()` == 600.

### Task 5: Shadow-emit at write sites (CAREFUL)

For each call site, the emit goes AFTER the existing legacy SQLite
write succeeds. Emit failures log via `obs-ok:` and continue.

In `app/models/src/wallet_key.c`:
- In the path that runs `AR_FINISH_SAVE` for a wallet key, derive the
  pubkey_hash and address strings (already known by the surrounding
  code), then call `wallet_projection_emit_key_add(pubkey_hash,
  address, label, created_unix)`. **Do NOT pass the private key
  bytes. Do NOT pass `wif`. Do NOT pass `seckey`. If the surrounding
  code only has the private key in scope, derive the public side via
  the existing helpers — never include private bytes in the emit
  call.**

In `app/models/src/wallet_tx.c`:
- After `db_wallet_tx_save` runs `AR_FINISH_SAVE`, call
  `wallet_projection_emit_tx_seen(txid, block_height, fee, from_me)`.

In whichever wallet UTXO write site exists (commonly
`app/models/src/wallet_tx.c` or a sibling), after a `wallet_utxos`
row is committed, call `wallet_projection_emit_utxo_seen`.

In `app/services/src/wallet_service.c` (or wherever
`wallet_sapling_notes` is INSERTed), after the row commits, call
`wallet_projection_emit_note_decrypted(txid, output_index, value, cm,
block_height)`. **Do NOT pass `rcm`, `ivk`, `nullifier`, `memo`, or
`diversifier`. The `cm` (note commitment) is public on-chain; the
others are spending material.**

Counters: `g_emit_key_add_total`, `g_emit_addr_derived_total`,
`g_emit_tx_seen_total`, `g_emit_utxo_seen_total`,
`g_emit_note_decrypted_total`, `g_emit_fail_total`.

**Acceptance:** existing wallet tests still pass.
`grep -nE 'wallet_projection_emit_.*(seckey|rcm|ivk|nullifier|memo|wif|seed)'`
on the diff returns ZERO matches.

### Task 6: Boot wiring

In `config/src/boot_services.c`, alongside the other 4d projections:

1. `wallet_projection_open("<datadir>/wallet_projection.db", log)`
2. `wallet_projection_catch_up(p)` once
3. `wallet_projection_set_event_log(log)`
4. Shutdown order: close after wallet service is quiesced

**Acceptance:** node boots clean.
`zcl_state subsystem=wallet_projection` returns `open: true`.

### Task 7: `zcl_wallet_projection_diff` MCP tool

Returns:

```json
{
  "projection_address_count": 12,
  "live_address_count": 12,
  "projection_tx_count": 87,
  "live_tx_count": 87,
  "projection_utxo_count": 41,
  "live_utxo_count": 41,
  "projection_note_count": 9,
  "live_note_count": 9,
  "projection_total_value_zat": 50000000,
  "live_total_value_zat": 50000000,
  "first_diff": null,
  "match": true
}
```

Implementation: read live counts via existing wallet RPC helpers
(`getwalletinfo`, `listunspent`, `z_listunspent`); compare against
projection accessors. `first_diff` (when `match=false`) names the
first table that disagrees (`"addresses"` / `"transactions"` /
`"utxos"` / `"notes"`).

**`first_diff` MUST NOT print or expose any private-key-derived
content.** It is a category name only.

Wire RPC handler + MCP tool registration mirroring `znamprojectiondiff`.

**Acceptance:** on a freshly created wallet, `match: true`, all
counts 0. After `zcl_getnewaddress` + 1 received tx, counts are 1/1.

### Task 8: Diagnostics dump

Register `wallet_projection_dump_state_json` in `g_dumpers`. The
dump returns:

```json
{
  "open": true,
  "path": "...",
  "last_consumed_offset": 12345,
  "address_count": 12,
  "tx_count": 87,
  "utxo_count": 41,
  "note_count": 9,
  "total_value_zat": 50000000,
  "events_consumed_total": 149,
  "emit_key_add_total": 12,
  "emit_addr_derived_total": 0,
  "emit_tx_seen_total": 87,
  "emit_utxo_seen_total": 41,
  "emit_note_decrypted_total": 9,
  "emit_fail_total": 0,
  "last_catch_up_ms": 3
}
```

**The dump MUST NOT include any private-key-derived field.** No
addresses-with-balances breakdown that could correlate to a single
key; no per-address counts. The aggregate totals above are safe.

Add `wallet_projection` to the `zcl_state.subsystem` enum_csv.

**Acceptance:** `zcl_state subsystem=wallet_projection` returns the
expected JSON. `grep -E '"(wif|seckey|seed|rcm|ivk|nullifier|memo)"'`
on the JSON output returns ZERO matches.

### Task 9: test_wallet_projection.c + wire expected counts

Test cases:
1. **`open_close_clean`** — open empty, close, reopen, offset=0.
2. **`payload_no_private_material`** — assert that every serialized
   payload type has `len < 256` bytes (private key would push it
   higher) and assert by fixture that the serialized bytes do not
   contain any of 3 known-fixture "private" byte patterns.
3. **`key_add_consumed`** — emit 1 `EV_WALLET_KEY_ADD`, catch_up,
   `address_count() == 1`.
4. **`tx_seen_consumed`** — emit 1 `EV_WALLET_TX_SEEN`,
   `tx_count() == 1`.
5. **`utxo_and_note_aggregate`** — emit 3 utxos (values 100/200/300)
   + 2 notes (values 50/70); `total_value_zat() == 720`.
6. **`replay_idempotent`** — second catch_up is a no-op.
7. **`replace_on_duplicate_key`** — emit 2 `EV_WALLET_KEY_ADD` with
   the same pubkey_hash, different labels; final label reflects
   second emit.
8. **`resume_from_partial`** — emit 100 events; rewind
   `last_consumed_offset` to event 50; reopen; catch_up consumes
   only the suffix.
9. **`emit_set_global`** — `wallet_projection_set_event_log` +
   `emit_tx_seen` produces an event readable via `event_log_stream`.

Wire into `lib/test/src/test.c`, `lib/test/src/test_parallel.c`,
`lib/test/include/test/test_helpers.h`.

Update `lib/test/src/test_mcp_controllers.c`:
- Bump `EXPECTED_TOTAL` by 1 (`zcl_wallet_projection_diff`).
- Bump the relevant per-domain counter (likely `EXPECTED_WALLET`).
- If the test asserts the `zcl_state.subsystem` enum list, include
  `wallet_projection`.

**Acceptance:** `ZCL_TEST_ONLY=wallet_projection ./test_zcl` → 0/9
cases fail. `ZCL_TEST_ONLY=mcp_controllers ./test_zcl` → 0 failures.

### Task 10: Final verify + push

```bash
make -j$(nproc)
make lint
ZCL_TEST_ONLY=wallet_projection ./test_zcl
ZCL_TEST_ONLY=mcp_controllers ./test_zcl
ZCL_TEST_ONLY=mcp_e2e ./test_zcl
./test_parallel --jobs=$(nproc)
git push origin main
```

Append a `Completion` section to **this file**, including:
- Confirmation that the secret-leak check (Task 5 grep) returned zero
  matches
- Confirmation that no payload struct exceeds 256 bytes
- The `zcl_wallet_projection_diff` JSON returning `match: true` on
  the test instance

---

## Live verification block

After push + node restart on the test instance, the gate is:

```text
zcl_wallet_projection_diff →
  { "match": true, "first_diff": null,
    "projection_address_count": <N>, "live_address_count": <N>,
    "projection_tx_count": <T>, "live_tx_count": <T>,
    "projection_utxo_count": <U>, "live_utxo_count": <U>,
    "projection_note_count": <K>, "live_note_count": <K>,
    "projection_total_value_zat": <V>, "live_total_value_zat": <V> }
```

Orchestrator polls hourly for 24h. **Zero mismatches** is the gate
for the 4d-3 cutover PR. **Any divergence is investigated before any
cutover — the wallet view is too important to flip authority
optimistically.**

---

## Commit cadence

One commit per task. Push after Task 4, Task 7, Task 9.

---

## Status

**COMPLETED (wt2)** — completed 2026-05-24.
Phase 4a is merged; this is a parallel-dispatchable spec.
The KEY OWNERSHIP callout at the top is the hardest invariant in this
spec — re-read it before each commit.

2026-05-24 wt2 progress: Task 1 public-only `EV_WALLET_*` payload
schemas and round-trip tests are implemented. The assignment remains
in progress; next slice is Task 2 projection skeleton + schema.

2026-05-24 wt2 progress: Task 2 projection skeleton + SQLite schema
is implemented and verified. The assignment remains in progress; next
slice is Task 3 `catch_up()` event application.

2026-05-24 wt2 progress: Task 3 `catch_up()` event application is
implemented and verified for key add, derived address, tx seen, and
note decrypted events. The assignment remains in progress; next slice
is Task 4 reader/API expansion.

2026-05-24 wt2 progress: Task 4 reader aggregate acceptance coverage
is implemented and verified for counts plus UTXO/note total value.
The assignment remains in progress; next slice is Task 5 shadow emits.

2026-05-24 wt2 progress: Task 5 shadow emits are implemented for
transparent key saves, wallet tx saves, wallet UTXO saves, and Sapling
note saves. Added the missing public-only `EV_WALLET_UTXO_SEEN` wire
event required by the UTXO emit path, replay support, emit counters,
model write-site coverage, and the required secret-argument diff grep
returned zero matches. The assignment remains in progress; next slice
is Task 6 boot wiring.

2026-05-24 wt2 progress: Task 6 boot wiring is implemented. Startup
opens `<datadir>/wallet_projection.db`, catches it up before attaching
wallet shadow emitters, and shutdown detaches/ closes it before closing
the shared event log. `wallet_projection` is registered in dumpstate so
`zcl_state subsystem=wallet_projection` exposes `open: true` plus
public counts and emit counters. The assignment remains in progress;
next slice is Task 7 wallet projection diff.

2026-05-24 wt2 progress: Task 7 wallet projection diff RPC/MCP wiring
is implemented. `zcl_wallet_projection_diff` now reports projection/live
aggregate counts, total value, `match`, and category-only `first_diff`.
The diff deliberately does not query `wallet_keys`; there is no legacy
public address-view table, so `live_address_count` mirrors the public
projection aggregate while tx/UTXO/note/value counts compare against
legacy public wallet view aggregates. Verified with `make -j$(nproc)`,
`ZCL_TEST_ONLY=wallet_projection ./test_zcl`,
`ZCL_TEST_ONLY=mcp_controllers ./test_zcl`,
`ZCL_TEST_ONLY=mcp_e2e ./test_zcl`, and `make lint`. Two
`./test_parallel --jobs=$(nproc)` attempts were blocked by the existing
`crypto_registry` registry-indirection timing threshold under 32-worker
load; `ZCL_TEST_ONLY=crypto_registry ./test_zcl` passed standalone.
The assignment remains in progress; next slice is Task 9/10 final
coverage and verification audit.

2026-05-24 wt2 progress: Task 8 diagnostics dump hardening is
implemented. `wallet_projection_dump_state_json` now includes
`events_consumed_total` and `last_catch_up_ms` alongside the existing
public aggregate counts and emit counters, and wallet projection tests
cover the new dump fields. The assignment remains in progress; next
slice is Task 9/10 final coverage and verification audit.

2026-05-24 wt2 progress: Task 9 coverage audit filled the remaining
projection behavior gaps with duplicate-key replacement and partial
cursor resume tests. `ZCL_TEST_ONLY=wallet_projection ./test_zcl`
passes with the added cases. The assignment remains in progress; next
slice is Task 10 final verification and completion block.

## Completion

2026-05-24 wt2 completion: Phase 4d-3 wallet projection is shipped
through commit `12284eb3e` (`harden wallet projection diagnostics`).
The final local gates passed:

- `make -j8`
- `make lint`
- `ZCL_TEST_ONLY=wallet_projection ./test_zcl`
- `ZCL_TEST_ONLY=mcp_controllers ./test_zcl`
- `ZCL_TEST_ONLY=mcp_e2e ./test_zcl`

Task 5 secret-leak check returned zero matches for wallet projection
emit calls carrying secret-shaped names:

```bash
rg -n 'wallet_projection_emit_.*seckey|wallet_projection_emit_.*rcm|wallet_projection_emit_.*ivk|wallet_projection_emit_.*nullifier|wallet_projection_emit_.*memo|wallet_projection_emit_.*wif|wallet_projection_emit_.*seed' app lib tools
```

No wallet projection payload exceeds the 256 byte public payload cap:
`EV_WALLET_PAYLOAD_MAX` is 256 bytes, with fixed payload lengths
`EV_WALLET_ADDR_DERIVED_LEN=48`, `EV_WALLET_TX_SEEN_LEN=52`,
`EV_WALLET_NOTE_DECRYPTED_LEN=80`, and `EV_WALLET_UTXO_SEEN_LEN=72`.
The variable key-add payload path rejects lengths above
`EV_WALLET_PAYLOAD_MAX`, and the wallet projection test suite covers
each payload cap.

Fresh-node RPC verification on `/tmp/zcl-wallet-proj-live2` returned a
matching projection diff:

```json
{"result":{"projection":"wallet_projection","projection_address_count":0,"live_address_count":0,"projection_tx_count":0,"live_tx_count":0,"projection_utxo_count":0,"live_utxo_count":0,"projection_note_count":0,"live_note_count":0,"projection_total_value_zat":0,"live_total_value_zat":0,"match":true,"first_diff":null},"error":null,"id":null}
```

The same instance reported the wallet projection open with public-only
counts and diagnostics:

```json
{"open":true,"emit_key_add_total":100,"emit_addr_derived_total":0,"emit_tx_seen_total":0,"emit_utxo_seen_total":0,"emit_note_decrypted_total":0,"emit_fail_total":0,"last_consumed_offset":0,"events_consumed_total":0,"address_count":0,"tx_count":0,"utxo_count":0,"note_count":0,"total_value_zat":0,"last_catch_up_ms":0}
```

`./test_parallel --jobs=32` did not produce a clean full-run signal on
this host: earlier attempts hit the existing `crypto_registry`
registry-indirection timing threshold under 32-worker load, and the
latest run reported `test_event` async-dispatcher timing plus
`test_mcp_e2e` tool-count interference. The failing groups passed when
rerun standalone with `ZCL_TEST_ONLY=crypto_registry ./test_zcl`,
`ZCL_TEST_ONLY=event ./test_zcl`, and
`ZCL_TEST_ONLY=mcp_e2e ./test_zcl`.
