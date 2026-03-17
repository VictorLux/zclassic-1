# ZClassic23

Pure C23 full node + decentralized web platform for ZClassic.

## Build

```bash
git clone https://github.com/RhettCreighton/zclassic-c.git
cd zclassic-c
make zclassic23
```

One command. Zero system dependencies beyond libc. 26MB binary.

## Run

```bash
./zclassic23                              # start node
./zclassic23 -tor                         # start with .onion service
./zclassic23 -addnode=127.0.0.1:8033      # connect to local zclassicd
```

Data directory: `~/.zclassic-c23/`

## What Is This?

ZClassic23 is a complete rewrite of zclassicd in pure C23. It is:

- **A full ZClassic node** — bug-for-bug compatible with legacy zclassicd (MagicBean). Syncs headers, validates blocks, relays transactions, serves peers on the ZClassic P2P network.

- **A fast sync server** — new zclassic23 nodes transfer the entire UTXO set (~1.6M entries) in about 60 seconds via the `NODE_ZCL23` protocol extension.

- **A Tor hidden service** — Tor is compiled into the binary. With `-tor`, the node creates a `.onion` address and serves web pages directly over Tor circuits. No ports exposed.

- **A decentralized web platform** — MVC framework (models/controllers/views in C23) for building database-backed web apps served over `.onion`.

- **A peer discovery engine** — ZSLP tokens on the ZClassic blockchain store `.onion` addresses. The blockchain is the DNS.

## Architecture

```
zclassic23 binary (26MB, statically linked)
├── Full node         P2P port 8033/18033, RPC port 18232
├── Tor (embedded)    .onion hosting via dynhost, no ports
├── MVC framework     Models (SQLite), Controllers (C23), Views (HTML)
├── Fast sync         UTXO snapshot transfer between zclassic23 nodes
├── ZSLP registry     On-chain .onion peer discovery
└── Browser           zcl-browser (GTK WebKit, Tor-only)
```

### Dual Protocol Mode

| Mode | Port | Peers | Features |
|------|------|-------|----------|
| Legacy zclassicd | 8033 | MagicBean, zclassicd | Standard P2P, same consensus |
| zclassic23 | 18033 | Other zclassic23 nodes | Fast sync, .onion, ZSLP |

Both modes run simultaneously. Legacy peers see a normal zclassicd node.

### Project Structure (MVC)

```
app/models/          SQLite persistence (ActiveRecord pattern)
app/controllers/     RPC handlers, sync bridge, blog, onion service
app/views/           JSON/HTML serializers
config/              Boot, shutdown, global state
lib/                 25 library modules (crypto, net, chain, wallet, ...)
vendor/              Static libraries (secp256k1, leveldb, sqlite3, tor)
tools/               zcl-browser, zcl-rpc, zcl CLI
```

## Tools

```bash
make zcl-rpc         # CLI RPC client
make zcl-browser     # GTK Tor-only browser
make test            # run 685+ tests
```

## Network

- **Mainnet P2P**: 8033 (legacy) / 18033 (zclassic23)
- **Mainnet RPC**: 18232
- **First .onion seed**: `zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion`

## License

Copyright 2026 Rhett Creighton — Apache License 2.0
