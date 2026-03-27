# ZClassic23 v0.1.0

## Vision
A decentralized internet platform. Every node is a full ZClassic node, a Tor hidden service, a web application server, and a peer discovery engine. One binary. Pure C23. Zero dependencies.

## Build
```bash
make zclassic23    # 26MB binary, zero system deps
make test          # 1066+ tests
make zcl-browser   # GTK Tor-only browser
make zcl-rpc       # CLI RPC client
```

## Documentation
- **README.md** — Quick start, architecture overview, build targets
- **API.md** — Complete API reference (95 RPC commands, REST endpoints, explorer routes)
- **VISION.md** — Project philosophy and roadmap
- **CLAUDE.md** — This file: project structure, protocol, security, design decisions

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

ZCL23 extension — UTXO snapshot (5): zsnapshot, zsnapreq, zsnapdata, zsnapend, zmanifest
ZCL23 extension — Block swarm (4): zblkmanfst, zblkreq, zblkdata, zblkbitmap

### Fast Sync (UTXO Snapshot)
```
Requester (behind)          Server (ahead)
   ← zsnapshot offer (height, SHA3 root, UTXO count)
   → zsnapreq (accept)
   ← zsnapdata (500 UTXOs per chunk)
   ← zsnapdata ...
   ← zsnapend
   compute SHA3 → verify against offer → switch to delta block sync
```
SHA3 verification after all chunks: `SHA3 UTXO verification: PASSED/FAILED`

### Block Swarm (BitTorrent-Style)
```
Both peers exchange:         zblkmanfst (height range, SHA3 piece hashes, merkle root)
Requester (behind):          zblkreq (piece index)
Server (ahead):              zblkdata (128 block hashes per piece)
Both peers:                  zblkbitmap (availability bitmap)
```
Each piece = 128 blocks, SHA3-256 verified. Rarest-first selection.
4-deep pipeline per peer. Endgame mode for last 8 pieces.
3000 blocks behind → 24 pieces → synced in seconds with multiple peers.

Defense: 20-bit PoW per snapshot, 5000 chunks/IP/hour, misbehavior scoring.

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

### ZSLP Token RPCs
```
zslp_createtoken "TICK" "Name" decimals supply   Create GENESIS tx
zslp_send "token_id" "address" amount             Send tokens
zslp_mint "token_id" "address" amount             Mint new tokens
zslp_balance "token_id" "address"                  Query balance
```
On-chain broadcast: builds OP_RETURN + dust outputs, signs via wallet,
commits to mempool. Falls back to SQLite-only tracking when wallet
unavailable (test mode).

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
Started with 1.00000000 ZCL. Balance changes with each shield op (0.0001 fee each).
zclassicd `z_gettotalbalance` is the ONLY source of truth.
Auto-sync: pulse handler syncs wallet_utxos + wallet_sapling_notes when dirty.
Shield delegation: C23 GUI calls zclassicd `z_sendmany` on port 8232.
Auto-sync: after shield/send, `sync_wallet_from_zclassicd()` refreshes wallet_utxos
and wallet_sapling_notes from zclassicd `listunspent` + `z_listunspent`.
Balance source: `wallet_utxos WHERE spent_txid IS NULL` + `wallet_sapling_notes`.

## Network Seeds
```
Clearnet:  74.50.74.102 (rhett.dev), 205.209.104.118, 140.174.189.3
DNS:       dnsseed.zclnet.net, dnsseed.zslp.org, mainnet.zclassic.org
Onion:     zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion
```

## Block Explorer & REST API
Live at https://zclnet.net/explorer — served by zclassic23 itself (TLS on port 443).

### Explorer Routes
```
/explorer              Dashboard (latest blocks, mempool stats)
/explorer/block/:id    Block detail (by height or hash)
/explorer/tx/:txid     Transaction detail (inputs, outputs, shielded, ZSLP)
/explorer/address/:a   Address balance + UTXOs
/explorer/stats        SVG charts with CSS-only tab controls (24h/7d/30d/1y/all)
/explorer/hodl         9-year HODL wave chart (from genesis, real UTXO data)
/explorer/tokens       ZSLP token scanner
/explorer/factoids     Historian factoids (13 sections, SHA3 receipts)
/explorer/search?q=    Smart search (height, hash, txid, address)
/explorer/style.css    Customizable CSS (from {datadir}/explorer/style.css)
/explorer/favicon.png  ZClassic logo
```

