# ZClassic23 API Reference

Version 0.1.0

## Overview

ZClassic23 exposes three interfaces:

| Interface | Port | Auth | Format |
|-----------|------|------|--------|
| **JSON-RPC** | 18232 | Cookie file or `rpcuser:rpcpassword` | JSON-RPC 1.0 |
| **REST API** | 443 (TLS) | None | JSON with CORS |
| **Block Explorer** | 443 (TLS) | None | HTML |

## JSON-RPC

### Connection

```bash
# Using zcl-rpc (reads cookie automatically)
zcl-rpc <method> [params...]

# Using curl
curl -u $(cat ~/.zclassic-c23/.cookie) -d '{"method":"getinfo","params":[]}' http://localhost:18232/
```

Cookie file: `~/.zclassic-c23/.cookie` (regenerated each start)

---

### Blockchain

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getblockchaininfo` | | Chain state: height, difficulty, best block |
| `getblockcount` | | Current block height |
| `getbestblockhash` | | Hash of chain tip |
| `getblockhash` | `height` | Block hash at given height |
| `getblockheader` | `hash_or_height [verbose]` | Block header |
| `getblock` | `hash_or_height [verbosity]` | Block data (0=hex, 1=JSON, 2=JSON+tx) |
| `getdifficulty` | | Current proof-of-work difficulty |
| `getmempoolinfo` | | Mempool size, bytes, usage |
| `gettxoutsetinfo` | | UTXO set statistics (count, total value) |

### Data Integrity (SHA3)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getutxocommitment` | | SHA3-256 hash over entire UTXO set in canonical order |
| `getdataintegrity` | | SHA3-256 hashes over ALL 12 consensus tables + master hash |
| `verifycheckpoint` | | Verify UTXO set against hardcoded SHA3 checkpoint |
| `getmmrroot` | | Merkle Mountain Range root over all block hashes |

**`getutxocommitment`** — Computes a deterministic SHA3-256 hash over every UTXO sorted by (txid, vout). Two nodes with identical UTXO sets produce the same hash. Takes ~1 second over 1.3M UTXOs.

```json
{
  "sha3_hash": "00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0",
  "height": 3056758,
  "utxo_count": 1354771,
  "elapsed_seconds": 1
}
```

**`getdataintegrity`** — SHA3-256 over every row of all consensus-critical tables: blocks, transactions, tx_inputs, tx_outputs, utxos, sapling_nullifiers, sapling_outputs, sapling_spends, sprout_nullifiers, joinsplits, zslp_tokens, zslp_transfers. Returns per-table hashes for diagnostics plus a master hash.

```json
{
  "blocks": "5e9ec791...",
  "transactions": "89359c96...",
  "utxos": "25d75dd9...",
  "zslp_tokens": "5d358e2c...",
  "master": "209f92de...",
  "height": 3056758,
  "elapsed_seconds": 96
}
```

**`verifycheckpoint`** — Compares local UTXO set SHA3 hash against the hardcoded checkpoint (height 3,056,758, verified bit-for-bit against zclassicd). Returns `PASSED` or `FAILED`.

```json
{
  "status": "PASSED",
  "checkpoint_height": 3056758,
  "expected_sha3": "00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0",
  "computed_sha3": "00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0",
  "expected_utxos": 1354771,
  "computed_utxos": 1354771
}
```

**`getmmrroot`** — Merkle Mountain Range root (SHA3-256 with domain separation) over all block hashes. Enables O(log n) inclusion proofs between power nodes.

```json
{
  "mmr_root": "6d03d381...",
  "num_leaves": 3056758,
  "num_peaks": 12
}
```

### Chain Inspection

| Command | Parameters | Description |
|---------|-----------|-------------|
| `chainview` | | Chain view details |
| `chainstats` | | Chain statistics summary |
| `gettxdetail` | `txid` | Detailed transaction information |
| `saplingtreeinfo` | | Sapling merkle tree state |
| `verifychainroots` | | Verify integrity of all chain roots |

### Chain Import & Repair

| Command | Parameters | Description |
|---------|-----------|-------------|
| `indexlegacy` | `path` | Index legacy zclassicd block files into SQLite |
| `importchainstate` | `path` | Import UTXO set from LevelDB chainstate directory |
| `reindexchainstate` | | Rebuild UTXO set from block data on disk |

**`importchainstate`** — The primary tool for fixing UTXO gaps. Reads every UTXO from a LevelDB chainstate directory and replaces the SQLite UTXO set. Uses parallel pipeline (30 decoder threads on 32-core). Runs live via RPC — no restart needed.

