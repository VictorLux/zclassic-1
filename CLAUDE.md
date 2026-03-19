# ZClassic23

## Vision
A decentralized internet platform. Every node is a full ZClassic node, a Tor hidden service, a web application server, and a peer discovery engine. One binary. Pure C23. Zero dependencies.

## Build
```bash
make zclassic23    # 26MB binary, zero system deps
make test          # 869+ tests
make zcl-browser   # GTK Tor-only browser
make zcl-rpc       # CLI RPC client
```

## Project Structure
```
app/
  models/         ActiveRecord (SQLite): block, utxo, wallet_key, wallet_tx, peer
  controllers/    RPC handlers: wallet, blockchain, transaction, mining, network,
                  blog, onion_service, sync, chain_inspect, wallet_shielded
  views/          JSON + HTML serializers
config/           boot.c (app_init/shutdown), boot.h
lib/
  crypto/         sha256, equihash, ed25519, blake2, chacha20, aes256
  chain/          chainparams, pow, checkpoints, subsidy
  net/            connman, msgprocessor, p2p_message, fast_sync, tor_integration,
                  onion_service, addrman, nat
  validation/     check_block, connect_block, process_block, sighash
  wallet/         keystore, wallet, sapling_keys
  storage/        LevelDB (block_index_db, coins_db, disk_block_io)
  sapling/        jubjub, groth16, incremental_merkle_tree, note_encryption, slp
  primitives/     block, transaction serialization
  script/         interpreter, standard, sigcache
  rpc/            httpserver, server, protocol
  [+ bloom, coins, consensus, core, encoding, json, keys, metrics,
     mining, policy, support, util, test]
vendor/
  tor/            Our Tor fork (compiled into binary, dynhost)
  lib/            Static archives: secp256k1, leveldb, sqlite3, openssl,
                  libevent, zlib, tor
  include/        Headers: secp256k1.h, leveldb/, sqlite3.h
tools/
  zcl-browser.c   GTK WebKit .onion browser
  zcl-rpc.c        Pure C23 RPC client
  zcl              Bash CLI wrapper
```

## Protocol

### Services (version message)
```
NODE_NETWORK  (1)     Standard full node
NODE_BLOOM    (4)     Bloom filter support (required by ZClassic peers)
NODE_ZCL23    (1024)  zclassic23 fast sync + .onion hosting
```
Legacy peers see `NODE_NETWORK | NODE_BLOOM = 5` (same as zclassicd).
ZCL23 peers detected via subversion string `/ZClassic-C23:x.y.z/`.

### P2P Messages
Legacy (18): version, verack, ping, pong, addr, inv, getdata, getblocks,
getheaders, block, tx, headers, getaddr, mempool, notfound, sendheaders,
reject, feefilter

ZCL23 extension (4): zsnapshot, zsnapreq, zsnapdata, zsnapend

### Fast Sync
```
Requester (behind)          Server (ahead)
   ← zsnapshot offer (height, UTXO count, root hash)
   → zsnapreq (accept)
   ← zsnapdata (500 UTXOs per chunk)
   ← zsnapdata ...
   ← zsnapend
   verify UTXO root → switch to delta block sync
```
Defense: 20-bit PoW per request, 5000 chunks/IP/hour rate limit.

### Clearnet vs Tor
| Transport | Content | Auth |
|-----------|---------|------|
| P2P 8033/18033 | Blockchain data + fast sync | None (PoW for sync) |
| RPC 18232 | JSON-RPC | Cookie or rpcuser:rpcpassword |
| Tor .onion | Blog, web apps, directory | Tor anonymity |

No web content over clearnet. Blog and apps are Tor-only.

## Security

### Peer Scoring
Per-peer misbehavior counter. Auto-ban at 100 points (24h):
- Invalid block/tx: 10-100 points depending on severity
- PoW required for snapshot requests (20-bit hashcash)
- Rate limit: 5000 fast sync chunks/IP/hour

### P2P Resilience
- Addrman feedback: failed connections penalized, successful handshakes
  promoted to tried bucket
- Outbound diversity: max 2 peers per /16 subnet (eclipse defense)
- Ping/pong latency tracking per peer

