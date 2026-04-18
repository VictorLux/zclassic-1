# BIP152 Compact Block Relay — Investigation

**Status:** investigation-only — no code. Wave 6 deliverable for zclassic23.
**Date:** 2026-04-11
**Owner:** agent2

## TL;DR

The zclassic23 node has **zero BIP152 support**. No sendcmpct / cmpctblock /
getblocktxn / blocktxn wire messages, no short-ID mempool index, no service
flag. Implementing BIP152 is estimated **L (large)** — the mempool short-ID
index is new infrastructure, a protocol bump is required, and the reconstruction
path has to integrate cleanly with IBD. There are no architectural showstoppers:
Sapling transaction serialization is already deterministic so SipHash short-IDs
will be stable across peers.

## 1. Current wire path (no compact blocks anywhere)

Block propagation goes through the classic `inv` / `getdata` / `block` handshake
in `lib/net/src/msgprocessor.c`:

| Handler | Location | Role |
|---|---|---|
| `process_getblocks`   | `msgprocessor.c:962`  | Legacy inventory walk |
| `process_getheaders`  | `msgprocessor.c:1020` | Headers-first sync |
| `process_inv`         | `msgprocessor.c:1117` | Receive announcements |
| `process_getdata`     | `msgprocessor.c:1208` | Serve block / tx bodies |
| `process_block_msg`   | `msgprocessor.c:1309` | Accept a block from a peer |

Inventory types are limited to three values
(`lib/net/include/net/protocol.h:24–28`):

```c
enum {
    MSG_TX             = 1,
    MSG_BLOCK          = 2,
    MSG_FILTERED_BLOCK = 3,
};
```

There is **no** `MSG_CMPCT_BLOCK` / `MSG_BLOCK_TXN` entry and **no** handler
for `sendcmpct`, `cmpctblock`, `getblocktxn`, or `blocktxn`. A repo-wide
grep for `SENDCMPCT`, `SHORT_IDS_BLOCKS`, `BIP152`, or `cmpctblock` matches
only in planning docs (`WAVE_6.md`, `BACKLOG.md`, and — after this commit —
this file). The wire surface for BIP152 is entirely absent.

## 2. Protocol version + service flags

Advertised version (`lib/net/include/net/version.h:9`):

```c
#define PROTOCOL_VERSION 170011
#define MIN_PEER_PROTO_VERSION 170002
```

Bitcoin Core gates BIP152 behind `SHORT_IDS_BLOCKS_VERSION = 70014`. Because
zclassic uses a dedicated namespace (`170000+`), we have a free hand here:
we can pick any version ≥ `170012` as the cut-over, or — simpler — gate on a
new service bit.

Service flags (`lib/net/include/net/protocol.h:20–22`):

```c
#define NODE_NETWORK (1 << 0)
#define NODE_BLOOM   (1 << 2)
```

BIP152 announces capability by sending `sendcmpct` in the version handshake
rather than via a service bit. We should keep that pattern: no new service
flag, just a `sendcmpct` emitter after `verack`. But we must bump
`PROTOCOL_VERSION` to let peers negotiate min-version.

## 3. Mempool reconstruction — new secondary index required

The delta that BIP152 ships instead of a full block is an 80-byte header plus
an array of 6-byte SipHash-derived short-IDs. On receipt, the node reconstructs
the block by looking up each short-ID in its mempool. If any short-ID is
missing, it sends `getblocktxn` for the missing indices and waits for a
`blocktxn` response.

The current mempool only indexes by full 256-bit hash
(`lib/validation/include/validation/txmempool.h:76–94`):

```c
struct tx_mempool {
    struct mempool_entry      *entries;    /* dense array */
    struct outpoint_map_entry *next_tx;    /* spend-set, keyed by (hash,n) */
    struct priority_delta     *deltas;     /* RBF priority bumps */
    ...
};
```

`tx_mempool_lookup` at `lib/validation/src/txmempool.c:301–315` does an O(n)
linear scan of `entries[i].tx.hash`. There is **no short-ID index**.

**Implementation cost:** a new hashmap keyed on 64-bit short-IDs (lower 48
bits used per BIP152, but 64-bit storage is cheaper). Two options:

1. **Derive on every receive**: on `cmpctblock`, iterate the entire mempool,
   compute SipHash per entry, build a temporary map, and drop it. Simple,
   zero memory cost at rest, O(|mempool|) per compact block received. At
   50k mempool entries this is still < 10 ms — acceptable.
2. **Persistent short-ID index**: hashmap updated on every `tx_mempool_add`/
   `_remove`. O(1) lookup at receive time. More memory, more code to
   maintain, more surface for bugs.

**Recommendation: go with option 1 first.** Simpler to land, easy to
benchmark against a 50k-entry mempool, and we can add the persistent index
later if profiles show it matters.

Note that BIP152's short-ID SipHash is keyed with
`SipHash(k0, k1, block_header)` — so the per-block key is derived from the
header itself. Each peer computes the same key for the same block, so short
IDs are consistent across the wire.

