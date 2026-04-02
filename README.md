# ZClassic23 v0.1.0

Pure C23 full node + decentralized web platform for ZClassic.

One binary. Zero dependencies. 26MB.

## Quick Start

```bash
git clone https://github.com/ArcadiaOS/zclassic23.git
cd zclassic23
make zclassic23    # build (requires only gcc/clang with C23 support)
make test          # run 1066+ tests
```

## Run

```bash
./zclassic23                              # start node
./zclassic23 -tor                         # start with .onion hidden service
./zclassic23 -fastsync ~/.zclassic        # instant sync from legacy data
./zclassic23 -addnode=74.50.74.102        # connect to seed node
```

Data directory: `~/.zclassic-c23/`

## What Is This?

ZClassic23 is a complete rewrite of zclassicd in pure C23. Every node is simultaneously:

- **A full ZClassic node** — Bug-for-bug compatible with legacy zclassicd. Syncs headers, validates blocks, relays transactions, serves peers on the ZClassic P2P network (port 8033).

- **A fast sync server** — New nodes transfer the entire UTXO set (~1.6M entries) in ~60 seconds via the `NODE_ZCL23` protocol extension.

- **A Tor hidden service** — Tor is compiled into the binary. With `-tor`, the node creates a `.onion` address and serves web pages over Tor circuits. No ports exposed to clearnet.

- **A decentralized web platform** — MVC framework (models/controllers/views in C23) for building database-backed web apps served over `.onion`.

- **A block explorer** — Full explorer with charts, HODL wave analysis, token scanner, and REST API at `/explorer`.

- **A ZSLP token platform** — On-chain tokens for commerce, peer discovery, and decentralized DNS.

## Architecture

Detailed refactor target: [ARCHITECTURE.md](ARCHITECTURE.md)
Execution progress checklist: [REFACTOR_CHECKLIST.md](REFACTOR_CHECKLIST.md)

```
zclassic23 binary (26MB, statically linked)
├── Full node         P2P port 8033, RPC port 18232
├── Tor (embedded)    .onion hosting via dynhost (in-process, no SOCKS)
├── MVC framework     Models (SQLite), Controllers (C23), Views (HTML/JSON)
├── Block explorer    /explorer routes + /api REST endpoints
├── Fast sync         UTXO snapshot transfer between zclassic23 nodes
├── ZSLP registry     On-chain .onion peer discovery + token commerce
└── Browser           zcl-browser (GTK WebKit, Tor-only)
```

## Build Targets

| Target | Description |
|--------|-------------|
| `make zclassic23` | Main binary (26MB, zero system deps) |
| `make test` | Run 1066+ tests |
| `make zcl-rpc` | Lightweight CLI RPC client |
| `make zcl-nodectl` | Compiled node lifecycle + follow verifier |
| `make zcl-browser` | GTK Tor-only browser |
| `make deploy` | Install systemd user service |
| `make check-restart-follow` | Restart `zclassic23` and verify it catches legacy `zclassicd` tip |

## Project Structure

```
main.c               Entry point (server + CLI dual mode)
app/
  models/             SQLite persistence (ActiveRecord pattern)
  controllers/        RPC handlers (27), HTTP routes, sync, blog, store
  views/              JSON + HTML serializers
config/               Boot, shutdown, global state
lib/
  crypto/             SHA-256 (SHA-NI), Equihash, Ed25519, Blake2, AES
  chain/              Chain params, PoW, checkpoints, subsidy
  net/                P2P networking, Tor integration, fast sync, addrman
  validation/         Block/tx validation, process_block, sighash
  sapling/            zk-SNARK shielded transactions (Groth16, JubJub)
  wallet/             Keystore, wallet, sapling key management
  storage/            LevelDB (block index, coins), disk I/O
  script/             Script interpreter, standard scripts, sigcache
  rpc/                HTTP server, RPC protocol
  coins/              UTXO set, coin commitment tracking
  test/               1066+ automated tests
  [+ 13 more modules]
vendor/               Static libs (secp256k1, leveldb, sqlite3, openssl, tor)
tools/                zcl-browser, zcl-rpc, zcl-nodectl, hodl wave tools, utilities
deploy/               systemd service, setup script
```

