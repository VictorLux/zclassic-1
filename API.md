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
curl -u rpcuser:rpcpassword -d '{"method":"getinfo","params":[]}' http://localhost:18232/
```

Cookie file: `~/.zclassic-c23/.cookie`

---

### Blockchain

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getblockchaininfo` | | Chain state: height, difficulty, best block, verification progress |
| `getblockcount` | | Current block height |
| `getbestblockhash` | | Hash of chain tip |
| `getblockhash` | `height` | Block hash at given height |
| `getblockheader` | `hash_or_height [verbose]` | Block header (JSON if verbose=true, hex otherwise) |
| `getblock` | `hash_or_height [verbosity]` | Block data (0=hex, 1=JSON, 2=JSON+tx details) |
| `getdifficulty` | | Current proof-of-work difficulty |
| `getmempoolinfo` | | Mempool size, bytes, usage |
| `gettxoutsetinfo` | | UTXO set statistics (count, total value, hash) |
| `invalidateblock` | `hash` | Mark block as invalid |
| `reconsiderblock` | `hash` | Reconsider previously invalidated block |
| `getnetworkhashps` | `[blocks] [height]` | Estimated network hash rate |
| `rebuildutxoindex` | | Rebuild UTXO index from chain |

### Chain Inspection

| Command | Parameters | Description |
|---------|-----------|-------------|
| `chainview` | | Chain view details |
| `chainstats` | | Chain statistics summary |
| `gettxdetail` | `txid` | Detailed transaction information |
| `saplingtreeinfo` | | Sapling merkle tree state |
| `scancommitments` | | Scan Sapling note commitments |
| `verifychainroots` | | Verify integrity of all chain roots |
| `indexlegacy` | `path` | Index legacy zclassicd block files |

### HODL Wave Analysis

| Command | Parameters | Description |
|---------|-----------|-------------|
| `gethodlwave` | | HODL wave data (UTXO age distribution) |
| `gethodlwaveimage` | | HODL wave as SVG image |
| `gethodlwavetimeline` | | HODL wave timeline data |
| `gethodlwavechart` | | HODL wave chart data |
| `hodltimeseries` | | HODL time series (historical) |

### Raw Transactions

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getrawtransaction` | `txid [verbose]` | Raw transaction (hex or JSON) |
| `decoderawtransaction` | `hex` | Decode raw transaction hex to JSON |
| `createrawtransaction` | `[inputs] [outputs]` | Create unsigned raw transaction |
| `signrawtransaction` | `hex [prevtxs] [privkeys]` | Sign raw transaction inputs |
| `sendrawtransaction` | `hex` | Broadcast signed raw transaction |

### Mining

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getmininginfo` | | Mining status and difficulty |
| `getblocktemplate` | `[params]` | Block template for miners |
| `submitblock` | `hex` | Submit solved block |
| `getblocksubsidy` | `[height]` | Block reward at height |
| `generate` | `count` | Generate blocks (regtest only) |

