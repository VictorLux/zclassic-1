# ZClassic23 Vision: A Sovereign Internet

## What We Are Building

A new internet where every participant runs a single binary that is simultaneously a full cryptocurrency node, a web application server, a Tor relay, and a peer discovery engine. No DNS. No clearnet websites. No cloud. No central servers. All traffic over Tor hidden services. All identity anchored to proof-of-work.

## Core Principles

1. **No DNS.** Peer discovery is on-chain via ZSLP tokens. The blockchain is the only name system.
2. **No clearnet content.** Web pages, apps, blogs — all served exclusively over `.onion`. Clearnet ports serve only the P2P blockchain protocol.
3. **No external dependencies.** One binary, statically linked, compiles from source with `make`. No package managers, no system libraries beyond libc.
4. **Proof-of-work identity.** Every action that costs the network (sync requests, site registration, peer announcements) requires a hashcash proof. No captchas, no accounts, no email.
5. **Privacy by default.** Shielded transactions (Sapling zk-SNARKs). Tor for all application traffic. No IP addresses leaked to web services.
6. **Every node is a server.** There is no client/server distinction. Every participant hosts, serves, and validates equally.

## Architecture

### Single Binary
```
zclassic23 (26MB, statically linked)
├── ZClassic full node        Consensus, validation, wallet
├── Tor (our fork)            Embedded, no ports, dynhost
├── MVC web framework         Models (SQLite), Controllers (C23), Views (HTML)
├── Fast sync                 UTXO snapshot transfer, PoW-gated
├── ZSLP registry             On-chain .onion discovery
└── Rate limiting             PoW + IP-based, per-service
```

### Network Layers

```
Layer 0: Tor circuits (transport, anonymity)
Layer 1: ZClassic P2P (blockchain consensus, tx relay)
Layer 2: zclassic23 P2P (fast sync, UTXO snapshots)
Layer 3: .onion web (blogs, apps, search, directory)
```

Layer 0 provides anonymity. Layer 1 provides consensus and money.
Layer 2 provides fast onboarding. Layer 3 provides the application platform.

### What Runs Over Clearnet (Attack Surface: Minimal)
- ZClassic P2P protocol on port 8033 (legacy zclassicd compatibility)
- zclassic23 P2P protocol on port 18033 (fast sync, PoW-protected)
- Authenticated RPC on port 18232 (localhost only, cookie auth)
- **Nothing else.** No HTTP. No WebSocket. No API endpoints.

### What Runs Over Tor (Attack Surface: Tor's)
- Landing page / directory (discover .onion sites)
- Search engine (find sites by keyword from ZSLP chain data)
- Blog / CMS (static files from `{datadir}/blog/`)
- MVC web apps (forms, databases, dynamic content)
- All served via dynhost — direct C function calls, no HTTP server

## Threat Model

### Threats We Defend Against
| Threat | Defense |
|--------|---------|
| Sybil attack on sync | PoW requirement per snapshot request (20-bit hashcash) |
| Bandwidth abuse | Rate limiting: 5000 chunks/IP/hour with LRU eviction |
| DNS poisoning | No DNS. Peer discovery is on-chain only. |
| Traffic analysis | All app traffic over Tor. P2P uses standard Bitcoin wire format. |
| Censorship of sites | Sites live on operator's node. No hosting provider to pressure. |
| Clearnet fingerprinting | No clearnet web content. Only blockchain P2P on clearnet. |
| Key theft | Shielded addresses (zk-SNARKs). Wallet encryption (TODO). |
| Eclipse attack | Multiple discovery methods: hardcoded seeds, ZSLP chain scan, Tor |
| Stale data attack | UTXO snapshot verified by SHA-256d commitment root |

### Threats We Accept
| Threat | Rationale |
|--------|-----------|
| Tor global adversary | Tor's threat model. We add nothing worse. |
| 51% attack on chain | Standard PoW security. ZClassic has dedicated miners. |
| Node operator sees their own data | Operators run their own nodes. This is a feature. |

## Backend Requirements

### P2P Layer (Clearnet)
- [ ] Bug-for-bug compatible with zclassicd (MagicBean)
- [x] `NODE_NETWORK | NODE_BLOOM` services (required by ZClassic peers)
- [x] Protocol version 170011
- [x] All 18 standard P2P messages handled
- [x] ZCL23 extension: 4 fast sync messages (zsnapshot, zsnapreq, zsnapdata, zsnapend)
- [x] PoW defense on fast sync requests
- [x] IP rate limiting on fast sync serving
- [ ] Peer scoring (prefer responsive peers, deprioritize dead ones)
- [ ] Addr relay (share good peers with other nodes)
- [ ] Block relay (relay new blocks to all peers)
- [ ] Transaction relay validation (check before forwarding)