### REST API
```
/api/blocks            Latest 25 blocks (JSON)
/api/block/:id         Block detail by height or hash
/api/tx/:txid          Transaction detail
/api/address/:addr     Address balance + UTXOs
/api/stats             Network stats (height, difficulty, hashrate, supply)
/api/stats/deep        Deep stats (SQLite-backed, shielded, ZSLP, addresses)
/api/supply            Circulating supply (plain number, CoinGecko format)
/api/hodl              HODL wave data
/api/factoids          Full historian factoids (JSON, SHA3 receipts)
/api/events?count=N    Event log (lock-free ring buffer, JSON)
/api/syncstate         Sync state machine state
/api/downloadstats     Download manager stats (in-flight, queued, timeouts)
```
All endpoints return JSON with CORS headers (`Access-Control-Allow-Origin: *`).

### Fast Sync
```bash
zcl-rpc stop                           # stop zclassicd
./zclassic23 -fastsync ~/.zclassic     # instant snapshot (symlinks)
# Node at tip in 34 seconds total:
#   Fastsync: 0s (symlinks on same FS)
#   Block index load: 7s
#   SQLite cache: 27s
#   P2P delta: ~10 blocks
```

### Performance
- SHA-256: 1,045 MB/s (SHA-NI hardware, 4.2x over portable)
- Chain indexer: ~6,000 blocks/sec, 3M blocks in ~9 minutes
- Fastsync: 0s symlinks, 34s total to chain tip (3M+ blocks)
- HODL wave: precomputed in background, served from cache in <10ms
- Stats charts: precomputed with 5-min cache, CSS-only tab switching
- SQLite: WAL mode, 256MB mmap, 256MB cache

### Event State Machine & Monitoring
Every P2P event, state transition, and validation step is logged in a
65536-event lock-free ring buffer. On crash, last 200 events dump to stderr.
```
zcl-rpc healthcheck                    # pass/fail health status
zcl-rpc eventlog 100                   # last 100 events (JSON)
zcl-rpc syncstate                      # sync state machine
zcl-rpc downloadstats                  # download manager stats
zcl-rpc getpeerinfo                    # peer states + misbehavior
zcl-rpc coinsinfo                      # UTXO cache diagnostics
curl localhost:18232/api/health         # HTTP 200/503 health check
```

Self-monitoring: tip-stale watchdog (auto-re-requests if no block for 10m),
adaptive peer reconnection (30s recovery from 0 peers), peer health warnings.

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
# First time (one sudo, then never again):
sudo deploy/setup.sh

# Every deploy:
make zclassic23 && make deploy
systemctl --user status zclassic23
tail -f ~/.zclassic-c23/node.log
```
SSL certs at `{datadir}/ssl/fullchain.pem` + `privkey.pem`.
CSS template at `{datadir}/explorer/style.css` (live-editable).

## SHA3 Data Integrity

### Hardcoded UTXO Checkpoint (height 3,056,758)
```
SHA3:    00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0
UTXOs:   1,354,771
Supply:  10,364,138.33747381 ZCL
Block:   000002979090fba9da6cdc140d050245c1b637480609510922662407855bd653
```
**Mandatory enforcement**: `connect_block` verifies at this height. Mismatch = fatal halt.
Verified bit-for-bit against zclassicd reference implementation.

### RPCs
- `getutxocommitment` — SHA3-256 over canonical UTXO set (~1s)
- `getdataintegrity` — SHA3-256 over ALL 12 consensus tables + master hash
- `verifycheckpoint` — compare against hardcoded SHA3 checkpoint
- `getmmrroot` — MMR root over block hashes (O(log n) proofs)

### Merkle Mountain Range (MMR)
SHA3-256 with domain separation: leaf=0x00, internal=0x01, root=0x02.
Built from 3M blocks in 2s at startup. Updated per block. Persisted on shutdown.
Enables O(log n) inclusion proofs between power nodes.

### Fixing UTXO Gaps
```bash
zcl-rpc importchainstate ~/.zclassic-c23/chainstate  # reimport from LevelDB
zcl-rpc verifycheckpoint                               # verify SHA3 matches
```

## Quality
Q = Clarity × Reliability × Performance × TestCoverage × UserImpact.
Every commit must raise Q.

## License
Copyright 2026 Rhett Creighton — Apache License 2.0
