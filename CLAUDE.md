# ZClassic C23 Full Node

## Mission
Make `zclassic23` (pure C23) a drop-in replacement for `zclassicd` (C++).
Wire-compatible, RPC-compatible, wallet-compatible, transaction-compatible.

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

## Current Status
- **Tests**: 609 ALL TESTS PASSED (0 failures)
- **C++ node** (`zclassicd`): P2P 8033, RPC 8232, data at `~/.zclassic`
- **C23 node** (`zclassic23`): P2P 18033, RPC 18232, data at `~/.zclassic-c23`
- **Sapling spends**: z→z and z→t implemented via `z_sendmany` (commitment tree tracking, witness maintenance, Merkle path extraction, spend proof construction)

## Known Issues
1. Sapling verification: 5 blocks failed `bad-txns-sapling-spend-description-invalid`
2. LevelDB MANIFEST corruption on DB open
3. JoinSplit anchor validation: placeholder returns true
4. Sapling witness persistence: witnesses need rescan after node restart (not yet maintained incrementally during connect_block)
5. **FIXED**: Witness cursor depth bug — `incremental_witness_deserialize` set `cursor.depth` to full tree depth (32) instead of `cursor_depth`, causing wrong root after serialization roundtrip

## Quality Bar
Q = Clarity × Reliability × Performance × TestCoverage × UserImpact
Every commit must raise Q.