### Network

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getnetworkinfo` | | Network state (version, connections, warnings) |
| `getpeerinfo` | | Connected peer details (addr, version, latency, misbehavior) |
| `getconnectioncount` | | Number of connected peers |
| `addnode` | `ip:port add\|remove\|onetry` | Manage peer connections |
| `ping` | | Ping all peers, measure RTT |

### Wallet (Transparent)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `getbalance` | `[account] [minconf]` | Wallet balance |
| `getunconfirmedbalance` | | Unconfirmed balance |
| `getwalletinfo` | | Wallet status |
| `getnewaddress` | `[account]` | Generate new t-address |
| `listunspent` | `[minconf] [maxconf] [addrs]` | List UTXOs |
| `sendtoaddress` | `addr amount [comment]` | Send ZCL |
| `sendmany` | `account {addr:amount,...}` | Send to multiple addresses |
| `dumpprivkey` | `address` | Export private key (WIF) |
| `importprivkey` | `wif [label] [rescan]` | Import private key |
| `listtransactions` | `[account] [count] [from]` | Transaction history |
| `gettransaction` | `txid` | Transaction details |
| `validateaddress` | `address` | Validate address format |
| `createmultisig` | `nrequired [keys]` | Create multisig address |
| `addmultisigaddress` | `nrequired [keys]` | Add multisig to wallet |
| `keypoolrefill` | `[size]` | Refill key pool |

### Wallet (Shielded / Sapling)

| Command | Parameters | Description |
|---------|-----------|-------------|
| `z_getnewaddress` | | Generate new z-address |
| `z_listaddresses` | | List all z-addresses |
| `z_getbalance` | `address [minconf]` | Shielded address balance |
| `z_gettotalbalance` | `[minconf]` | Total balance (t + z) |
| `z_sendmany` | `from [{addr, amount, memo}]` | Send from z-address |
| `z_listunspent` | `[minconf] [maxconf] [addrs]` | List shielded UTXOs |
| `z_listreceivedbyaddress` | `address [minconf]` | Notes received by address |
| `z_exportkey` | `address` | Export spending key |
| `z_importkey` | `key [rescan] [height]` | Import spending key |
| `z_exportviewingkey` | `address` | Export viewing key |
| `z_getmemo` | `txid` | Get memo field |
| `z_listallnotes` | | List all Sapling notes |

### Wallet Diagnostics

| Command | Parameters | Description |
|---------|-----------|-------------|
| `walletaudit` | | Audit wallet integrity |
| `walletledger` | | Full wallet ledger |
| `getwalletaccounting` | | Accounting summary |
| `getbalanceflow` | | Balance flow analysis |
| `diagnoseutxos` | | UTXO diagnostic report |
| `reconcilewalletutxos` | | Reconcile wallet vs chain |
| `purgephantomutxos` | | Remove phantom UTXOs |
| `traceutxo` | `txid vout` | Trace UTXO origin |
| `getchaincoins` | | Chain UTXO coins |
| `listwalletkeys` | | List all wallet keys |
| `listwallettxdetail` | | Detailed transaction list |
| `removestalletxs` | | Remove stale transactions |
| `scanblockfiles` | | Scan block files for UTXOs |
| `reindexdb` | | Reindex wallet database |
| `importlegacy` | `path` | Import from legacy wallet |
| `db_info` | | Database diagnostics |

### Wallet Sync

| Command | Parameters | Description |
|---------|-----------|-------------|
| `rescanblockchain` | `[start] [stop]` | Rescan blockchain for wallet txs |
| `rescanwallet` | | Rescan wallet |
| `rescanwitnesses` | | Rescan Sapling witnesses |
| `replaywalletfromchain` | | Full wallet replay |
| `syncwalletfromdb` | | Sync wallet from SQLite |
| `coinanalysis` | | Analyze coin history |

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

---

## REST API

Base URL: `https://zclnet.net` (or `http://localhost:18232`)

All endpoints return JSON with `Access-Control-Allow-Origin: *`.

### `GET /api/blocks`

Latest 25 blocks.

```json
[
  {
    "hash": "0000000...",
    "height": 3000000,
    "time": 1710000000,
    "num_tx": 2,
    "difficulty": 12345.67
  }
]
```

### `GET /api/block/:id`

Block by height (integer) or hash (hex string).

```json
{
  "hash": "0000000...",
  "height": 3000000,
  "time": 1710000000,
  "bits": "1d00ffff",
  "num_tx": 2,
  "tx": ["txid1", "txid2"],
  "sapling_value": 0
}
```

### `GET /api/tx/:txid`

Transaction detail.

```json
{
  "txid": "abc123...",
  "block_hash": "0000000...",
  "block_height": 3000000,
  "vin": [...],
  "vout": [...],
  "is_coinbase": false,
  "value_balance": 0
}
```

### `GET /api/address/:addr`

Address balance and UTXOs.

```json
{
  "address": "t1...",
  "balance": 98000000,
  "utxo_count": 3,
  "utxos": [
    {"txid": "...", "vout": 0, "value": 50000000, "height": 2999000}
  ]
}
```

### `GET /api/stats`

Network statistics.

```json
{
  "height": 3000000,
  "difficulty": 12345.67,
  "hashrate": 1234567890,
  "supply": 2062500000000000,
  "connections": 8,
  "mempool_size": 5
}
```

### `GET /api/stats/deep`

Extended statistics (SQLite-backed).

