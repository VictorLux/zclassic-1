# ZClassic23 Sync Guide

**zclassic23** is the next-generation ZClassic power node. This guide covers
how a fresh zclassic23 node reaches chain tip, and the one legacy-bootstrap
path that exists only while the zclassic23 peer network is still small.

Primary = zclassic23-native. Legacy = pulling data from the old C++ `zclassicd`.

---

## Method 1 (native): P2P Fast Sync (~60 s)

The zclassic23-native path. A fresh node downloads a verified UTXO snapshot
from another zclassic23 peer, then catches up the tail via standard P2P.

```bash
./zclassic23 -fastsync -addnode=<zclassic23_peer>
```

What happens:
1. Find a peer advertising `NODE_ZCL23`.
2. Receive a strict v2 UTXO snapshot manifest (protocol/schema version,
   anchor height/hash, serving peer tip, chainwork, UTXO SHA3, byte length,
   UTXO count, chunk size/count, and per-chunk hashes).
3. Download chunks in parallel, verify each chunk hash before import.
4. Verify FlyClient MMR/MMB proof binding the UTXOs to the PoW chain and
   reject missing or non-competitive chainwork.
5. Verify the imported UTXO set SHA3 exactly matches the manifest.
6. Activate only an anchor at least 10 blocks behind the serving peer's tip,
   then delta-sync the finality window to tip by normal block validation.

**Use this for any fresh zclassic23 deployment where at least one other
zclassic23 peer exists.** Once the network is established this becomes the
default and only user-facing path.

### zclassic-only serving profile

Use `-profile=zclassic-only` for power nodes whose job is to sync other
zclassic23 nodes quickly. This profile keeps consensus state, P2P, RPC,
snapshot offer construction, FlyClient/MMB proof serving, and normal block
relay. It intentionally does not start explorer cache prewarming, store/market
services, onion hosting unless `-tor` is explicitly set, or file-service
snapshot export and chunk/block-piece manifests.

Full, onion-node, and legacy-compat profiles keep the broader app surfaces.
The explorer profile keeps explorer APIs and cache prewarming but still avoids
store and file-service serving.

---

## Method 2 (native): Full P2P Sync (~7 h)

Trustless sync from genesis over the standard P2P protocol. No snapshot.

```bash
./zclassic23 -addnode=<any_peer>
```

Headers → blocks → connect. Scripts/signatures below deferred proof validation height
(h=3,054,000) are accepted; full validation runs above that. Background
services then re-verify every hash, signature, and proof end to end.

Use when no snapshot source is available (or when you want to validate from
scratch).

---

## Method 3 (legacy bootstrap, development only): LevelDB Import from zclassicd (~20 s)

**This path exists because the zclassic23 peer network is still small on
mainnet.** We temporarily pull data from a running legacy `zclassicd` (C++)
node, which is widely deployed, to get developer workstations to tip fast.
`zclassicd` is advisory only: its block files, UTXO snapshots, and height/hash
answers are accepted only when they also match compiled SHA3 anchors, runtime
window commitments, local consensus state, or quorum with zclassic23 peers.
Once the zclassic23 peer network is healthy, this path goes away.

Requirements: a local synced legacy `zclassicd` with `~/.zclassic/chainstate/`.

```bash
systemctl --user stop zclassic23
cp -r ~/.zclassic/chainstate ~/.zclassic-c23/chainstate
for f in ~/.zclassic/blocks/blk*.dat; do cp "$f" ~/.zclassic-c23/blocks/; done
sqlite3 ~/.zclassic-c23/node.db \
  "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'"
systemctl --user start zclassic23
```

On boot, zclassic23 reads `'c'+txid` LevelDB keys, decodes compressed CCoins,
and writes 1.35M UTXOs into its SQLite UTXO store in ~9.7 s (30 decoder threads
→ single SQLite writer). Post-import SHA3-256 over the full UTXO set is
checked against a hardcoded commitment before the node starts serving.

Rules:
- Always **copy**, never symlink — zclassicd writes to its block files.
- Never copy LevelDB from `~/.zclassic-c23/` into zclassicd — different formats.
- Legacy data is acceleration only. It must match compiled SHA3 windows,
  runtime windows, local consensus checks, or zclassic23 quorum before it
  elevates trust.
- To force reimport after it's been run once:
  `./zclassic23 -reimport-utxos -datadir=~/.zclassic-c23`

See `memory/reference_zclassicd_service.md` for managing the legacy peer service.

### Legacy chain oracle boundary

All direct reads from local legacy `zclassicd` RPC go through
`rpc/legacy_rpc_client.h` for transport and `rpc/legacy_chain_oracle.h` for
typed chain data such as block hashes, MMB leaves, and chainwork. Boot-time MMB
catchup, fast-sync offer construction, FlyClient proof fallback, and the
zclassicd drift oracle share that transport instead of parsing JSON-RPC in
their own service code.

This boundary keeps legacy compatibility behind a small adapter: zclassic23
services may use legacy responses as bridge/oracle inputs, but snapshot
acceptance still requires the native v2 manifest contract, FlyClient MMB/MMR
proofs, PoW/chainwork checks, finality policy, and UTXO SHA3 verification.

---

## Verification Layers

Every sync method reaches the same verified state. Background services run
after sync completes:

