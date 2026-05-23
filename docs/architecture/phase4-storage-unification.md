# Phase 4 — Storage unification (the generational win)

**Status:** PLAN (draft 2026-05-23)
**Phase:** 4 (after Phase 2 completes and Phase 3 mega-modules dissolve)
**Estimated scope:** ~6,000 LOC out, ~2,000 LOC in. **Net ~4,000 LOC deletion.**

> "Phase 4 is the change that makes everything else cheap."
> — FRAMEWORK.md §8

---

## The problem today

The current node has **5+ independent storage layers**, each with its own
format, lifecycle, recovery story, and consistency invariants. Live data
directory (`~/.zclassic-c23/`) inspection:

| Layer | Size (live, 2026-05-23) | Format | What it stores | Recovery story |
|---|---|---|---|---|
| `blocks/blk*.dat` | **6.3 GB** | Bitcoin Core legacy binary | Block bodies, append-only | mmap + per-file rebuild on corruption |
| `node.db` | **961 MB** | SQLite (WAL) | tx_index, mempool, peers, wallet, store, znam, msg | per-table validators + AR_* lifecycle |
| `progress.kv` | **556 MB** | SQLite (WAL) | Wave S stage cursors + per-stage logs | shadow stage rebuild |
| `block_index.bin` | **513 MB** | LevelDB | block header index, file-position mapping | rebuild from `blocks/` on mismatch |
| `consensus_snapshot.db` | **446 MB** | SQLite | UTXO snapshot for FlyClient fast-sync | rebuild from chain replay |
| `mmb_leaves.bin` | **96 MB** | binary | Merkle Mountain Belt leaves | rebuild from headers |
| `peers.dat` | **216 KB** | binary addrman | DNS+manual peer info | discard on parse error |
| `blocks.shadow/` | **744 KB** | binary | shadow feeder data | discard |

**8 distinct storage formats. 8 distinct recovery paths. 8 distinct
crash-safety invariants.** Every change to consensus rules requires
updating 3-5 of them in lockstep. The kill-9 ordering invariant (coins.db
must commit BEFORE LevelDB block_index fsync) exists because we have
multiple atomic-write engines that don't share a transaction.

The "at-tip kill-9 ordering invariant" memory captures one such bug —
solved by careful ordering inside a single binary, but the underlying
problem is **there is no single source of truth**.

---

## The destination

**One append-only event log. N projections built from it.**

```
                  ┌─────────────────────────────┐
                  │ event_log.dat (append-only) │
                  │                             │
                  │  evt_n+0  BLOCK_HEADER     │
                  │  evt_n+1  BLOCK_BODY       │
                  │  evt_n+2  TX_ADMIT         │
                  │  evt_n+3  UTXO_DELTA       │
                  │  evt_n+4  PEER_SEEN        │
                  │  evt_n+5  WALLET_KEY_ADD   │
                  │  ...                        │
                  │  (offset, type, payload)   │
                  └──────────────┬──────────────┘
                                 │
                                 │ one writer thread per process
                                 │ many reader projections
                                 │
                  ┌──────────────┴──────────────┐
                  │                             │
        ┌─────────▼────────┐           ┌────────▼─────────┐
        │  utxo_projection │           │  peers_projection│
        │  (SQLite or LMDB)│           │  (SQLite or LMDB)│
        │                  │           │                  │
        │  rebuilds from   │           │  rebuilds from   │
        │  event_log on    │           │  event_log on    │
        │  any corruption  │           │  any corruption  │
        └──────────────────┘           └──────────────────┘
                  │                             │
        ┌─────────▼────────┐           ┌────────▼─────────┐
        │  block_index_*   │           │  wallet_keys_*   │
        │  projection      │           │  projection      │
        └──────────────────┘           └──────────────────┘
                  │
        ┌─────────▼────────┐
        │  mempool_*       │
        │  projection      │  (in-memory rebuild on boot)
        └──────────────────┘
```

### Properties

1. **One source of truth** — the event log. Everything else is computed.
2. **Atomic events** — each event is fsync-flushed before the writer
   acknowledges. No torn writes possible.
