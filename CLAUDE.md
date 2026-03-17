# ZClassic23 — A New Internet

## Vision
ZClassic23 is not just a cryptocurrency node. It is a decentralized internet platform.

Every zclassic23 node is simultaneously:
- A **full ZClassic node** (bug-for-bug compatible with legacy zclassicd)
- A **Tor hidden service** hosting web applications (.onion, no ports exposed)
- A **peer discovery engine** using ZSLP tokens on-chain (blockchain = DNS)
- A **fast sync server** transferring UTXO snapshots to new nodes in ~60 seconds
- An **MVC application platform** for building database-backed web apps in pure C23

One binary. Pure C23. Tor built in. No cloud. No DNS. No central servers.

## Mission
Drop-in replacement for `zclassicd` in legacy mode. New internet platform in zclassic23 mode.

## Build
```bash
make zclassic23    # build node
make test          # run tests (609 tests)
```

## Project Structure (Rails-style MVC)
```
zclassic/
├── main.c                  # Entry point
├── Makefile                # Build system
├── app/                    # Application layer (MVC)
│   ├── models/             # ActiveRecord models (SQLite persistence)
│   │   ├── include/models/ # block.h, utxo.h, wallet_key.h, ...
│   │   └── src/            # block.c, database.c, ...
│   ├── controllers/        # RPC handlers + sync bridge
│   │   ├── include/controllers/
│   │   └── src/            # blockchain_controller.c, wallet_controller.c, ...
│   └── views/              # JSON serializers (planned)
│       ├── include/views/
│       └── src/
├── config/                 # Boot + configuration
│   ├── include/config/     # boot.h
│   └── src/                # boot.c (app_init, app_shutdown)
├── db/                     # Schema + migrations
│   ├── schema.sql          # Canonical schema definition
│   └── migrate/            # Migration history
├── lib/                    # Library modules
│   ├── crypto/             # sha256, equihash, ed25519, ...
│   ├── chain/              # chainparams, pow, checkpoints
│   ├── net/                # connman, msgprocessor, addrman
│   ├── rpc/                # HTTP server, JSON-RPC protocol, routing
│   ├── validation/         # check_block, connect_block, process_block
│   ├── wallet/             # keystore, coin selection, tx creation
│   ├── storage/            # LevelDB wrappers (block_index_db, coins_db)
│   ├── coins/              # UTXO view hierarchy
│   ├── primitives/         # block, transaction serialization
│   ├── script/             # interpreter, standard, sigcache
│   ├── consensus/          # params, upgrades
│   ├── keys/               # pubkey, key, key_io
│   ├── encoding/           # base58, bech32
│   ├── json/               # JSON parser + builder
│   ├── mining/             # miner, gen
│   ├── zcash/              # sapling, jubjub, groth16
│   ├── bloom/              # BIP37 bloom filters
│   ├── policy/             # fee estimation
│   ├── support/            # cleanse, pagelocker
│   ├── util/               # scheduler, sync, timedata
│   ├── metrics/            # TUI display
│   └── test/               # 488 tests
└── vendor/
    ├── include/            # External headers (secp256k1, leveldb)
    ├── lib/                # Pre-built libraries (gitignored)
    └── zclassic-ref/       # Full C++ reference node (gitignored)
```

## MVC Architecture

### Models (`app/models/`)
ActiveRecord pattern with SQLite. Each model has CRUD operations, validations, and callbacks.
- `database.h/.c` — SQLite connection, schema creation, prepared statement cache
- `activerecord.h` — Base: validates_presence_of, before_save/after_save callbacks
- `block.h/.c` — Block headers + metadata
- `tx_index.h/.c` — Transaction index (txid → file position)
- `utxo.h/.c` — Unspent outputs with script classification
- `wallet_key.h/.c` — Transparent + Sapling keys
- `wallet_tx.h/.c` — Wallet transactions + UTXOs + Sapling notes
- `mempool_entry.h/.c` — Mempool persistence
- `peer.h/.c` — P2P peer addresses

