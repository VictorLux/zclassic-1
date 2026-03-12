# ZClassic C23 Full Node — Compatibility Validation Plan

## Mission
Make `zcld` (pure C23) a drop-in replacement for `zclassicd` (C++).
Wire-compatible, RPC-compatible, wallet-compatible, transaction-compatible.

## Current Status
- **C++ node** (`zclassicd`): Running on default ports (P2P 8033, RPC 8232), data at `~/.zclassic`
- **C23 node** (`zcld`): Running on ports (P2P 18033, RPC 18232), data at `~/.zclassic-c23`
- **Block height**: C++ at ~3035745, C23 syncing from copied chainstate
- **Tests**: ALL TESTS PASSED (0 failures)
- **Critical fix applied**: Double-free in `coins_view_cache_get_coins()` — was shallow-copying coins with shared vout pointer

## Phase 1: Block Sync Verification (IN PROGRESS)
- [x] Fix double-free crash in coins cache (shallow copy → deep copy via `coins_copy()`)
- [x] Fix height-aware equihash validation (N=200/K=9 pre-fork, N=192/K=7 post-Blossom)
- [x] Fix block file position allocation (`find_block_pos`)
- [x] Fix P2P addnode direct connection
- [x] Fix default P2P port (8033 not 8233)
- [ ] Verify C23 node syncs blocks from C++ node without crashing
- [ ] Verify chain tip advances to match C++ node height
- [ ] Fix Sapling spend verification (5 blocks failed with `bad-txns-sapling-spend-description-invalid`)

## Phase 2: RPC Compatibility Testing
Test all 38 RPC commands against both nodes, compare outputs.

### Blockchain RPCs
- [ ] `getblockcount` — heights must match
- [ ] `getbestblockhash` — hashes must match at same height
- [ ] `getblockhash <height>` — test at heights 0, 1, 585318 (Blossom fork), tip
- [ ] `getblock <hash>` — full JSON output comparison
- [ ] `getblockheader <hash>` — field-by-field comparison
- [ ] `getblockchaininfo` — chain, blocks, headers, difficulty, verificationprogress
- [ ] `getdifficulty` — must match exactly
- [ ] `getmempoolinfo` — size, bytes fields

### Raw Transaction RPCs
- [ ] `getrawtransaction <txid>` — hex must match for known txids
- [ ] `decoderawtransaction <hex>` — all fields must match
- [ ] `createrawtransaction` — produce valid unsigned tx
- [ ] `sendrawtransaction <hex>` — broadcast and relay to peers

### Network RPCs
- [ ] `getnetworkinfo` — version, subversion, connections
- [ ] `getpeerinfo` — peer details, latency
- [ ] `getconnectioncount` — correct count
- [ ] `ping` — triggers ping/pong cycle
- [ ] `addnode` — adds and connects to peer

### Mining RPCs
- [ ] `getmininginfo` — blocks, difficulty, chain
- [ ] `getblocksubsidy <height>` — correct subsidy at various heights
- [ ] `getblocktemplate` — valid template returned
- [ ] `submitblock` — accepts valid block hex

### Wallet RPCs
- [ ] `getnewaddress` — valid t-addr with correct prefix (t1...)
- [ ] `getbalance` — correct balance
- [ ] `getwalletinfo` — wallet metadata
- [ ] `listunspent` — UTXO list
- [ ] `dumpprivkey <addr>` — export and re-import roundtrip
- [ ] `importprivkey <key>` — import key, rescan, find balance

### Misc RPCs
- [ ] `getinfo` — version, blocks, connections, balance
- [ ] `validateaddress <addr>` — isvalid, ismine, address fields
- [ ] `stop` — graceful shutdown

## Phase 3: Transaction Testing (requires real ZCL)
- [ ] **t-addr → t-addr**: Send transparent-to-transparent
- [ ] **Receive at C23 wallet**: Import key or generate address, send from C++ wallet
- [ ] **Send from C23 wallet**: `sendtoaddress` from C23 node
- [ ] **Verify on both nodes**: Both nodes see the tx in mempool, then confirmed
- [ ] **Multi-input tx**: Spend multiple UTXOs in one transaction
- [ ] **Change output**: Verify change returns to wallet
- [ ] **Mempool relay**: Tx created on C23 appears in C++ mempool and vice versa
- [ ] **Block confirmation**: Mined block includes the tx, both nodes advance

## Phase 4: Advanced Testing
- [ ] **Reorg handling**: Force a reorg, verify both nodes agree
- [ ] **Peer discovery**: C23 node discovers peers from C++ node via `addr` messages
- [ ] **Persistence**: Restart C23 node, verify state is preserved
- [ ] **Concurrent operation**: Both nodes run indefinitely without memory leaks
- [ ] **Large blocks**: Process blocks with many transactions

## Known Issues
1. **Sapling verification**: 5 blocks failed `bad-txns-sapling-spend-description-invalid`
2. **LevelDB MANIFEST corruption**: C23 node modifies MANIFEST when opening DBs for writing
3. **Debug logging**: Extensive printf debugging throughout — needs cleanup
4. **JoinSplit anchor validation**: Placeholder returns true (not fully implemented)

## Build
```bash
make zcld    # build node
make test    # run tests (488 tests)
```

## Module Structure
C23 source uses qedc module layout under `modules/`:
```
modules/<name>/include/<name>/*.h   (public headers)
modules/<name>/src/*.c              (source files)
modules/<name>/module.cfg           (module metadata)
```
25 modules: bloom, chain, coins, consensus, core, crypto, db, encoding, init,
json, keys, metrics, mining, net, policy, primitives, rpc, script, storage,
support, util, validation, wallet, zcash, test.

Include paths use `-Imodules/<name>/include` so `#include "crypto/sha256.h"` works unchanged.

## Running Both Nodes
```bash
# C++ node (default ports)
./src/zclassicd -showmetrics=0

# C23 node (alternate ports, connects to C++ node, accepts inbound)
./zcld -datadir=/home/bob/.zclassic-c23 \
  -port=18033 -rpcport=18232 -rpcuser=c23user -rpcpassword=c23pass \
  -addnode=127.0.0.1:8033 -listen -showmetrics=0
```

## RPC Testing
```bash
# C++ node
./src/zclassic-cli -rpcuser=zcluser -rpcpassword=zclpass <command>

# C23 node
./src/zclassic-cli -rpcuser=zcluser -rpcpassword=zclpass -rpcport=18232 <command>
```

## Quality Bar
Q = Clarity × Reliability × Performance × TestCoverage × UserImpact
Every commit must raise Q. The C23 node must produce identical outputs to the C++ node for all supported RPCs.