```json
{
  "height": 3000000,
  "total_transactions": 5000000,
  "total_addresses": 200000,
  "shielded_supply": 100000000000,
  "zslp_tokens": 15,
  "utxo_count": 1600000
}
```

### `GET /api/supply`

Plain-text circulating supply (CoinGecko-compatible).

```
20625000.00000000
```

### `GET /api/hodl`

HODL wave data — UTXO age distribution for chart rendering.

```json
{
  "total_supply": 2062500000000000,
  "bands": [
    {"label": "<1d", "value": 1000000000},
    {"label": "1d-1w", "value": 5000000000},
    {"label": "1w-1m", "value": 20000000000}
  ]
}
```

### `GET /api/events?count=N`

Last N events from the lock-free ring buffer (default 100, max 65536).

```json
[
  {
    "seq": 12345,
    "type": "BLOCK_CONNECTED",
    "timestamp": 1710000000,
    "payload": "height=3000000"
  }
]
```

### `GET /api/health`

Health check. Returns HTTP 200 if healthy, 503 if not.

```json
{"status": "pass", "height": 3000000, "connections": 8}
```

### `GET /api/syncstate`

Sync state machine state.

### `GET /api/downloadstats`

Download manager stats (in-flight, queued, timeouts).

### `GET /api/factoids`

Full historian factoids with SHA3 receipts.

---

## Block Explorer Routes

All routes return HTML. CSS is customizable via `{datadir}/explorer/style.css`.

| Route | Description |
|-------|-------------|
| `GET /explorer` | Dashboard: latest blocks, mempool stats, network info |
| `GET /explorer/block/:id` | Block detail (by height or hash) |
| `GET /explorer/tx/:txid` | Transaction: inputs, outputs, shielded, ZSLP |
| `GET /explorer/address/:addr` | Address: balance, UTXO list |
| `GET /explorer/stats` | SVG charts with CSS tab controls (24h/7d/30d/1y/all) |
| `GET /explorer/hodl` | 9-year HODL wave chart (real UTXO data) |
| `GET /explorer/tokens` | ZSLP token scanner |
| `GET /explorer/factoids` | Historian factoids (13 sections, SHA3 receipts) |
| `GET /explorer/search?q=` | Smart search (height, hash, txid, address) |
| `GET /explorer/style.css` | Custom CSS |
| `GET /explorer/favicon.png` | ZClassic logo |

## Store (Tor-Only)

Decentralized commerce over `.onion`.

| Route | Method | Description |
|-------|--------|-------------|
| `/store` | GET | Product listing |
| `/store/product/:id` | GET | Product detail + payment form |
| `/store/buy/:id` | POST | Create order (returns z-address for payment) |
| `/store/order/:id` | GET | Payment status (pending/paid/minted) |
| `/store/access` | GET | Token-gated content (`addr=` `token=` params) |

Background thread polls pending orders every 30s. Auto-mints ZSLP tokens when shielded payment confirms.

## Error Handling

### RPC Errors

```json
{
  "result": null,
  "error": {
    "code": -1,
    "message": "Method not found"
  },
  "id": 1
}
```

| Code | Meaning |
|------|---------|
| -1 | General error |
| -3 | Invalid type |
| -5 | Invalid address |
| -6 | Insufficient funds |
| -8 | Invalid parameter |
| -25 | Transaction already in chain |
| -26 | Transaction rejected |

### HTTP Status Codes (REST API)

| Code | Meaning |
|------|---------|
| 200 | Success |
| 404 | Resource not found |
| 429 | Rate limited (onion service: 100 req/s) |
| 500 | Internal error |
| 503 | Node not healthy / syncing |

## Rate Limits

| Interface | Limit |
|-----------|-------|
| Onion service | 100 requests/second global |
| Fast sync | 5000 chunks/IP/hour + 20-bit PoW |
| RPC | No limit (localhost only by default) |

## Security

- RPC is localhost-only by default. Use `-rpcallowip=` to open.
- Cookie authentication regenerated each start: `~/.zclassic-c23/.cookie`
- All REST API responses include CORS headers.
- HTML views escape all DB-sourced values.
- P2P messages capped at 2MB. Compact size bounds checked.
- Peer misbehavior scoring: auto-ban at 100 points (24h).
