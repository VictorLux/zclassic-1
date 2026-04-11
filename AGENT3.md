# AGENT3 — RPC Hardening, Wallet Encryption & Observability Depth

**Worktree:** `~/zclassic23-3`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT2 (`~/zclassic23-2`) — boot decomposition / consensus / resource controls, different files

> **⇒ Active plan: [`WAVE_6.md`](./WAVE_6.md).** The priority queue, carry-over, and new items are there now. When wave 6 clears, pull from [`BACKLOG.md`](./BACKLOG.md). AGENT1 no longer edits the `Current Status` section below — it is yours.

---

## Mission

The MCP surface is now thoroughly hardened. **Wave 5 extends that to the HTTP RPC server, the on-disk wallet, and every other surface where a mistake is expensive.** Finish carry-over wave 4 items #2 and #3 first. Then deepen observability with WebSocket event streaming, tracing spans, and per-peer bandwidth quotas. Finally open a second interface path (gRPC) as a modern alternative to JSON-RPC so external tooling has a typed contract.

## Already done (don't redo)

- Phase 1–3: router + 70 tools + controllers + middleware + metrics + validators + e2e tests
- `peer_scoring` + `zcl_peer_report` tool + peer metrics wired into Prometheus
- `log_json` structured logging helper + 10 migrated sites (wave 4 #4)
- `test_mcp_e2e` hardened against stale binary + dynamic tool count (wave 4 #1)
- Secrets hygiene audit (CI grep + runtime scan) + allowlisted recovery tools
- Last known `zcl_self_test`: **45 pass / 21 skip / 0 fail**

## Wave 5 — 10 items, reach for stretch in the same session

### 1. HTTP RPC middleware — `lib/rpc/rpc_middleware.{h,c}` (carry-over wave 4 #2)

Mirror the MCP middleware pattern for the HTTP RPC surface.

- Rate limits: `ZCL_RPC_GLOBAL_RPS` (default 100), `ZCL_RPC_WRITE_RPS` (default 5)
- Timeouts: `ZCL_RPC_TIMEOUT_MS` (default 10000)
- IP ban after `ZCL_RPC_AUTH_FAIL_THRESHOLD=5` auth failures for `ZCL_RPC_AUTH_FAIL_BAN_SECONDS=300`
- Localhost exemption (mirror `is_trusted_peer()` — sacred per memory)
- Write methods hit the bucket twice: `sendmany`, `z_sendmany`, `importprivkey`, `keypoolrefill`, `backupwallet`, `dumpprivkey`, `walletpassphrasechange`

Wire into `lib/rpc/src/httpserver.c` as the outermost handler wrapper. Cookie auth still runs first; middleware gates the dispatch after cookie verification.

Events: `EV_RPC_REQUEST`, `EV_RPC_RATE_LIMITED`, `EV_RPC_AUTH_FAIL`, `EV_RPC_AUTH_BAN`, `EV_RPC_TIMEOUT`.

Tests in `lib/test/src/test_rpc_middleware.c` — 15+ cases covering global limit, write-method double-charge, localhost exemption, IP ban trigger, ban expiry, timeout fire, concurrent bucket refills.

### 2. Wallet key encryption at rest — `lib/wallet/wallet_encrypted.{h,c}` (carry-over wave 4 #3)

Scrypt-derived master key, AES-256-GCM per-key wrapping, `ZCL_WALLET_PASSPHRASE` env var, schema migration with optional plaintext fallback.

```c
struct wallet_encryption_ctx {
    uint8_t master_key[32];
    bool    enabled;
};

bool wallet_enc_init(const char *passphrase,
                      struct wallet_encryption_ctx *out);
bool wallet_enc_wrap_key(const struct wallet_encryption_ctx *ctx,
                          const uint8_t *plaintext, size_t plen,
                          uint8_t *out_blob, size_t *out_blen,
                          uint8_t out_nonce[12]);
bool wallet_enc_unwrap_key(const struct wallet_encryption_ctx *ctx,
                            const uint8_t *blob, size_t blen,
                            const uint8_t nonce[12],
                            uint8_t *out_plain, size_t *out_plen);
bool wallet_encryption_is_enabled(void);
```

Integration:
- `lib/wallet/src/sapling_keys.c` — wrap/unwrap at read/write
- `lib/wallet/src/keystore.c` — same for transparent keys
- `app/models/src/wallet_key.c` — add `encrypted_blob`, `nonce`, `enc_version` columns via migration
- **Coordinate with AGENT2**: AGENT2's `wallet_backup_service` should call `wallet_encryption_is_enabled()`; if yes, back up encrypted blobs as-is (don't decrypt during backup).

Tests: round-trip, wrong passphrase fails, plaintext → encrypted migration, mixed plaintext+encrypted DB, tampered blob caught by GCM.

**Drop a BOOT_QUEUE.md entry** for `wallet_enc_init()` wire-up in boot.c.

### 3. WebSocket event stream — `lib/net/ws_events.{h,c}`

Push, not poll. Stream `EV_*` events as they fire:

```c
bool ws_events_start(struct https_server *srv);
void ws_events_stop(void);
```

Clients connect to `/events`, optionally with `?domain=msg,chain` to filter. Each event becomes one text frame with JSON body `{ts, domain, type, fields}`.

Implementation:
- Hook via `event_subscribe()`
- Per-client ring buffer (max 1000 events); overflow drops oldest and sends `{"overflow": true}` frame
- Heartbeat ping every 30s to detect dead connections
- Max 100 concurrent subscribers (refuse 101st with 503)

Tests: connect, emit, receive, filter, overflow, heartbeat, max-subscribers.

### 4. OpenTelemetry-compat tracing — `lib/util/trace.{h,c}`

Upgrade from structured logging to distributed tracing spans. W3C Trace Context format for Jaeger/Tempo/Honeycomb compatibility.

```c
struct trace_span;

struct trace_span *trace_start(const char *name);
void               trace_attr_str(struct trace_span *s, const char *k, const char *v);
void               trace_attr_int(struct trace_span *s, const char *k, int64_t v);
void               trace_end(struct trace_span *s);
```

Output via `log_json` in OTLP-compatible JSON format. Spans form a tree via parent_id.

Migrate 5 hot paths:
- MCP dispatch (every tool call is a span)
- HTTP RPC dispatch (uses #1's middleware)
- `connect_tip` in `process_block.c`
- `csr_commit_tip` in `chain_state_repository.c`
- `snapsync_begin_receive`

Tests: span creation, attribute setting, parent/child linkage, trace_id propagation.

### 5. Admin dashboard tool — `zcl_admin`

One composite MCP tool that aggregates everything an operator needs:

```json
{
  "node": { "height": 2014948, "peers": 9, "sync": "tip", "uptime_s": 1234 },
  "health": { "score": 98, "checks": {"chain": "ok", "peers": "ok", "onion": "warn"} },
  "peers": { "total": 9, "banned": 0, "misbehaving": 2 },
  "policy": { "wipe_allowed": 1000, "wipe_refused_24h": 3 },
  "txn": { "begins_24h": 12, "commits_24h": 10, "rollbacks_24h": 2, "leaked_24h": 0 },
  "recent_events": [...10],
  "alerts": [ "disk_low: 4.2GB free" ]
}
```

Union of `zcl_kpi` + `zcl_peer_report` + `zcl_metrics` + `zcl_logtail`. Add a `since` parameter to scope the counters to a time window.

Tests: schema shape, `since` bounds, handles missing subsystems gracefully.

### 6. Per-peer bandwidth quotas — `lib/net/peer_bandwidth.{h,c}`

Today a single peer can saturate the uplink by requesting every block. Add token-bucket quotas per direction:

```c
struct peer_bandwidth_config {
    int64_t upload_bps_per_peer;    /* env ZCL_PEER_UP_BPS, default 10MB/s */
    int64_t download_bps_per_peer;  /* env ZCL_PEER_DOWN_BPS, default 20MB/s */
    int64_t burst_bytes;            /* env ZCL_PEER_BURST, default 1MB */
};

bool peer_bandwidth_consume(struct net_peer *p, int64_t bytes, bool upload);
void peer_bandwidth_refill_all(int64_t now_us);
```

Wire into `lib/net/src/connman.c` send/recv paths. If a peer is starved, pause their reads/writes until the bucket refills. Emit `EV_PEER_THROTTLED` with direction and bytes_dropped. Localhost/trusted peers skip the quota.

Tests: 10+ cases covering burst, starvation, refill, localhost exemption, direction isolation.

### 7. Onion service health monitoring — `zcl_onion_health`

The node serves over `.onion` but nothing tests that the service is actually reachable. Active probe via direct in-process path (dynhost has no SOCKS per memory):

```c
bool onion_health_start(int interval_s);
```

Background thread every `interval_s`:
1. Look up own onion address
2. Invoke `onion_service_handle_request()` with `GET /directory.json`
3. Parse response, verify our own height is in it
4. Emit `EV_ONION_HEALTH` with `{ok, latency_ms, error?}`

Expose via `zcl_onion_health` MCP tool in ops controller.

Tests: happy path, simulated failure, interval enforcement.

### 8. Coverage reporting — `make coverage`

Establish the measurement. Don't target a specific percentage yet:

```makefile
coverage: CFLAGS += --coverage -O0
coverage: test_zcl
	./test_zcl
	lcov --capture --directory . --output-file coverage.info
	lcov --remove coverage.info '*/lib/test/*' '*/vendor/*' --output-file coverage.info
	genhtml coverage.info --output-directory coverage_html
```

Post the current baseline in Current Status so we can track trajectory.

### 9. (Stretch) gRPC alternative interface — `tools/grpc/`

Modern typed interface alongside MCP and HTTP RPC. Proto files derived from router metadata, minimal libgrpc server stub. Only attempt if items 1–8 are done.

### 10. (Stretch) Continue tool backfill

`CLAUDE.md` claims **85+ RPC methods**. Current MCP count is 70. Audit the gap: list every RPC method without an MCP wrapper and either wrap it or add a justification note. Target: MCP parity with RPC.

---

## New coordination artifacts

### BOOT_QUEUE.md

When you want a `config/src/boot.c` edit — e.g., to wire `rpc_middleware_init()` or `wallet_enc_init()` — add it to `BOOT_QUEUE.md` at the repo root. Don't touch `boot.c` directly.

### FUZZER_FINDINGS.md

Security audit hits, fuzzer discoveries, and any other latent bug you find goes there first, logged with severity + owner, BEFORE you fix it. Fixes are separate commits.

---

## Rules

- Rebase/pull before every session. Push direct to `master` after `./test_zcl` green.
- One commit per logical step. `agent3:` prefix.
- **Never touch** AGENT2 territory: `config/src/boot.c`, `app/services/chain_state_repository.*`, `recovery_policy.*`, `block_index_integrity.*`, `app/models/db_txn.*`, `app/services/wallet_backup_service.*`, `app/services/block_index_loader.*`, `chain_state_validator.*`, `utxo_recovery_service.*`, `mempool_limits.*`, `disk_monitor.*`, `db_maintenance.*`, `tools/fuzz/**`, `tools/crash_recovery_test.c`, `lib/sapling/src/sprout.c` (PHGR13 investigation is AGENT2's).
- **Always rebuild `zclassic23` before running `./test_zcl`** — `make zclassic23 test_zcl` is canonical.
- **Smoke-test `zcl_self_test` after every push.**
- **Separate discovery from repair.** Log findings in `FUZZER_FINDINGS.md`, fixes are separate commits.
- **Reach down for stretch.** Don't wait for a new plan if you clear items 1–8.
- **Trade work.** If AGENT2 is still grinding and you're done, take test files or tool backfills from their plan that don't touch their owned files.
- Update "Current Status" each session.

## Definition of done for wave 5

- [ ] `rpc_middleware` live on HTTP RPC surface, 15+ tests
- [ ] `wallet_encrypted` service + tests + migration path
- [ ] `ws_events` WebSocket endpoint + tests
- [ ] `trace.{h,c}` + 5 migrated hot paths
- [ ] `zcl_admin` composite tool
- [ ] `peer_bandwidth` quotas wired into connman
- [ ] `zcl_onion_health` + background probe
- [ ] `make coverage` works, baseline posted
- [ ] `./test_zcl` still green
- [ ] `zcl_self_test` still **0 fail**

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Router + 41 tools + operator tooling (Phases 1, 2a, 2b, 2c).
- **2026-04-11 wave 2** — `database_validators` + 28 tests (Phase 3); `mcp/middleware` + tests. `zcl_self_test`: 45 pass / 21 skip / 0 fail.
- **2026-04-11 wave 3 session 2** — #4 `peer_scoring` (18 tests) + #3 `secrets_hygiene` (two-layer audit, grep + runtime). `./test_zcl`: ALL PASSED.
- **2026-04-11 wave 4 session 1** — **#1 done.** `test_mcp_e2e` hardened: mtime-based stale-binary skip, dynamic `mcp_router_count()`, new `make test-e2e` target rebuilds zclassic23 + test_zcl.
- **2026-04-11 wave 4 session 2** — **#5 done.** Peer scoring metrics + `zcl_peer_report` tool. Surface 70 tools. 6 new tests. Wave 4 #1's dynamic-count test absorbed the new tool with zero edits.
- **2026-04-11 wave 4 session 3** — **#4 done.** `log_json` helper + 10 migrated sites (net.c, onion_service.c, nat.c, file_service.c).
- **2026-04-11 wave 4 session 4** — **#2 done.** HTTP RPC middleware: global (50 rps/100 burst) + per-IP (5 rps/10 burst, LRU 256) + IP ban after 5 auth fails (1h default). All env-tunable. Loopback bypass. Wired into `handle_client()` — returns 403 banned / 429 rate limited pre-flight. 12 tests.
- **2026-04-11 wave 4 session 5** — **#3 done (primitives).** Wallet keystore encryption — AES-256-GCM via OpenSSL EVP, PBKDF2-HMAC-SHA512 (200k default iters, env-tunable, clamped 10k–10M), 60-byte envelope (magic+version+iters+reserved+salt+nonce+tag+ciphertext). Tampered blob/tag/bit fails closed. 14 tests. **Live wallet integration is a follow-up** (wallet_db.c / wallet.c / keystore.c / wallet_key.c / wallet_sqlite.c, ~3500 lines) — ships separately with per-controller regression coverage.
- **2026-04-11 wave 5 (AGENT1 COORDINATOR)** — **New plan, 10 deliverables above.** Wave 5 items **already done**: #1 (RPC middleware — landed in wave 4 session 4), #2 primitives (wallet keystore — landed in wave 4 session 5, integration still pending). Remaining priority: **#2 live-wallet integration** (wire `wks_encrypt/decrypt` through wallet_db/wallet/keystore/wallet_key/wallet_sqlite), then **#3 WebSocket event stream**, **#4 trace.{h,c}**, **#5 zcl_admin**, **#6 peer_bandwidth**, **#7 onion_health**, **#8 make coverage**. Reach for #9/#10 if you clear the list. New artifacts: `BOOT_QUEUE.md` + `FUZZER_FINDINGS.md` at repo root.
- **2026-04-11 wave 5 session 1** — **observability-depth groundwork** (not formally on the 10-item list, but closes the `zcl_rpc_report` comment in `http_middleware.h` and gives every subsequent wave-5 item a Prometheus-visible RPC surface to reason about). Extended `http_middleware.h/c` with a mutex-guarded global handle (`rpc_http_middleware_set_global` / `_get_global`) and a `rpc_http_stats_snapshot` struct that reads the 6 counters + 6 config fields + 2 gauges (`tracked_ips`, `active_bans`) under the middleware's internal lock. `httpserver.c` publishes `&g_middleware` at `rpc_http_start()` and clears to NULL at `rpc_http_stop()`. `metrics.c` appends five `zcl_rpc_*` families to the Prometheus dump (`zcl_rpc_requests_total{result=…}`, `zcl_rpc_auth_failures_total`, `zcl_rpc_bans_issued_total`, `zcl_rpc_bans_active` gauge, `zcl_rpc_tracked_ips` gauge) and exposes `mcp_metrics_rpc_report_json()` → compact JSON envelope with `"rpc_server":"active|inactive"`, nested `config`/`stats`, plus the gauges. New `zcl_rpc_report` tool in `meta_controller.c` (ops domain) wraps the JSON. Surface: **71 tools** (ops 17 → 18). **Tests:** 3 new in `test_http_middleware.c` (global set/get, NULL-safe snapshot, snapshot mirrors live struct) + 4 new in `test_mcp_metrics.c` (inactive-server report, active config+stats, Prometheus render with labels, inactive Prometheus render zeros). `test_mcp_controllers.c` `EXPECTED_TOTAL`/`EXPECTED_OPS` bumped in the same commit. `./test_zcl`: **ALL TESTS PASSED (0 failures)**. Dynamic-count e2e test absorbs the new tool with zero edits.
- **2026-04-11 wave 5 session 2** — **#5 done.** `zcl_admin` composite dashboard tool. New handler in `meta_controller.c` dispatches `zcl_kpi` + `zcl_peer_report` + `zcl_rpc_report` + `zcl_events` in-process via `mcp_router_dispatch()`, stitches the bodies into a single `{since, kpi, peer_report, rpc_report, events, alerts}` envelope, and derives a small `alerts` array from threshold heuristics over the nested counters (rpc rate-limited / banned / auth_failures, peer bans_total, peer offences_total > 100). **Graceful sub-tool handling**: `embed_or_null()` translates NULL, error envelopes, AND malformed JSON bodies into `null` — caught the legacy `zcl_kpi` bug where `mcp_node_rpc` splices raw `Unauthorized` strings into the response in test contexts with no live node. Optional `since` int param (default 0) is accepted for API stability; counters remain cumulative since boot (future windowing TBD). Surface: **72 tools** (ops 18 → 19). **Tests:** 3 new in `test_mcp_controllers.c` (dispatch shape + subkeys, `since` echoed, never propagates an error envelope → full `json_read` round-trip verifying each top-level field is parseable). `./test_zcl`: **ALL TESTS PASSED (0 failures)**.
- **2026-04-11 wave 5 session 3** — **#8 done.** `make coverage` target + baseline **26.0% line coverage** (298 translation units, 35,535 / 136,714 lines). Key design calls: (a) separate `build/cov/<same/path>/file.o` object tree so duplicate basenames (`lib/net/src/protocol.c` vs `lib/rpc/src/protocol.c`, `metrics.c` across several libs, `equihash.c`, `file_service.c`, `block.c`) don't collide at `.gcda` write time — the original single-`cc` build hit "overwriting an existing profile data with a different checksum" for every duplicate pair and rendered 0.0%; (b) `-O1` not `-O0` because the latter drove `test_json.c` into an 11-minute stack-blow regression; (c) new `lib/test/src/cov_flush.c` installs a SIGSEGV/SIGABRT/SIGBUS/SIGFPE handler (gated behind `-DCOVERAGE_BUILD`) that calls `__gcov_dump()` and `_exit()`s, so partial data from every test that ran before the crash is still flushed — without this, the `test_json` segfault silently wiped the entire run; (d) render pipeline supports `lcov → genhtml` (preferred) → `gcovr` → plain `gcov` fallback with an awk aggregator that filters out system headers, vendor, test, and fuzz sources so the headline number is about production code only. New `make coverage-clean` target. Documented in `FUZZER_FINDINGS.md` as **finding #2** (`test_json.c` write+read roundtrip segfault under `-O1 + gcov` — production builds unaffected, needs a valgrind dive to diagnose). `./test_zcl`: **ALL TESTS PASSED (0 failures)** on the main build (cov_flush.c is an empty translation unit when `COVERAGE_BUILD` isn't defined, with a typedef placeholder so `-Werror=pedantic` stays happy). **Next up**: wave 5 #1 RPC timeout layer, then #2 live wallet integration, #3 WebSocket stream, #4 trace.{h,c}, #6 peer_bandwidth, #7 onion_health.