| Service | What it verifies | Speed |
|---------|------------------|-------|
| `bg_hash_verify` | SHA256d of every block header | 83K blk/s |
| `bg_validation`  | Equihash, ECDSA, Groth16, merkle roots | 400–500 blk/s |
| Boot checks      | UTXO count + XOR commitment vs checkpoint | ~2 s |
| Post-import      | SHA3-256 full UTXO set vs hardcoded commitment | ~5 s |

See `lib/validation/include/validation/validation_audit.h` for the full matrix.

---

## Self-Healing

| Problem | Recovery |
|---------|----------|
| Missing UTXO during connect_block | Look up source tx via tx index, extract output, retry |
| Missing undo data during disconnect | Reconstruct from tx index + source blocks |
| Wrong block on disk (hash mismatch) | Clear `BLOCK_HAVE_DATA`, re-download from P2P |
| Stale `coins_best_block` after crash | Boot detects mismatch, resets to a consistent state |
| Download stall | Scan 10-height window for gaps, request from alternate peers |

---

## Data Directory

```
~/.zclassic-c23/
├── node.db              SQLite (UTXOs, block index cache, node state, wallet)
├── blocks/
│   ├── blk*.dat         Block data (4B magic + 4B size + block)
│   ├── rev*.dat         Undo data for reorgs
│   └── index/           LevelDB block index
├── chainstate/          LevelDB UTXO set (only used during legacy import)
├── block_index.bin      Flat file cache (instant block index load on restart)
└── wallet.dat           Encrypted wallet keys
```

Key `node_state` keys: `coins_best_block`, `tip_height`, `leveldb_utxo_migrated`,
`bg_validation_height`, `bg_hash_verification_height`.

Finality policy: `ZCL_FINALITY_DEPTH=10`. Heights `<= tip - 10` are treated as
immutable for reorg refusal, snapshot eligibility, rolling SHA3 anchors,
block-window reuse, and diagnostics. Steady-state reorgs of 10 blocks or less
are allowed; 11-block reorgs are refused. IBD can still resolve deeper
competition before a verified immutable anchor is installed.

The code path for these decisions is centralized in
`validation/sync_evidence_policy.h`. `COINBASE_MATURITY=100` remains a consensus
spend-maturity rule and does not control reorg depth or immutable-prefix
policy.

Snapshot protocol: zclassic23 peers must speak
`FAST_SYNC_PROTOCOL_VERSION=2` and `FAST_SYNC_SNAPSHOT_SCHEMA_VERSION=1`.
Missing v2 fields, zero chainwork, non-final anchors, missing MMR/MMB roots,
or stale schemas are rejected. Legacy/non-v2 data may still be used locally
for bootstrap acceleration, but it is not trusted P2P snapshot sync.

Quorum model: votes are grouped by source class: local zclassic23,
local zclassicd, and remote zclassic23 peers. Remote votes are keyed by
unique peer and expire by TTL; rolling-anchor commits require a matching
source-class quorum when multiple source classes are available. Splits halt
anchor extension and are visible through the quorum/oracle dumpstate surface.

Rolling anchors: runtime SHA3 windows are only persisted for fully immutable
windows with local block bytes present, normal oracle policy, and quorum
approval. On load, runtime anchor files are checksum-, schema-, alignment-,
and continuity-checked against compiled anchors; failures discard the runtime
file.

---

## Operator Runbook

### Check sync status
Use MCP: `zcl_status`, `zcl_kpi`, `zcl_syncstate`, `zcl_validationstatus`.
(Or the `zcl-rpc` escape hatch if MCP is unavailable: `zcl-rpc getblockchaininfo`.)
Status/dumpstate surfaces include sync phase, local/header/peer heights,
immutable height, snapshot anchor, UTXO root, chainwork/quorum verdict,
watchdog state, last recovery, and active acceleration source where available.

### Recovery from OOM kill
Just restart — the node detects stale state and resets to a consistent point.

### Nuclear reset
```bash
systemctl --user stop zclassic23
rm -rf ~/.zclassic-c23/node.db ~/.zclassic-c23/blocks/ ~/.zclassic-c23/chainstate/
rm -f  ~/.zclassic-c23/block_index.bin
systemctl --user start zclassic23
```
The node re-syncs via Method 1 or 2 from its configured peers.

---

## Architecture

```
        NATIVE                                   LEGACY BOOTSTRAP
┌──────────────────────┬──────────────────┐  ┌────────────────────┐
│  Method 1: fastsync  │ Method 2: full   │  │ Method 3: from     │
│  zclassic23 peer     │ P2P from genesis │  │ zclassicd          │
│  ~60 s               │ ~7 h             │  │ chainstate → SQLite│
│  NODE_ZCL23 + chunks │ headers + blocks │  │ ~20 s, dev-only    │
└──────────┬───────────┴────────┬─────────┘  └─────────┬──────────┘
           │                    │                      │
           ▼                    ▼                      ▼
         ┌───────────────────────────────────────────────┐
         │           UTXO SET (SQLite)                   │
         │  1.35M entries · coins_best_block @ tip       │
         └────────────────────┬──────────────────────────┘
                              ▼
         ┌───────────────────────────────────────────────┐
         │        BACKGROUND VERIFICATION                │
         │  bg_hash_verify · bg_validation · checkpoints │
         └───────────────────────────────────────────────┘
```
