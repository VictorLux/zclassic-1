# MCP Reference — ZClassic23

Auto-generated from `./zclassic23 -mcp` via `make docs-mcp`. Do not edit by hand; regenerate after changing the router surface.

**Tool count:** 76


## Ops & Observability

Node health, diagnostics, metrics, and MCP introspection.  
**Tools:** 22

### `zcl_admin`

Admin dashboard: aggregates zcl_kpi + zcl_peer_report + zcl_rpc_report + zcl_events into one snapshot and derives threshold-based alerts from the nested counters. Missing subsystems render as null; flagship single-call operator tool.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `since` | integer (min 0, max 9223372036854775807) | no | `0` | Unix-seconds baseline for future windowed counters (unused). |

### `zcl_benchmark`

Hash / malloc / hash160 throughput (sha256d, malloc-4K, hash160 ops/sec).

_No parameters._

### `zcl_config_reload`

Re-read env-tunable config for live subsystems (peer_scoring, rpc_middleware) without restarting the node. Returns the new effective values so an operator can verify the change took effect.

_No parameters._

### `zcl_consensus_report`

Consensus-reject snapshot: per-(kind, reason) counts plus tx/block totals and overflow buckets for the in-process EV_CONSENSUS_REJECT_TX / EV_CONSENSUS_REJECT_BLOCK stream. Dashboard companion to AGENT2's zcl_explain_reject.

_No parameters._

### `zcl_dbstats`

Database health: table counts, SQLite page stats, sizes.

_No parameters._

### `zcl_events`

Recent event log: sync events, peer connections, blocks.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `count` | integer (min 1, max 1000) | no | `20` | Number of events |

### `zcl_filemanifest`

File service status: chunks, SHA3 hashes, total size.

_No parameters._

### `zcl_getmempoolinfo`

Mempool size, bytes, usage.

_No parameters._

### `zcl_getmininginfo`

Mining stats: hashrate, difficulty, current block, pooled tx.

_No parameters._

### `zcl_getrawmempool`

Array of txids currently in the mempool.

_No parameters._

### `zcl_health`

Health check: pass/fail, chain height, peers, sync, onion.

_No parameters._

### `zcl_kpi`

One-shot KPI dashboard: height, peer_count, sync, validation, health, mempool, wallet, chain, network — every subsystem in one response. The flagship operator tool for debugging.

_No parameters._

### `zcl_logtail`

Tail the structured event log. Optional domain prefix filter.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `count` | integer (min 1, max 10000) | no | `100` | Number of events to scan |
| `domain` | string (len ≤ 64) | no | `—` | Event type prefix filter (e.g. "MCP", "VAL.", "NET.") |

### `zcl_metrics`

Prometheus-text metrics dump: request counters, latency histogram, and summary totals accumulated in-process.

_No parameters._

### `zcl_metrics_reset`

Reset all MCP metric counters. Destructive — gated by the middleware rate limiter.

_No parameters._

### `zcl_openapi`

Emit an OpenAPI 3.0-flavored schema document derived from the MCP routing table. Clients can use it for type generation or auto-test harnesses.

_No parameters._

### `zcl_profile`