### Tor Layer (No Clearnet)
- [x] Tor compiled into binary (our fork, dynhost)
- [x] SocksPort 0 (no SOCKS listener)
- [x] No HiddenServicePort (dynhost handles internally)
- [x] .onion address generated at startup
- [ ] dynhost → onion_service_handle_request wired
- [ ] Persistent .onion key (survives restarts)
- [ ] .onion address published on-chain via ZSLP
- [ ] Tor-to-Tor P2P connections (find peers via .onion, sync via Tor)

### On-Chain Registry (ZSLP)
- [x] ZCL23NODES token defined
- [x] blog_build_node_registry_genesis() builds GENESIS tx
- [x] blog_build_node_announce() embeds .onion in OP_RETURN
- [x] blog_discover_onion_peers() scans chain for announcements
- [ ] Actual ZSLP GENESIS tx broadcast on mainnet
- [ ] Site metadata: title, description, categories in OP_RETURN
- [ ] Site search by keyword matching against on-chain metadata

### Web Application Platform
- [x] Blog controller (static files from datadir)
- [x] Landing page / directory (lists discovered .onion sites)
- [x] Search handler (keyword search against ZSLP data)
- [x] Onion service request router (/ → directory, /search, /blog)
- [ ] MVC model layer (create models from C23 structs + SQLite)
- [ ] HTML template engine (variable substitution in .html files)
- [ ] Form handling (POST data → controller → model → redirect)
- [ ] Session management (per-circuit state, no cookies over Tor)
- [ ] CSS/JS static asset serving

### Wallet & Transactions
- [x] Transparent send/receive
- [x] Shielded send/receive (z_sendmany with witness maintenance)
- [x] importprivkey (instant, 11ms via SQLite index)
- [x] createrawtransaction (overwintered v4 format)
- [x] signrawtransaction (SQLite UTXO lookup)
- [x] sendrawtransaction (relay to peers)
- [x] Balance from ground truth (utxos × wallet_keys)
- [ ] Wallet encryption (encryptwallet, walletpassphrase)
- [ ] HD wallet (BIP32/44 key derivation)
- [ ] ZSLP token wallet (track token balances)

### Data Integrity
- [x] SQLite UTXO set mirrored from LevelDB
- [x] Wallet rebuilt from UTXO set at every boot
- [x] Block hashes verified against zclassicd (10/10 match)
- [x] Live blocks validated in real-time
- [x] Sapling commitment tree maintained incrementally
- [ ] UTXO set Merkle commitment (for snapshot verification)
- [ ] Checkpoint validation at known heights
- [ ] Periodic chain consistency audit (background thread)

### Performance
- [x] Warm restart: 4.3s (flat block_index mmap)
- [x] Fast sync: 1.6M UTXOs in ~60s
- [x] importprivkey: 11ms
- [x] Block validation: matches zclassicd speed
- [ ] Parallel block validation (multi-threaded script checking)
- [ ] UTXO set compression (for faster snapshot transfer)
- [ ] Incremental flat file updates (don't rebuild on every restart)

## The Growth Cycle

```
1. User runs ./zclassic23
2. Node connects to hardcoded .onion seeds via Tor
3. Fast syncs UTXO set from a zclassic23 peer (~60s)
4. Node becomes a full zclassicd peer on clearnet (port 8033)
5. Node creates its own .onion address
6. Node publishes .onion via ZSLP token on-chain
7. Node starts serving its blog/app over .onion
8. Other new nodes discover this node from the blockchain
9. The network grows. Every node serves. Every node validates.
```

No central authority decides who can participate.
No registrar controls the names.
No hosting provider can take sites down.
The blockchain is the only source of truth.

## What This Is Not

- Not a browser. Not a search engine. Not a social network.
- It is the **infrastructure** for all of those things.
- Anyone can build a browser, search engine, or social network on top.
- The platform provides: identity (PoW), naming (ZSLP), hosting (.onion),
  money (ZCL), privacy (Sapling), and consensus (blockchain).

## License

Copyright 2026 Rhett Creighton — Apache License 2.0