3. **Idempotent replay** — every projection can rebuild itself from the
   event log. Corruption is recovered by deleting the projection and
   replaying.
4. **No cross-engine consistency invariants** — there's only one engine
   that needs to be crash-safe. Projections are derived state and can
   be wiped.
5. **Audit-friendly** — append-only log is a complete history. SHA3
   commitment over the log gives FlyClient-style fingerprinting for
   the entire node state, not just consensus.
6. **Composable readers** — projections are independent. A new
   subsystem (e.g., a future shielded-name index) is a new projection
   file, not a coordinated change across 5 storage layers.

---

## Phase 4 sub-phases

### 4a: Build the event log primitive

NEW: `lib/storage/include/storage/event_log.h` + `event_log.c`.

```c
typedef struct event_log event_log_t;

enum event_type {
    EV_BLOCK_HEADER       = 1,
    EV_BLOCK_BODY         = 2,
    EV_TX_ADMIT_MEMPOOL   = 3,
    EV_TX_REMOVE_MEMPOOL  = 4,
    EV_UTXO_ADD           = 5,
    EV_UTXO_SPEND         = 6,
    EV_PEER_OBSERVED      = 7,
    EV_PEER_DROPPED       = 8,
    EV_WALLET_KEY_ADD     = 9,
    EV_WALLET_TX_SEEN     = 10,
    /* expand cautiously — every entry is a permanent surface */
};

event_log_t *event_log_open(const char *path);
void         event_log_close(event_log_t *log);

/* Append; fsyncs before returning. Returns offset of the new event,
 * or (uint64_t)-1 on failure. */
uint64_t event_log_append(event_log_t *log,
                          enum event_type type,
                          const void *payload, size_t payload_len);

/* Read at offset. Caller-owned buffer; returns 0 on success. */
int event_log_read(event_log_t *log, uint64_t offset,
                   enum event_type *type_out,
                   void *buf, size_t buf_cap, size_t *out_len);

/* Stream all events from start_offset to end (or until log end).
 * Callback returns false to stop. */
typedef bool (*event_log_cb)(uint64_t offset, enum event_type type,
                              const void *payload, size_t len,
                              void *user);
int event_log_stream(event_log_t *log, uint64_t start_offset,
                     event_log_cb cb, void *user);

/* SHA3-256 over the entire log up to current end. Used for the
 * "node state fingerprint" tool. */
int event_log_fingerprint(event_log_t *log, uint8_t out[32]);
```

Wire format per event (on disk):
```
[ 4B  payload_length    ]
[ 4B  event_type        ]
[ 4B  flags (reserved)  ]
[ 4B  payload_crc32c    ]
[ NB  payload           ]
[ 16B fsync_sentinel    ]   <- written AFTER fsync(payload), used by replay
                              to detect a torn append at the tail
```

The `fsync_sentinel` is the trick. When a writer crashes mid-append,
the tail event has no sentinel; replay truncates the log at the last
valid sentinel offset. **No torn writes are ever observable.**

Acceptance for 4a:
- Unit test: 10K appends + read-back, all OK.
- Stress test: writer crash simulated (kill -9 between fsync and
  sentinel write); replay correctly truncates the partial tail.
- Benchmark: append-only throughput on the live disk ≥ 50K events/sec
  (the only number that matters for ingest rate).

### 4b: First projection — utxo_projection

Wraps `coins_view_sqlite.c` to populate ONLY from EV_UTXO_ADD /
EV_UTXO_SPEND events. The existing UTXO write path inside `connect_block`
is RE-WIRED to emit events instead of writing directly to SQLite — the
projection consumes the events.

Two-phase rollout:
1. **Shadow:** emit events alongside the direct write. Verify projection
   state matches direct-write state for 24h on live node.
2. **Cutover:** disable direct write. SQLite becomes purely the projection.

Phase 4 acceptance gate: shadow diff is zero for 24h.

### 4c: block_index projection (kill LevelDB)

Same pattern. Block index becomes a projection over EV_BLOCK_HEADER
events. LevelDB dependency goes away entirely. **~500 MB on disk goes
to 0** (the projection is tiny because event log holds the truth).

After 4c: `vendor/lib/libleveldb.a` is no longer linked. **~3 MB binary
shrink** + dependency elimination.