```bash
# Import from your own chainstate (created during fastsync)
zcl-rpc importchainstate ~/.zclassic-c23/chainstate

# Import from a zclassicd data directory
zcl-rpc importchainstate /path/to/.zclassic/chainstate
```

### HODL Wave Analysis

| Command | Parameters | Description |
|---------|-----------|-------------|
| `gethodlwave` | | HODL wave data (UTXO age distribution) |
| `gethodlwaveimage` | | HODL wave as SVG image |
| `gethodlwavetimeline` | | HODL wave timeline data |
| `gethodlwavechart` | | HODL wave chart data |

### Raw Transactions

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getrawtransaction` | `txid [verbose]` | Raw transaction (hex or JSON) |
| `decoderawtransaction` | `hex` | Decode raw transaction hex |
| `createrawtransaction` | `[inputs] [outputs]` | Create unsigned raw transaction |
| `signrawtransaction` | `hex [prevtxs] [privkeys]` | Sign raw transaction |
| `sendrawtransaction` | `hex` | Broadcast signed transaction |

### Mining

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getmininginfo` | | Mining status and difficulty |
| `getblocktemplate` | `[params]` | Block template for miners |
| `submitblock` | `hex` | Submit solved block |
| `getblocksubsidy` | `[height]` | Block reward at height |

### Network

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getnetworkinfo` | | Network state (version, connections) |
| `getpeerinfo` | | Connected peer details |
| `getconnectioncount` | | Number of connected peers |
| `addnode` | `ip:port add\|remove\|onetry` | Manage peer connections |
| `ping` | | Ping all peers |

### Wallet (Transparent)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getbalance` | `[minconf]` | Wallet balance |
| `getwalletinfo` | | Wallet status |
| `getnewaddress` | | Generate new t-address |
| `listunspent` | `[minconf] [maxconf]` | List UTXOs |
| `sendtoaddress` | `addr amount` | Send ZCL |
| `dumpprivkey` | `address` | Export private key (WIF) |
| `importprivkey` | `wif [label] [rescan]` | Import private key |
| `validateaddress` | `address` | Validate address format |

### Wallet (Shielded / Sapling)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `z_getnewaddress` | | Generate new z-address |
| `z_listaddresses` | | List all z-addresses |
| `z_getbalance` | `address [minconf]` | Shielded balance |
| `z_gettotalbalance` | `[minconf]` | Total balance (t + z) |
| `z_sendmany` | `from [{addr, amount}]` | Shielded send |
| `z_listunspent` | `[minconf] [maxconf]` | Shielded UTXOs |
| `z_exportkey` | `address` | Export spending key |
| `z_importkey` | `key [rescan] [height]` | Import spending key |

### Wallet Sync

| Command | Parameters | Description |
|---------|-----------|-------------|
| `rescanblockchain` | `[start] [stop]` | Rescan for wallet txs |
| `rescanwallet` | | Rescan wallet |

### Control & Monitoring

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getinfo` | | Server info (version, height, connections) |
| `stop` | | Graceful shutdown |
| `healthcheck` | | Health status (pass/fail) |
| `eventlog` | `[count]` | Last N events from ring buffer |
| `syncstate` | | Sync state machine status |
| `downloadstats` | | Download manager statistics |
| `coinsinfo` | | UTXO cache diagnostics |

### ZSLP Tokens

| Command | Parameters | Description |
|---------|-----------|-------------|
| `zslp_createtoken` | `ticker name decimals supply` | Create GENESIS tx |
| `zslp_send` | `token_id address amount` | Send tokens |
| `zslp_mint` | `token_id address amount` | Mint new tokens |
| `zslp_balance` | `token_id address` | Query token balance |

---

## REST API

Base URL: `https://zclnet.net` (port 443)

All endpoints return JSON with `Access-Control-Allow-Origin: *`.

| Endpoint | Description |
|----------|-------------|
| `GET /api/blocks` | Latest 25 blocks |
| `GET /api/block/:id` | Block by height or hash |
| `GET /api/tx/:txid` | Transaction detail |
| `GET /api/address/:addr` | Address balance + UTXOs |
| `GET /api/stats` | Network stats (height, difficulty, supply) |
| `GET /api/stats/deep` | Extended stats (shielded, ZSLP, addresses) |
| `GET /api/supply` | Circulating supply (plain number, CoinGecko format) |
| `GET /api/hodl` | HODL wave data |
| `GET /api/events?count=N` | Event log (lock-free ring buffer) |
| `GET /api/health` | HTTP 200/503 health check |
| `GET /api/syncstate` | Sync state machine |
| `GET /api/downloadstats` | Download manager stats |
| `GET /api/factoids` | Historian factoids (SHA3 receipts) |