### Controllers (`app/controllers/`)
Handle RPC requests and bridge validation pipeline to models.
- `wallet_controller` — getnewaddress, getbalance, sendtoaddress, listunspent, multisig (916 lines)
- `wallet_shielded_controller` — z_sendmany, z_getbalance, z_listunspent, z_exportkey (1636 lines)
- `wallet_diagnostic_controller` — walletaudit, traceutxo, diagnoseutxos, walletledger (2393 lines)
- `wallet_rescan_controller` — rescanwitnesses, rescanwallet, fastsync, coinanalysis (871 lines)
- `wallet_helpers` — Shared wallet state + amount formatting utilities (229 lines)
- `blockchain_controller` — getblockcount, getblock, getblockhash, ...
- `transaction_controller` — getrawtransaction, sendrawtransaction, ...
- `mining_controller` — getmininginfo, getblocktemplate, submitblock
- `network_controller` — getpeerinfo, getconnectioncount, addnode
- `misc_controller` — getinfo, validateaddress, stop
- `chain_inspect_controller` — chainview, chainstats, gettxdetail, saplingtreeinfo, scancommitments, verifychainroots (629 lines)
- `sync_controller` — Bridges validation events to SQLite (connect/disconnect block)

### Views (`app/views/`) — Planned
JSON response serializers for each model type.

### Config (`config/`)
- `boot.h/.c` — app_init(), app_shutdown(), global state wiring

### Lib (`lib/`)
23 library modules. Include paths: `#include "crypto/sha256.h"` works via `-Ilib/<name>/include`.

## Dual Protocol Mode
| Mode | Port | Compatible With | Features |
|------|------|----------------|----------|
| **Legacy zclassicd** | 8033 | MagicBean, zclassicd | Bug-for-bug P2P, same consensus rules |
| **zclassic23** | 18033 | Other zclassic23 nodes | Fast sync, .onion hosting, ZSLP discovery |

Peers detect zclassic23 mode via `NODE_ZCL23` service bit (1<<10) in version handshake.
Legacy peers ignore the bit. Both modes run simultaneously.

## Current Status
- **Fast sync**: 1,598,612 UTXOs transferred between 2 live nodes in 60s
- **Tor**: Linked into binary (vendor/tor, our fork). dynhost handles .onion directly.
- **First .onion**: `zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion` (rhett.dev)
- **Browser**: `zcl-browser` — GTK WebKit, Tor-only, .onion directory
- **ZSLP registry**: ZCL23NODES token for on-chain peer discovery
- **Wallet**: 0.98 ZCL consolidated at `t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn`
- **Witnesses**: Incremental maintenance in connect_block + bulk_blocks
- **Startup**: 4.3s warm restart (flat block_index mmap)

## Architecture

### The Pipeline
```
New node starts
  → Hardcoded .onion seeds (no DNS needed)
  → Connects via Tor to seed node
  → NODE_ZCL23 detected → fast sync triggered
  → 1.6M UTXOs transferred in ~60s
  → Node becomes full legacy zclassicd peer (port 8033)
  → Node publishes its own .onion via ZSLP token
  → Other new nodes discover it from blockchain
  → Cycle repeats — network grows organically
```

### Clearnet vs Tor
| Transport | Serves | Defense |
|-----------|--------|---------|
| Clearnet P2P (8033/18033) | Legacy ZCL + fast sync only | PoW + IP rate limit |
| Clearnet RPC (18232) | Authenticated JSON-RPC only | Cookie/password auth |
| Tor .onion | Blog, web apps, search, directory | Tor anonymity |

No blog over clearnet. No web content over clearnet. Clearnet = blockchain data only.

## Quality Bar
Q = Clarity × Reliability × Performance × TestCoverage × UserImpact
Every commit must raise Q.
