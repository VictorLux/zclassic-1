# Backlog

Long-running queue of unscheduled work. When an agent clears its wave priority list and stretch items, pull from here instead of waiting for AGENT1 to write the next wave.

## Rules

- **One agent per item.** Add your name in the `Owner` column when you start; remove if you abandon.
- **Stay in your territory.** Don't claim an item in files the other agent owns.
- **Commit checkoff.** When you finish, move the row to the `## Done` section below with the commit hash.
- **Add new items anywhere.** Both agents and AGENT1 can append new backlog entries.

---

## Open

| Area           | Item                                                          | Est   | Owner  | Notes |
|----------------|---------------------------------------------------------------|-------|--------|-------|
| consensus      | PHGR13 fix (after investigation lands)                        | L     |        | blocks network tip sync, highest user value |
| validation     | Parallel script verification (workpool)                       | M     |        | measurable perf win on connect_tip |
| validation     | Timestamp sanity check hardening                              | S     |        | audit contextual_check_block_header vs BIP113 |
| net            | Compact block (BIP152) implementation (after investigation)   | L     |        | bandwidth reduction, IBD speedup |
| net            | Headers-first sync refinement                                 | M     |        | tighter getheaders loop, backoff tuning |
| net            | Addrman buckets rebalancing                                   | S     |        | improve peer diversity |
| net            | NAT traversal fallback (UPnP + NAT-PMP)                       | M     |        | already partial; finish the path |
| storage        | Block pruning service                                         | M     |        | disk saving for non-archival nodes |
| storage        | SQLite WAL size cap                                           | S     |        | prevent unbounded wal file on slow checkpoints |
| storage        | Database schema migration framework                           | M     |        | proper versioned migrations instead of ad-hoc |
| wallet         | HD wallet (BIP32/39/44) derivation                            | L     |        | big ticket, separate commit per phase |
| wallet         | Watch-only address support                                    | S     |        | for monitoring cold storage |
| wallet         | Transaction coin selection algorithm audit                    | M     |        | BnB vs knapsack; test with adversarial inputs |
| wallet         | Multisig P2SH support (transparent)                           | M     |        | audit script building |
| mempool        | Fee estimation robustness                                     | S     |        | protect against manipulation |
| mempool        | Orphan tx cache with eviction                                 | S     |        |       |
| p2p            | Bloom filter support (BIP37) audit                            | S     |        | deprecate or properly gate behind config |
| p2p            | Dandelion tx propagation                                      | L     |        | privacy upgrade |
| security       | RPC cookie rotation                                           | S     |        | already generated per-boot; add timed rotation |
| security       | Sapling key scrubbing on free                                 | S     |        | audit all paths that touch spending keys |
| security       | Dependency vulnerability scan                                 | S     |        | CI job: audit OpenSSL, libevent, leveldb versions |
| security       | TLS for HTTP RPC                                              | M     |        | currently plaintext on non-loopback |
| observability  | Prometheus /metrics HTTP endpoint                             | S     | agent3 | wave 6 item — move here when plan rotates |
| observability  | Grafana dashboard JSON export                                 | S     |        | ship a canonical dashboard |
| observability  | OpenTelemetry span export                                     | M     |        | after trace.{h,c} lands |
| observability  | MCP request/response recording for replay                     | M     |        | for debugging + regression |
| tooling        | `make ci` target that runs test + fuzz-ci + coverage          | S     |        |       |
| tooling        | `zcl-cli` subcommand for common ops                           | M     |        | wrap the important RPC calls |
| tooling        | Dockerfile + docker-compose for reproducible dev env          | S     |        |       |
| tooling        | Release builder: tagged binary + SHA3 + signature             | M     |        |       |
| tests          | 1000-iteration fuzzer CI run (weekly)                         | S     |        | catch rare bugs after merge |
| tests          | Property-based tests for tx validation                        | M     |        | quickcheck-style |
| docs           | Operator runbook                                              | M     |        | "what to do when X is wrong" |
| docs           | Architecture diagrams                                         | S     |        | current ARCHITECTURE.md is text only |
| docs           | MCP tool reference generated from router                      | S     |        | auto-generated from `zcl_tools_list` |

---

## Done

*(Move completed rows here with commit hash.)*

| Area     | Item                                          | Commit      | Notes |
|----------|-----------------------------------------------|-------------|-------|
| security | HTTP RPC middleware (rate limit + IP ban)     | `6f9d52?`   | wave 4 session 4 |
| security | Wallet keystore encryption primitives         | `9eb4b63`   | wave 4 session 5 |
| safety   | chain_state_repository (single-writer tip)    | `adc0371`   | wave 1  |
| safety   | recovery_policy (destructive op gate)         | `6a30730`   | wave 2  |
| safety   | db_txn (scoped transactions)                  | `85397d0`   | wave 2  |
| safety   | block_index_integrity (SHA3 sidecar)          | `7453523`   | wave 3  |
| safety   | wallet_backup_service (hourly rotated)        | `22a1a82`   | wave 4  |
| safety   | crash_recovery_test harness                   | `5f41ebc`   | wave 4  |
| safety   | libFuzzer harnesses (block/script/p2p)        | `d05cd66`   | wave 4  |
| safety   | disk_monitor service                          | `bc3b7a9`   | wave 5  |
| safety   | db_maintenance scheduler                      | `fae62de`   | wave 5  |
| safety   | fuzzer finding #1 fix (transaction_alloc)     | `e53dc59`   | wave 4 session 3 |