### 4d: mempool, peers, wallet, znam, store projections

Mechanical conversion of each existing SQLite table to event-driven
projection. Each is a small PR.

### 4e: Delete the legacy `blocks/blk*.dat` format

The legacy block files (6.3 GB of binary) get replaced by EV_BLOCK_BODY
events in the event log. The event log is also append-only, so on-disk
size doesn't grow vs the legacy format — but it's now ONE file format
instead of "blk*.dat + index pointer in LevelDB."

This is the last 4e PR; afterward `lib/storage/src/blocks_mmap_reader.c`,
`blocks_index_legacy_reader.c`, `block_index_db.c`, `chainstate_legacy_reader.c`,
`txdb.c`, and `dbwrapper.c` are all DELETED. ~5,000 LOC.

### 4f: progress.kv folds into event log

Wave S stage cursors become EV_STAGE_CURSOR_ADVANCE events. The
`progress.kv` SQLite DB is deleted. Existing per-stage log tables
(`header_admit_log`, `validate_headers_log`, ..., `proof_validate_log`)
all become projections over their corresponding EV_STAGE_LOG events.

---

## What survives Phase 4

- `event_log.dat` — append-only, the only durable storage.
- `node_projections/` — directory of SQLite or LMDB projection files.
  Each is regenerable from event_log.dat. Crash-safe by being derived.
- `wallet_secret.dat` — wallet seed material, encrypted, separate file
  for key-material hygiene (NOT in event log — secrets don't go into
  audit trails).
- `tor_data/` — Tor's own state directory; we don't touch.

**That's it. ~3 durable files instead of ~10.**

---

## Estimated work

- 4a (event log primitive): 1 worker session. Owns: `lib/storage/event_log.*`.
- 4b (utxo projection): 1 worker session shadow + 1 cutover.
- 4c (block_index projection): 1 worker session shadow + 1 cutover.
- 4d (mempool/peers/wallet/znam/store projections): 5 worker sessions, parallelizable.
- 4e (delete legacy block files): 1 worker session.
- 4f (progress.kv fold-in): 1 worker session.

**Total: ~10 worker sessions, all parallelizable except cutover gates.**

---

## Risk + mitigations

- **Disk write amplification** — every UTXO update writes to both
  event log AND projection. Mitigation: projection writes happen
  asynchronously in batch, off the writer thread.
- **Replay time on cold start** — 10M events × replay time per event.
  Mitigation: each projection persists its `last_consumed_offset`;
  replay only catches up the delta. Cold reboot of an existing node
  replays ~0 events.
- **Event log corruption** — single file failure mode. Mitigation: the
  fsync_sentinel design + a periodic full-fingerprint checkpoint
  written to a sibling file. Tested by the kill-9 fuzz harness in 4a.
- **Schema evolution of events** — adding a field to an event payload.
  Mitigation: events are serialized with `[type][version][payload]`;
  readers handle older versions explicitly. Same pattern as Bitcoin's
  wire protocol versioning.

---

## What this unlocks

After Phase 4 lands:

- **Phase 6 (determinism + simulator)** becomes trivial. The event log
  IS the deterministic input tape. Replay any bug from a seed by
  feeding the seed to the platform.rng + replaying the event log.
- **Phase 5 (reproducible builds)** is simpler. Fewer dependencies
  (LevelDB gone). Smaller surface to audit.
- **MTBF target (5.5d → 30d)** is achievable. Single recovery path
  (replay) instead of 8 recovery paths.
- **RSS target (2.2 GB → 1 GB)** is achievable. Projections can be
  partial (LRU subset of UTXOs) without losing safety because the
  full set is in the log.
- **Cold-start target (145s → 60s)** is achievable. Boot reads
  projections' last_consumed_offset and is operational; replay is
  background.
- **Operator pages: 0/month** is the natural state. Corruption auto-
  heals by reprojecting from the log.

---

## Status

DRAFT — not actionable until Phase 2 + Phase 3 complete. Workers
should not pick up Phase 4 tasks yet.

When ready, the first dispatch is `wt?-phase4a-event-log-primitive.md`.