## REST API

All endpoints return JSON with CORS headers.

| Endpoint | Description |
|----------|-------------|
| `GET /api/blocks` | Latest 25 blocks |
| `GET /api/block/:id` | Block by height or hash |
| `GET /api/tx/:txid` | Transaction detail |
| `GET /api/address/:addr` | Address balance + UTXOs |
| `GET /api/stats` | Network stats (height, difficulty, hashrate, supply) |
| `GET /api/stats/deep` | Extended stats (shielded, ZSLP, addresses) |
| `GET /api/supply` | Circulating supply (plain number) |
| `GET /api/hodl` | HODL wave data (9-year holding analysis) |
| `GET /api/events?count=N` | Event log (lock-free ring buffer) |
| `GET /api/health` | Health check (HTTP 200/503) |

See [API.md](API.md) for complete documentation including 83 RPC commands.

## Block Explorer

Live at [zclnet.net/explorer](https://zclnet.net/explorer) — served by zclassic23 itself.

| Route | Description |
|-------|-------------|
| `/explorer` | Dashboard (latest blocks, mempool) |
| `/explorer/block/:id` | Block detail |
| `/explorer/tx/:txid` | Transaction detail (inputs, outputs, shielded) |
| `/explorer/address/:a` | Address balance + UTXOs |
| `/explorer/stats` | SVG charts (24h/7d/30d/1y/all) |
| `/explorer/hodl` | 9-year HODL wave chart |
| `/explorer/tokens` | ZSLP token scanner |

## RPC Quick Reference

```bash
# Blockchain
zcl-rpc getblockchaininfo
zcl-rpc getblock <hash> [verbosity]
zcl-rpc getblockhash <height>

# Wallet
zcl-rpc getbalance
zcl-rpc sendtoaddress <addr> <amount>
zcl-rpc z_sendmany <from> '[{"address":"...","amount":0.1}]'
zcl-rpc listunspent

# Mining
zcl-rpc getblocktemplate
zcl-rpc getnetworkhashps

# Network
zcl-rpc getpeerinfo
zcl-rpc getnetworkinfo
zcl-rpc addnode <ip:port> add

# Diagnostics
zcl-rpc healthcheck
zcl-rpc eventlog 100
zcl-rpc syncstate
zcl-rpc downloadstats

# Node control
make zcl-nodectl
./zcl-nodectl status
./zcl-nodectl stop
./zcl-nodectl start
./zcl-nodectl restart
./zcl-nodectl verify-follow --restart
```

## Deployment

```bash
# One-time setup (grants port capabilities, enables linger)
sudo deploy/setup.sh

# Build and deploy
make zclassic23 && make deploy
systemctl --user status zclassic23
tail -f ~/.zclassic-c23/node.log

# Validate restart + legacy tip following
make zcl-nodectl
make check-restart-follow
```

## Performance

| Metric | Value |
|--------|-------|
| SHA-256 | 1,045 MB/s (SHA-NI hardware) |
| Chain indexer | ~6,000 blocks/sec |
| Full sync | ~5 min for 3M blocks (from legacy data) |
| Fast sync | 34s to chain tip (UTXO snapshot) |
| HODL wave | <10ms (precomputed cache) |

## Network

| Service | Port | Description |
|---------|------|-------------|
| P2P | 8033 | Blockchain data + fast sync |
| RPC | 18232 | JSON-RPC (cookie or password auth) |
| Tor | .onion | Blog, web apps, store (no clearnet) |
| Explorer | 443 (TLS) | Block explorer + REST API |

**Seeds**: `74.50.74.102`, `205.209.104.118`, `140.174.189.3`
**DNS**: `dnsseed.zclnet.net`, `dnsseed.zslp.org`, `mainnet.zclassic.org`

## License

Copyright 2026 Rhett Creighton — Apache License 2.0
