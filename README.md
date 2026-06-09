# ZClassic23 v0.1.0

Pure C23 full node + decentralized web platform for ZClassic.

One binary. Zero dependencies. 15MB.

## Status Notice

ZClassic23 is work in progress and is currently not syncing properly. Do not use this build as a reliable mainnet node until sync is fixed and the v1 acceptance criteria are met.

## Quick Start

```bash
git clone https://github.com/ArcadiaOS/zclassic23.git
cd zclassic23
make zclassic23    # build (requires only gcc/clang with C23 support)
make test          # run 1500+ tests
```

## Run

```bash
build/bin/zclassic23                              # start node
build/bin/zclassic23 -tor                         # start with .onion hidden service
build/bin/zclassic23 -cold-import=~/.zclassic     # instant sync from legacy data
build/bin/zclassic23 -addnode=74.50.74.102        # connect to seed node
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

## Docs

- [README.md](README.md): quick start and operator overview
- [CLAUDE.md](CLAUDE.md): MCP tool reference, architecture overview, build/test/deploy
- [DEFENSIVE_CODING.md](DEFENSIVE_CODING.md): mandatory coding standards (enforced by `make lint`)
- [docs/SYNC.md](docs/SYNC.md): sync methods, verification layers, self-healing, operator runbook
- [docs/RUNBOOK.md](docs/RUNBOOK.md): symptom-driven troubleshooting
- [docs/ARCHITECTURE_DIAGRAMS.md](docs/ARCHITECTURE_DIAGRAMS.md): boot, services, P2P, wallet diagrams
- [docs/MVP.md](docs/MVP.md): MVP acceptance criteria and readiness score
- [docs/spec/power-node-contract.md](docs/spec/power-node-contract.md): power-node contract surface

```
zclassic23 binary (15MB, statically linked)
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
| `make zclassic23` | Main binary at `build/bin/zclassic23` |
| `make test` | Run 1500+ tests |
| `make zcl-rpc` | Lightweight CLI RPC client at `build/bin/zcl-rpc` |
| `make zcl-nodectl` | Compiled node lifecycle + follow verifier at `build/bin/zcl-nodectl` |
| `make zcl-browser` | GTK Tor-only browser at `build/bin/zcl-browser` |
| `make deploy` | Install systemd user service |
| `make check-restart-follow` | Restart `zclassic23` and verify it catches legacy `zclassicd` tip |

## Project Structure

```
main.c               Entry point (server + CLI dual mode)
app/
  models/             SQLite persistence (ActiveRecord pattern)
  controllers/        RPC handlers (142), HTTP routes, sync, blog, store
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
  test/               1500+ automated tests across 198 files
  [+ 13 more modules]
vendor/               Static libs (secp256k1, leveldb, sqlite3, openssl, tor)
tools/                source for zcl-browser, zcl-rpc, zcl-nodectl, hodl wave tools, utilities
build/bin/            compiled binaries
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

See the RPC tool table in [CLAUDE.md](CLAUDE.md#mcp-server-model-context-protocol) for the full surface (142 RPC methods exposed via MCP plus the raw `zcl_rpc` escape hatch).

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
build/bin/zcl-rpc getblockchaininfo
build/bin/zcl-rpc getblock <hash> [verbosity]
build/bin/zcl-rpc getblockhash <height>

# Wallet
build/bin/zcl-rpc getbalance
build/bin/zcl-rpc sendtoaddress <addr> <amount>
build/bin/zcl-rpc z_sendmany <from> '[{"address":"...","amount":0.1}]'
build/bin/zcl-rpc listunspent

# Mining
build/bin/zcl-rpc getblocktemplate
build/bin/zcl-rpc getnetworkhashps

# Network
build/bin/zcl-rpc getpeerinfo
build/bin/zcl-rpc getnetworkinfo
build/bin/zcl-rpc addnode <ip:port> add

# Diagnostics
build/bin/zcl-rpc healthcheck
build/bin/zcl-rpc eventlog 100
build/bin/zcl-rpc syncstate
build/bin/zcl-rpc downloadstats

# Node control
make zcl-nodectl
build/bin/zcl-nodectl status
build/bin/zcl-nodectl stop
build/bin/zcl-nodectl start
build/bin/zcl-nodectl restart
build/bin/zcl-nodectl verify-follow --restart
```

## Environment variables

A handful of env vars override default behavior. The most commonly
useful for clients/tools:

| Variable | Used by | Effect |
|---|---|---|
| `ZCL_RPCPORT` | `zcl-rpc`, `zclassic-cli` | Override the RPC port to dial (default `18232`; legacy zclassicd uses `8232`). Unparseable values fall back to the default. |
| `ZCL_DATADIR` | `zcl-rpc` | Override the data directory used to locate the `.cookie` file. Default `$HOME/.zclassic-c23`. |
| `ZCL_OFFLINE_REPAIR` | `zclassic23` | Allow `node_db_wipe_utxos()` to drop more than 1,000 rows. Off-line use only. |
| `ZCL_PRUNE_KEEP_BLOCKS` | `zclassic23` | Block-pruning retention depth (default 1000, min 288). |
| `ZCL_RPC_COOKIE_ROTATE_SEC` | `zclassic23` | Cookie rotation interval in seconds (default 86400 = 24h). |

Many other `ZCL_*` env vars exist for niche tuning (see source for the
full set). The ones above are the ones most commonly reached for from
outside the node process.

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

Operator-specific flags (`-externalip` and the seeded `-addnode` list)
live in `~/.config/zclassic23/env`, not in the tracked systemd unit.
Copy `deploy/zclassic23.env.example` to that path and edit for your
deployment; a fresh clone without the env file starts cleanly against
DNS seeds.

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

Copyright 2026 Rhett Creighton

Licensed under the Apache License, Version 2.0 — see [`LICENSE`](LICENSE)
for the full text. Upstream copyright notices from inherited code
(Bitcoin Core, Zcash, zclassicd) and vendored dependencies (Tor,
SQLite, secp256k1, LevelDB, dcrdex) are preserved in [`NOTICE`](NOTICE).
Architectural concept attributions (Erigon, mcp-language-server, etc.)
are tracked in [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