## 4. Zclassic-specific concerns — none

The short-ID is computed over the transaction's wire serialization. Any
serialization instability would make short-IDs non-reproducible across peers.
Reviewing `lib/primitives/src/transaction.c:362–428`:

- **v4 Sapling txs** serialize in a fixed field order: inputs → outputs →
  locktime → expiry → value_balance → shielded_spends → shielded_outputs
  → joinsplits.
- **JoinSplit blob** (`js_description_serialize`, `transaction.c:303`) writes
  in a fixed order: vpub_old, vpub_new, anchor, nullifiers[0..3],
  commitments[0..1], ephemeral_key, ciphertexts[0..1], zk_proof. No
  randomness, no reordering.
- **Sapling spend/output proofs** (`spend_description_serialize:259`,
  `output_description_serialize:281`) write fixed byte arrays directly.
- **Sprout JoinSplits** in v2/v3 transactions use the pre-Sapling field
  order, but are still byte-stable.

**Conclusion**: `transaction_serialize` is deterministic, so SipHash
short-IDs will be consistent across implementations.

## 5. Reference implementation

No bitcoin-core / zclassic-cpp tree is vendored under `vendor/`. The
canonical reference is Bitcoin Core mainline (as of the 0.13.x BIP152
landing, ~2016):

- `src/net_processing.cpp` — handlers for `sendcmpct` / `cmpctblock` /
  `getblocktxn` / `blocktxn`
- `src/blockencodings.h` — `CBlockHeaderAndShortTxIDs` /
  `BlockTransactionsRequest` / `BlockTransactions` primitives
- `src/util/siphash.h` — the SipHash-2-4 implementation used for short-IDs

The AGENT2 current-status log references `~/zclassic-cpp` as the historical
C++ reference tree — if present on an operator's machine, it will already
have BIP152 code in the same structure as Bitcoin Core.

## 6. Implementation sketch (for the follow-up, not this commit)

1. **Protocol version bump** — `PROTOCOL_VERSION = 170012`.
   Peers at `< 170012` stay on the legacy `inv`/`getdata`/`block` path.

2. **New wire structs** (`lib/net/include/net/compactblock.h`):
   - `struct cmpct_block { block_header header; uint64_t nonce;
     size_t num_short_ids; uint64_t *short_ids;
     size_t num_prefilled; struct prefilled_tx *prefilled; }`
   - `struct block_txn_request { uint256 blockhash; size_t num_indices;
     uint32_t *indices; }`
   - `struct block_txn { uint256 blockhash; size_t num_txns;
     struct transaction *txns; }`

3. **New wire messages** in `msgprocessor.c`:
   - `process_sendcmpct()` — mark peer as compact-capable
   - `process_cmpctblock()` — attempt reconstruction; on miss, emit
     `getblocktxn`
   - `process_getblocktxn()` — serve requested tx indices from a block
     the sender has (look up via block_index + disk_block_io)
   - `process_blocktxn()` — splice received txns into the pending
     reconstruction and activate the block

4. **SipHash helper** in `lib/crypto/siphash.{h,c}` — 2-4 variant, matches
   BIP152 spec. Short-ID is `SipHash(k0, k1, tx_wire_bytes) & 0xFFFFFFFFFFFF`
   (lower 48 bits).

5. **Mempool short-ID lookup** — option 1 above: iterate on each receive.

6. **Tests** — at minimum:
   - serialize/deserialize round-trip for all four new wire messages
   - reconstruct a full block from a cmpctblock using a seeded mempool
   - fall back to `getblocktxn` when a short-ID is missing
   - SipHash vector test (BIP152 appendix has known-answer vectors)

## 7. Cost estimate

- New files: `~400 lines` in `lib/net/{src,include}/compactblock*`,
  `~150 lines` for SipHash
- `msgprocessor.c`: ~300 added lines (four new handlers + dispatch)
- Mempool: ~50 lines (inline short-ID iterator helper)
- Tests: ~500 lines

**Risk items**:
- Version handshake ordering — BIP152 requires `sendcmpct` to arrive after
  `verack` but before any `inv`; the existing handshake flow in
  `msgprocessor.c` needs a small insertion point
- Reconstruction correctness against adversarial peers — a peer could
  craft a cmpctblock whose short-IDs collide with unrelated mempool txs,
  producing a bogus reconstructed block; the post-reconstruction hash
  check catches this but the test suite must exercise the miss/collision
  path explicitly
- No Sapling-specific risks identified

## 8. Recommended next steps

- **Not now.** BIP152 is a bandwidth optimization, not a correctness fix.
  Wave-6 priority queue has several higher-value items (PHGR13 sync stall,
  boot decomposition).
- **When scheduled**, split into three landings: (1) SipHash + short-ID
  helper + tests, (2) wire structs + serialization + tests, (3) handlers
  + version bump + integration. Each should leave `./test_zcl` green.

---

_This investigation consumed one session. No code changes were made._