### Input Sanitization
- HTML escaping on all DB-sourced values in store/onion views
- ZSLP input validation: ticker 1-10 alphanum, decimals 0-8, amount>0
- Address validation (alphanumeric) on store POST inputs
- Blog path traversal protection (realpath canonicalization)
- ZSLP mint overflow check (INT64_MAX cap)
- 2MB max P2P message size, compact size bounds checking

### Rate Limiting
- Onion service: 100 requests/second global (429 Too Many Requests)
- Fast sync: 5000 chunks/IP/hour + 20-bit PoW per request

### Store Commerce (Tor-only)
```
GET  /store              Product listing (3 demo products)
GET  /store/product/:id  Detail + payment form
POST /store/buy/:id      Create order → z-address for payment
GET  /store/order/:id    Payment status (pending/paid/minted)
GET  /store/access       Token-gated content (addr= token= params)
```
Background thread polls pending orders every 30s, auto-mints ZSLP
tokens when shielded payment confirms.

## Key Design

### Instant Operations (no rescans)
- `importprivkey`: Hash160(pubkey) → SQLite UTXO index → 11ms
- `z_sendmany`: witnesses maintained incrementally by connect_block
- `signrawtransaction`: SQLite UTXO lookup before LevelDB fallback
- `listunspent`: cross-references global UTXO index for correct heights

### Startup
- Warm restart: 4.3s (flat block_index mmap, 406MB)
- ZK keys: loaded in background thread (parallel with block index)
- Snapshot offer: pre-computed in background thread

### Wallet
All funds at `t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn` (0.98 ZCL).

## Network Seeds
```
Clearnet:  74.50.74.102 (rhett.dev), 205.209.104.118, 140.174.189.3
DNS:       dnsseed.zclnet.net, dnsseed.zslp.org, mainnet.zclassic.org
Onion:     zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion
```

## Block Explorer & REST API
Live at https://zclnet.net/explorer — served by zclassic23 itself (TLS on port 443).

### Routes
```
/explorer              Dashboard (latest blocks, stats)
/explorer/block/:id    Block detail (by height or hash)
/explorer/tx/:txid     Transaction detail (inputs, outputs, shielded, ZSLP)
/explorer/address/:a   Address balance + UTXOs
/explorer/stats        SVG charts: difficulty, hashrate, block times, HODL waves
/explorer/tokens       ZSLP token scanner
/explorer/search?q=    Smart search (height, hash, txid, address)
/explorer/style.css    Customizable CSS (from {datadir}/explorer/style.css)
```

### SQLite Database (node.db)
All blockchain data is indexed in SQLite for instant queries. Schema v5:
```
blocks          — hash(PK), height, time, bits, num_tx, sapling_value, file_pos
transactions    — txid(PK), block_hash, block_height, is_coinbase
utxos           — (txid,vout)(PK), value, address_hash, height, script_type
addresses       — address_hash(PK), balance, utxo_count, first/last_seen
chain_stats     — height(PK), difficulty, tx_count, supply, shielded_supply
zslp_tokens     — token_id(PK), ticker, name, decimals, genesis_height
```
Key indexes: `idx_utxo_address`, `idx_utxo_height_value`, `idx_blocks_height_all`, `idx_addr_balance`

### Populating the Index
```bash
# Import from legacy zclassicd block files (works while zclassicd runs):
zcl-rpc indexlegacy /path/to/.zclassic
# Indexes ~10,000 blocks/sec, ~5 minutes for 3M blocks
```

### Deployment
```bash
make zclassic23 && systemctl restart zclassic23   # no sudo needed
systemctl status zclassic23                        # check status
tail -f ~/.zclassic-c23/node.log                   # watch logs
```
SSL certs at `{datadir}/ssl/fullchain.pem` + `privkey.pem`.
CSS template at `{datadir}/explorer/style.css` (live-editable).

## Quality
Q = Clarity × Reliability × Performance × TestCoverage × UserImpact.
Every commit must raise Q.

## License
Copyright 2026 Rhett Creighton — Apache License 2.0
