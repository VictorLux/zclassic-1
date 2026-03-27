# ZClassic23 API Reference

## Overview

ZClassic23 exposes three interfaces:

| Interface | Port | Auth | Format |
|-----------|------|------|--------|
| **JSON-RPC** | 18232 | Cookie file or `rpcuser:rpcpassword` | JSON-RPC 1.0 |
| **REST API** | 443 (TLS) | None | JSON with CORS |
| **Block Explorer** | 443 (TLS) | None | HTML |

---

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
| `getdataintegrity` | | SHA3-256 hashes over all 12 consensus tables + master hash |
| `verifycheckpoint` | | Verify UTXO set against hardcoded SHA3 checkpoint |
| `getmmrroot` | | Merkle Mountain Range root over all block hashes |

**`getutxocommitment`** — Deterministic SHA3-256 over every UTXO sorted by (txid, vout). Two nodes with identical UTXO sets produce the same hash. ~1 second over 1.3M UTXOs.

```json
{
  "sha3_hash": "00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0",
  "height": 3056758,
  "utxo_count": 1354771,
  "elapsed_seconds": 1
}
```

**`verifycheckpoint`** — Compares local UTXO SHA3 against hardcoded checkpoint at height 3,056,758. Returns `PASSED` or `FAILED`.

**`getmmrroot`** — Merkle Mountain Range root (SHA3-256 with domain separation) over all block hashes. Enables O(log n) inclusion proofs between power nodes.

### Chain Inspection

| Command | Parameters | Description |
|---------|-----------|-------------|
| `chainview` | | Chain view: fork info, active chain details |
| `chainstats` | | Chain statistics: blocks, txs, data size |
| `gettxdetail` | `txid` | Detailed transaction with inputs/outputs |
| `saplingtreeinfo` | | Sapling merkle tree state |
| `verifychainroots` | | Verify integrity of all chain roots |
| `hodltimeseries` | `[years]` | Monthly HODL wave time series (default 9 years) |

### Chain Import & Repair

| Command | Parameters | Description |
|---------|-----------|-------------|
| `importchainstate` | `path` | Import UTXO set from LevelDB chainstate |
| `reindexchainstate` | | Rebuild UTXO set by replaying all blocks |
| `indexlegacy` | `path` | Index legacy zclassicd block files into SQLite |
| `repairutxos` | `[port] [creds] [num_blocks]` | Fetch missing UTXOs from running zclassicd |
| `repairheights` | | Fix height=0 UTXOs from transaction index |

**`repairutxos`** — Scans forward through blocks via zclassicd RPC, finds missing input UTXOs, fetches and inserts them. Runs live, no restart needed.

```bash
zcl-rpc repairutxos                              # defaults: port 8232, 10000 blocks
zcl-rpc repairutxos 8232 "user:pass" 50000       # custom
```

**`repairheights`** — Fixes UTXOs with height=0 (from LevelDB import) by looking up block heights from the transaction index. Fixes HODL wave calculations.

**`importchainstate`** — Bulk import from LevelDB chainstate. Parallel pipeline (30 decoder threads). Auto-runs `repairheights` after import.

### HODL Wave

| Command | Parameters | Description |
|---------|-----------|-------------|
| `gethodlwave` | | Current UTXO age distribution (10 buckets) |
| `gethodlwaveimage` | | HODL wave heatmap PNG |
| `gethodlwavetimeline` | | Timeline PNG showing UTXO creation dates |
| `gethodlwavechart` | | Stacked-area HODL wave chart PNG |

**`gethodlwave`** — Scans entire UTXO set, returns value and count per age bucket.

```json
{
  "tip_height": 3056897,
  "total_supply_zcl": "10364151.61831153",
  "buckets": [
    { "age": "< 1 day",  "zcl": "469.87",      "utxos": 147,     "pct": 0.00 },
    { "age": "1d - 1w",  "zcl": "152106.25",   "utxos": 197,     "pct": 1.47 },
    { "age": "1w - 1m",  "zcl": "97407.17",    "utxos": 475,     "pct": 0.94 },
    { "age": "1 - 3m",   "zcl": "165744.42",   "utxos": 1323,    "pct": 1.60 },
    { "age": "3 - 6m",   "zcl": "886910.61",   "utxos": 2249,    "pct": 8.56 },
    { "age": "6 - 12m",  "zcl": "374491.09",   "utxos": 68806,   "pct": 3.61 },
    { "age": "1 - 2y",   "zcl": "544212.28",   "utxos": 37288,   "pct": 5.25 },
    { "age": "2 - 3y",   "zcl": "760274.96",   "utxos": 45889,   "pct": 7.34 },
    { "age": "3 - 5y",   "zcl": "1248721.80",  "utxos": 75914,   "pct": 12.05 },
    { "age": "> 5y",     "zcl": "6133813.15",  "utxos": 1122447, "pct": 59.18 }
  ]
}
```

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
| `generate` | `num_blocks` | Generate blocks (regtest only) |

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
| `listtransactions` | `[count] [skip]` | Recent transactions |
| `sendtoaddress` | `addr amount` | Send ZCL |
| `sendmany` | `"" {addr:amount,...}` | Send to multiple addresses |
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
| `z_listreceivedbyaddress` | `address [minconf]` | Received notes |
| `z_exportkey` | `address` | Export spending key |
| `z_exportviewingkey` | `address` | Export viewing key |
| `z_importkey` | `key [rescan] [height]` | Import spending key |

### Wallet Sync & Repair

| Command | Parameters | Description |
|---------|-----------|-------------|
| `rescanblockchain` | `[start] [stop]` | Rescan for wallet transactions |
| `rescanwallet` | | Full wallet rescan |
| `rescanwitnesses` | | Rebuild Sapling witnesses |

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

## CLI Tools

### `--repair` (standalone, no node required)

Scans ahead through zclassicd blocks, inserts missing UTXOs into SQLite. Resets `coins_best_block` after repair so the node doesn't roll back on restart.

```bash
./zclassic23 --repair [num_blocks] [port] [creds]
./zclassic23 --repair 5000 8232 "zcluser:zclpass"
```

### `--importchainstate` (standalone)

```bash
./zclassic23 --importchainstate /path/to/chainstate [db_path]
```

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
| `GET /api/hodl` | Full HODL wave data (10 age buckets with values) |
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
| `/explorer/hodl` | HODL wave chart (9-year history) |
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

After all chunks received, the client computes SHA3-256 over its UTXO set and verifies against the manifest root.

### Block Swarm (BitTorrent-Style)
| Message | Direction | Description |
|---------|-----------|-------------|
| `zblkmanfst` | Both | Block piece manifest (height range, SHA3 piece hashes) |
| `zblkreq` | Client → Server | Request piece by index |
| `zblkdata` | Server → Client | Piece response (128 blocks per piece) |
| `zblkbitmap` | Both | Piece availability bitmap |

Each piece = 128 blocks, SHA3-256 verified. Rarest-first selection. 4-deep pipeline per peer.

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

Enforced in `connect_block`. Mismatch = fatal halt. Verified bit-for-bit against zclassicd.

---

## Quick Start

```bash
# Build
make zclassic23

# Fast bootstrap from zclassicd (same machine)
./zclassic23 -fastsync ~/.zclassic -datadir=~/.zclassic-c23

# Or sync via P2P
./zclassic23 -datadir=~/.zclassic-c23 -addnode=74.50.74.102

# Fix UTXO gaps (if stuck during sync)
zcl-rpc repairutxos                         # live repair from zclassicd
./zclassic23 --repair 5000                   # standalone repair (node stopped)
zcl-rpc repairheights                        # fix HODL wave heights

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
