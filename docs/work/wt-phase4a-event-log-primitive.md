# Worker Assignment — Phase 4a: event_log primitive

**Worktree:** wt2 OR wt3 (either)
**Branch:** `wt?/phase4a-event-log-primitive`
**Phase:** 4 (Storage unification)
**Depends on:** Phase 3 dissolves complete (sync_watchdog, chain_advance,
legacy_mirror DELETED). Doesn't depend on cutover — primitive can be
built standalone.
**Plan reference:** [`docs/architecture/phase4-storage-unification.md`](../architecture/phase4-storage-unification.md)

**Owns:**
- NEW `lib/storage/include/storage/event_log.h`
- NEW `lib/storage/src/event_log.c`
- NEW `lib/test/src/test_event_log.c` — includes the kill-9 fuzz harness
- Edits to `test.c`, `test_parallel.c`, `test_helpers.h`

**MUST NOT touch:**
- Existing storage layers (`block_index_db.c`, `coins_db.c`,
  `progress_store.c`, etc.) — Phase 4b+ wires them up; this PR is
  pure primitive
- `app/`, `tools/`, `config/`
- `lib/framework/`, `lib/util/` (except adding event_log.h)

---

## Why this matters

The event log is the foundation of Phase 4. It's the append-only file
that becomes the single source of truth, with N projections derived
from it.

This PR ships ONLY the primitive — no callers wire it up yet. That
makes it low-risk and independently testable.

Read `docs/architecture/phase4-storage-unification.md` §"4a — Build
the event log primitive" before starting.

---

## API

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
    EV_STAGE_CURSOR_ADVANCE = 11,
    /* Add cautiously — every entry is a permanent surface. */
};

event_log_t *event_log_open(const char *path);
void         event_log_close(event_log_t *log);

uint64_t event_log_append(event_log_t *log,
                          enum event_type type,
                          const void *payload, size_t payload_len);

int event_log_read(event_log_t *log, uint64_t offset,
                   enum event_type *type_out,
                   void *buf, size_t buf_cap, size_t *out_len);

typedef bool (*event_log_cb)(uint64_t offset, enum event_type type,
                              const void *payload, size_t len,
                              void *user);
int event_log_stream(event_log_t *log, uint64_t start_offset,
                     event_log_cb cb, void *user);

int event_log_fingerprint(event_log_t *log, uint8_t out[32]);
```

## Wire format (per event)

```
[ 4B  payload_length    ]
[ 4B  event_type        ]
[ 4B  flags (reserved)  ]
[ 4B  payload_crc32c    ]
[ NB  payload           ]
[ 16B fsync_sentinel    ]    <- 8B magic 0xE7E10C5E_NTSENTNL + 8B offset
```

The sentinel design:
1. Write the header + payload (4+4+4+4 + N bytes).
2. `fsync(fd)`.
3. Write the sentinel (16 bytes: magic + offset).
4. `fsync(fd)`.

On replay/recovery:
- Scan from end of file backward.
- Last event must end with the magic sentinel containing its own offset.
- If sentinel is absent or wrong, TRUNCATE the file at the start of
  the partial event.
- Result: no torn writes are ever observable.

---

## Tasks (in order)

### Task 1: Header + stub implementation

Just the public API surface, with `event_log_open` returning a stub
handle. No real I/O yet.

Acceptance: code compiles, header documented, stub returns NULL or a
no-op handle.

### Task 2: Append + read implementations

Implement the wire format. Use `pwrite` + `fsync` for now (Phase 7a
can replace with io_uring later). Use crc32c from
`lib/crypto/include/crypto/` if available — otherwise the included
public-domain one in `vendor/`.

Acceptance: 1000 appends round-trip via `event_log_read`.

### Task 3: Stream implementation

`event_log_stream(start_offset, cb, user)` — opens the file at offset,
iterates events until end or `cb` returns false.

Acceptance: stream over 1000 appended events; callback sees all of
them in order.

### Task 4: Fingerprint (SHA3-256 over log content)

Iterates the log, hashes the entire payload (NOT including sentinels,
which are framing). Result fits in 32 bytes.

Acceptance: identical content → identical fingerprint. One-byte change
→ different fingerprint.

### Task 5: kill-9 fuzz harness

`lib/test/src/test_event_log.c` includes a test that:
1. Forks a child process that loops appending events.
2. After K events (random K), parent sends SIGKILL.
3. Parent opens the log, scans for valid sentinels, computes how many
   complete events landed.
4. Verifies the last complete event has a valid sentinel + checksum.
5. Verifies any partial event at the tail is correctly truncated by
   the next `event_log_open`.

Run with K = 1..1000 in a parameterized loop. **All runs must result
in a clean, valid log after recovery.**

This is the load-bearing test. If it doesn't hold, fix the sentinel
logic before proceeding.

### Task 6: Benchmark

```bash
./test_event_log --bench
```

Reports: append throughput (events/sec) on the live disk.
**Target: ≥ 50K events/sec.**

If significantly below, document the disk + filesystem (probably tmpfs
will hit 200K, ext4 with fsync will hit 50K-100K, ZFS varies).

### Task 7: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
git push origin wt?/phase4a-event-log-primitive
```

Append Completion section.

---

## Commit cadence

One commit per task. Push after tasks 2, 4, 5.

---

## What this does NOT do

- Does NOT wire the event log into any existing call site.
- Does NOT replace any current storage layer.
- Does NOT change any production behavior.

It's purely the new primitive sitting in `lib/storage/` waiting for
the next sub-phase to start using it.

---

## Status

**READY** (status updated 2026-05-24) — the prior "gated on Phase 3"
note was about priority ordering, not a technical dependency. This
primitive sits in `lib/storage/` with no callers wired, so it's safe
to land any time. Any worker may claim by marking IN PROGRESS (wt<N>).

<!-- Worker: append a Completion section below when done. -->