---

## Block Explorer

HTML routes at `https://zclnet.net/explorer`. CSS customizable via `{datadir}/explorer/style.css`.

| Route | Description |
|-------|-------------|
| `/explorer` | Dashboard: latest blocks, mempool, network |
| `/explorer/block/:id` | Block detail (height or hash) |
| `/explorer/tx/:txid` | Transaction: inputs, outputs, shielded, ZSLP |
| `/explorer/address/:addr` | Address: balance, UTXO list |
| `/explorer/stats` | SVG charts (24h/7d/30d/1y/all) |
| `/explorer/hodl` | HODL wave chart |
| `/explorer/tokens` | ZSLP token scanner |
| `/explorer/factoids` | Historian factoids |
| `/explorer/search?q=` | Smart search (height, hash, txid, address) |

---

## P2P Protocol (ZCL23 Extension)

Power nodes (service bit `NODE_ZCL23 = 1024`) exchange these messages for fast sync:

### UTXO Snapshot Sync
| Message | Direction | Description |
|---------|-----------|-------------|
| `zsnapshot` | Server → Client | Offer UTXO snapshot (height, root hash, count) |
| `zsnapreq` | Client → Server | Accept snapshot offer |
| `zsnapdata` | Server → Client | Chunked UTXO transfer (500 per chunk) |
| `zsnapend` | Server → Client | End of snapshot transfer |
| `zmanifest` | Server → Client | Chunk manifest with Merkle root |

After all chunks received, the client computes SHA3-256 over its UTXO set and verifies against the manifest root. Prints `SHA3 UTXO verification: PASSED` or `FAILED`.

### Block Swarm (BitTorrent-Style)
| Message | Direction | Description |
|---------|-----------|-------------|
| `zblkmanfst` | Both | Block piece manifest (height range, SHA3 piece hashes) |
| `zblkreq` | Client → Server | Request piece by index |
| `zblkdata` | Server → Client | Piece response (128 blocks of hashes) |
| `zblkbitmap` | Both | Piece availability bitmap |

Each piece = 128 blocks. SHA3-256 verified. Rarest-first selection. 4-deep pipeline per peer. Endgame mode broadcasts last 8 pieces to all peers.

**Example: Peer 3000 blocks behind**
- 3000 blocks / 128 per piece = 24 pieces needed
- With 4 peers x 4 pipeline depth = 16 pieces in flight simultaneously
- Each piece SHA3 verified independently
- Synced in seconds, not minutes

### Defense
- 20-bit PoW per snapshot request (hashcash)
- 5000 chunks/IP/hour rate limit
- Misbehavior scoring (auto-ban at 100 points)

---

## SHA3 Checkpoint

At height **3,056,758**, every node MUST pass the hardcoded SHA3 UTXO checkpoint:

```
SHA3:    00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0
UTXOs:   1,354,771
Supply:  10,364,138.33747381 ZCL
Block:   000002979090fba9da6cdc140d050245c1b637480609510922662407855bd653
```

This is **mandatory** — enforced in `connect_block`. A node with a corrupt UTXO set will halt at this height with a fatal error. Verified bit-for-bit against the zclassicd reference implementation.

---

## Quick Start: New Node

```bash
# Build
make zclassic23

# Fast bootstrap from zclassicd (same machine, instant symlinks)
./zclassic23 -fastsync ~/.zclassic -datadir=~/.zclassic-c23

# Or start fresh and sync via P2P
./zclassic23 -datadir=~/.zclassic-c23 -addnode=74.50.74.102

# Fix UTXO gaps (if stuck during sync)
zcl-rpc importchainstate ~/.zclassic-c23/chainstate

# Verify data integrity
zcl-rpc verifycheckpoint
zcl-rpc getutxocommitment
zcl-rpc getdataintegrity
```

## Error Codes

| Code | Meaning |
|------|---------|
| -1 | General error |
| -3 | Invalid type |
| -5 | Invalid address |
| -6 | Insufficient funds |
| -8 | Invalid parameter |
| -25 | Transaction already in chain |
| -26 | Transaction rejected |

## Rate Limits

| Interface | Limit |
|-----------|-------|
| Onion service | 100 requests/second |
| Fast sync | 5000 chunks/IP/hour + 20-bit PoW |
| Block swarm | Rate-limited per peer |
| RPC | No limit (localhost only) |