Per-thread CPU sampler: reads /proc/self/task/*/stat before and after `duration_ms`, returns top N threads by CPU delta with name, user_ms, sys_ms, cpu_pct. For diagnosing slow nodes without attaching gdb.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `duration_ms` | integer (min 100, max 10000) | no | `1000` | Sample window in ms (clamped to [100, 10000]) |
| `top_n` | integer (min 1, max 64) | no | `10` | Max threads returned, sorted by CPU (clamped to [1, 64]) |

### `zcl_rpc`

Call any RPC method directly. 85+ commands available.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `method` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | RPC method name |
| `params` | string | no | `[]` | JSON params array |

### `zcl_rpc_report`

HTTP RPC middleware report: live rate-limit / ban config plus allowed/rate-limited/banned/auth-failure counters and current tracked-IP and active-ban gauges. Parallel to zcl_peer_report for the RPC surface.

_No parameters._

### `zcl_self_test`

Call every registered tool with safe defaults, reporting pass/fail/skip. Destructive tools are skipped.

_No parameters._

### `zcl_status`

Node status: block height, peers, sync state, onion address, bg-validation progress, health checks. The single command to check if everything is working.

_No parameters._

### `zcl_tools_list`

Dump the full MCP routing table: every tool with its domain, description, and parameter schema. Self-documenting surface.

_No parameters._


## Chain & Consensus

Block/transaction lookup and consensus state.  
**Tools:** 10

### `zcl_dataintegrity`

SHA3-256 hashes over all consensus tables.

_No parameters._

### `zcl_getblock`

Get block by height or hash.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `block_id` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Height or hash |
| `verbosity` | integer (min 0, max 2) | no | `1` | 0=hex, 1=JSON, 2=JSON+tx |

### `zcl_getblockchaininfo`

Chain state: height, best block, difficulty, chain work, value pools.

_No parameters._

### `zcl_getblockcount`

Current block height.

_No parameters._

### `zcl_getrawtransaction`

Transaction by id. verbose=1 decodes, verbose=0 returns hex.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `txid` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Transaction id (hex) |
| `verbose` | integer (min 0, max 1) | no | `1` | 0=hex, 1=JSON |

### `zcl_hodlwave`

UTXO age distribution: 10 buckets from 24h to 5y+.

_No parameters._

### `zcl_mmb`

Merkle Mountain Belt root. FlyClient chain verification.

_No parameters._

### `zcl_syncstate`

Sync state machine: phase, progress, header/block/UTXO status.

_No parameters._

### `zcl_utxocommitment`

SHA3-256 over entire UTXO set in canonical order.

_No parameters._

### `zcl_validationstatus`

Background validation: verified height, sigs, proofs, blocks/sec.

_No parameters._


## Network & Peers

P2P peer management, latency, and onion service.  
**Tools:** 9

### `zcl_addnode`

Add/remove peer. Actions: add, remove, onetry.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `addr` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | IP:port |
| `action` | string (add \| remove \| onetry) | no | `onetry` | add \| remove \| onetry |

### `zcl_gametypes`

P2P game types: Ping (latency measurement), TicTacToe.

_No parameters._

### `zcl_networkinfo`

Network info: version, connections, relay fee.

_No parameters._

### `zcl_onion_health`

Probe the in-process onion service via direct function call (no Tor circuit, no SOCKS). Returns {ok, onion_address, path, latency_ms, response_bytes}. Liveness check, not an e2e reach test.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string (len ≤ 256) | no | `/directory.json` | URL path to probe (default /directory.json) |

### `zcl_onion_status`

Tor onion service: .onion address, bootstrap state.

_No parameters._

### `zcl_peer_report`

Peer scoring report: live ban threshold/hours/decay config plus per-kind offence counts and total bans observed since boot.

_No parameters._

### `zcl_peerlatency`

Latency for all peers: ping_ms, min_ping_ms, avg_latency_ms.

_No parameters._

### `zcl_peers`

Connected peers with addresses, latency, services, heights.

_No parameters._

### `zcl_pingpeer`

Measure round-trip latency to a connected peer.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `peer_id` | integer (min 0, max 1000000) | **yes** | `—` | Peer ID from zcl_peers |


## Wallet

Transparent + shielded balance, keys, and transactions.  
**Tools:** 19

### `zcl_balance`

Total wallet balance: transparent + shielded.

_No parameters._

### `zcl_dumpprivkey`

Export the WIF private key for a transparent address.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Address |

### `zcl_getnewaddress`

Generate new transparent (t-addr) receiving address.

_No parameters._

### `zcl_gettransaction`

Fetch a single wallet transaction by id.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `txid` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Transaction id (hex) |

### `zcl_getwalletinfo`

One-shot wallet health snapshot: balance, tx count, keys, status.

_No parameters._

### `zcl_importprivkey`

Import a WIF private key into the wallet.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `privkey` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | WIF-encoded private key |
| `label` | string (len ≤ 128) | no | `—` | Optional label |
| `rescan` | boolean | no | `false` | Rescan chain after import |

### `zcl_listaddresses`

All transparent (t-addr) addresses in the wallet.

_No parameters._

### `zcl_listtransactions`

Recent wallet transaction history.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `count` | integer (min 1, max 10000) | no | `10` | Number of transactions to return |
| `skip` | integer (min 0, max 10000000) | no | `0` | Number of most recent to skip |

### `zcl_listunspent`

List transparent UTXOs available to spend.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `minconf` | integer (min 0, max 9999999) | no | `1` | Minimum confirmations |
| `maxconf` | integer (min 0, max 9999999) | no | `9999999` | Maximum confirmations |

### `zcl_listwalletkeys`

List all keys (metadata, and optionally WIFs).

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `include_privkeys` | boolean | no | `false` | Include WIF private keys in the response |

### `zcl_replaywalletfromchain`

Rebuild the derived wallet state by replaying the chain. Destructive — requires confirm=true.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `confirm` | boolean | **yes** | `—` | Must be true — destructive: wipes derived wallet state |

### `zcl_rescanblockchain`

Manually trigger a wallet rescan over a height range.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `start_height` | integer (min 0, max 100000000) | no | `0` | Start block height |
| `stop_height` | integer (min 0, max 100000000) | no | `—` | Stop block height |

### `zcl_send`

Send ZCL (transparent or shielded).

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `from` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Source address |
| `to` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Destination address |
| `amount` | number | **yes** | `—` | Amount in ZCL |

### `zcl_sendtoaddress`

Simple send to a single transparent address.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Destination t-address |
| `amount` | number | **yes** | `—` | Amount in ZCL |

### `zcl_walletaudit`

Reconcile the wallet against the on-chain UTXO set.

_No parameters._

### `zcl_z_getbalance`

Balance for a single t-address or z-address.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Shielded z-address or t-address |
| `minconf` | integer (min 0, max 9999999) | no | `1` | Minimum confirmations |

### `zcl_z_getnewaddress`

Generate new shielded Sapling (z-addr) receiving address.

_No parameters._

### `zcl_z_listaddresses`

All shielded Sapling (z-addr) addresses in the wallet.

_No parameters._

### `zcl_z_listunspent`

List shielded notes available to spend.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `minconf` | integer (min 0, max 9999999) | no | `1` | Minimum confirmations |


## Apps (ZSLP, Names, Messaging, Market, Swaps)

Higher-level apps built on the chain.  
**Tools:** 16

### `zcl_market_buy`

Initiate purchase and download of a file from the ZCL Market.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `root_hash` | string (len ≥ 64, len ≤ 64) | **yes** | `—` | 64-char hex SHA3 of offer |

### `zcl_market_list`

List files available on the ZCL Market P2P file sharing network.

_No parameters._

### `zcl_market_offer`

Announce a file for sale on the ZCL Market.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `filepath` | string (len ≥ 1, len ≤ 1024) | **yes** | `—` | Path to file to share |
| `price_per_mb_zat` | integer (min 0, max 1000000000) | **yes** | `—` | Price per MB in zatoshis |

### `zcl_market_status`

ZCL Market status: cached offers, persisted offers, active downloads.

_No parameters._

### `zcl_msg_inbox`

List messages in the inbox. Newest first.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `unread_only` | boolean | no | `false` | Only unread |

### `zcl_msg_read`

Mark a message as read and return its content.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `msg_id` | string (len ≥ 64, len ≤ 64) | **yes** | `—` | 64-char hex message ID |

### `zcl_msg_send`

Send a P2P message to a connected peer.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `peer_id` | integer (min 0, max 1000000) | **yes** | `—` | Connected peer ID |
| `message` | string (len ≥ 1, len ≤ 4000) | **yes** | `—` | Message text |

### `zcl_msg_send_named`

Send a message to a ZCL Name. Resolves the name first.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `name` | string (len ≥ 1, len ≤ 63) | **yes** | `—` | ZCL Name (e.g. alice) |
| `message` | string (len ≥ 1, len ≤ 4000) | **yes** | `—` | Message text |

### `zcl_name_list`

List all registered ZCL Names on the network.

_No parameters._

### `zcl_name_register`

Build an OP_RETURN script to register a ZCL Name on-chain.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `name` | string (len ≥ 1, len ≤ 63) | **yes** | `—` | Name (1-63 chars) |
| `type` | string (onion \| zaddr \| taddr) | **yes** | `—` | Target type |
| `value` | string (len ≥ 1, len ≤ 256) | **yes** | `—` | Target value |

### `zcl_name_resolve`

Resolve a ZCL Name to its target (.onion, z-addr, or t-addr).

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `name` | string (len ≥ 1, len ≤ 63) | **yes** | `—` | Name to resolve |

### `zcl_swap_chains`

List supported chains for atomic swaps: ZCL, BTC, LTC, DOGE.

_No parameters._

### `zcl_swap_initiate`

Initiate an atomic swap. Generates secret, builds HTLC, returns P2SH.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `my_address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Your address (refund path) |
| `counter_address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Counterparty address |
| `amount` | integer (min 1, max 21000000) | **yes** | `—` | Amount in coins |
| `locktime_blocks` | integer (min 1, max 1000000) | **yes** | `—` | Lock duration in blocks |
| `chain` | string (zcl \| btc \| ltc \| doge) | no | `zcl` | Chain |

### `zcl_swap_list`

List atomic swap contracts.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `state` | string (pending \| funded \| redeemed \| refunded) | no | `—` | Filter by state |

### `zcl_swap_participate`

Participate in an atomic swap (counter-HTLC with shorter locktime).

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `my_address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Your address |
| `counter_address` | string (len ≥ 1, len ≤ 128) | **yes** | `—` | Initiator address |
| `amount` | integer (min 1, max 21000000) | **yes** | `—` | Amount |
| `locktime_blocks` | integer (min 1, max 1000000) | **yes** | `—` | Lock blocks (shorter than initiator) |
| `secret_hash` | string (len ≥ 64, len ≤ 64) | **yes** | `—` | 64-char hex secret hash |
| `chain` | string (zcl \| btc \| ltc \| doge) | no | `zcl` | Chain |

### `zcl_tokens`

List all ZSLP tokens on the network.

_No parameters._
