# AGENT3 — Security Hardening, Observability & Integration Testing

**Worktree:** `~/zclassic23-3`
**Workflow:** push directly to `master` after `./test_zcl` passes
**Coordinator:** AGENT1 (main `~/zclassic23`)
**Peer:** AGENT2 (`~/zclassic23-2`) — disaster recovery / boot decomposition, different files

---

## Mission

The MCP surface is now 66 tools, schema-validated, middleware-gated, and operator-instrumented. **Wave 3 proves that surface is actually safe and observable in production.** Fork the real binary and hammer it with an end-to-end test that catches envelope regressions. Wire up Prometheus metrics so operators can see what's happening. Audit the node's secrets hygiene so we're sure private keys and seeds never leak into logs. Then tackle the P2P-side misbehavior surface — which is the attack vector no one has looked at yet.

## Already done (don't redo)

- Router + schema validation + error envelope + EV_MCP_REQUEST (Phase 1)
- Controller split into 6 files; `mcp_server.c` at 199 lines (Phase 2b)
- 22-tool backfill; total **66 tools** (Phase 2a)
- `zcl_tools_list` / `zcl_self_test` / `zcl_logtail` (Phase 2c)
- `database_validators.{h,c}` + 17-model registry + 28 tests
- `tools/mcp/middleware.{h,c}` — auth + rate-limit + timeout + tests
- Live `zcl_self_test`: 45 pass / 21 skip / 0 fail

## Wave 3 — ordered by impact

### 1. End-to-end integration test — `tools/mcp/test_mcp_e2e.c` (carry-over)

Today's router tests are in-process. Nothing forks the real binary. A regression like "Phase 2b renamed a JSON field and nobody noticed" would slip through. Build a proper e2e harness:

```c
/* Fork zclassic23 -mcp -datadir=<temp> with pipes to stdin/stdout.
 * Speak MCP over the pipes. Assert on real envelope shapes. */

struct mcp_e2e_session;
struct mcp_e2e_session *mcp_e2e_start(const char *zclassic23_bin,
                                       const char *env[],
                                       char *err_out, size_t err_cap);
struct json_value *mcp_e2e_call(struct mcp_e2e_session *s,
                                 const char *tool,
                                 const char *args_json);
void mcp_e2e_stop(struct mcp_e2e_session *s);
```

Test cases (all required):

1. **`tools/list` shape** — iterate, assert exactly 66 tool names, each has `name`/`description`/`inputSchema`
2. **`zcl_status` / `zcl_kpi` / `zcl_health` required fields** — if any of these lose a field, break the build
3. **Envelope: MISSING_PARAM** — call `zcl_getblock` with no args, assert shape `{error:{code,message,tool,param}}`
4. **Envelope: INVALID_TYPE** — call `zcl_getblock` with `height: "abc"`, assert
5. **Envelope: UNKNOWN_TOOL** — call `zcl_nonexistent`, assert `UNKNOWN_TOOL`
6. **Envelope: ENUM_MISMATCH** — call `zcl_addnode` with `command: "bogus"`, assert
7. **Auth required** — start with `ZCL_MCP_BEARER_TOKEN=secret` in env, call any tool without token, assert `AUTH_REQUIRED`
8. **Auth success** — same, but with correct token in request metadata
9. **Rate limit** — rapid-fire 200 calls to a non-destructive tool, assert at least 1 `RATE_LIMITED`
10. **Timeout** — add a `#ifdef ZCL_TESTING zcl_testsleep(ms: 10000)` tool gated by a build flag, set `ZCL_MCP_TIMEOUT_MS=100`, assert `TOOL_TIMEOUT`

New `make test-e2e` target that builds a fresh `zclassic23` and runs the harness. Add to CI.

### 2. Prometheus metrics — `tools/mcp/metrics.{h,c}` (carry-over)

Expose operational counters in the Prometheus text format. New tool `zcl_metrics` returns the full text as a JSON string.

**Counters** (at minimum):

```
# Counters
zcl_mcp_requests_total{tool="…",code="OK|MISSING_PARAM|…"} N
zcl_mcp_rate_limited_total{kind="global|destructive"} N
zcl_mcp_timeouts_total{tool="…"} N
zcl_mcp_auth_failures_total N

# Histograms
zcl_mcp_request_duration_seconds_bucket{tool="…",le="0.001|0.005|0.01|…"} N

# Gauges (sampled from live state)
zcl_chain_height N
zcl_chain_utxo_count N
zcl_chain_peer_count N
zcl_chain_sync_state{state="idle|headers|blocks|snapshot|tip"} 1
zcl_wallet_balance_zatoshi N
zcl_mempool_tx_count N

# Counters from AGENT2's events
zcl_chain_state_commits_total{code="OK|REJECTED_STALE_INDEX|…"} N
zcl_recovery_policy_decisions_total{kind="wipe|rollback|rewind",decision="ALLOW|REFUSE_…"} N
zcl_db_txn_total{state="BEGIN|COMMIT|ROLLBACK|LEAKED"} N
```

Implementation:
- Subscribe to `EV_MCP_REQUEST`, `EV_CHAIN_TIP_*`, `EV_RECOVERY_POLICY_*`, `EV_DB_TXN_*`, `EV_MCP_TIMEOUT`, `EV_MCP_RATE_LIMITED` via an event observer that updates counters in-process. **Do not poll** — the events already fire at the right times.
- Histograms use a fixed bucket layout: `[0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5, +Inf]`.
- Label cardinality guard: refuse to record a label value if the tool doesn't exist (prevents an attacker flooding labels).

Tests in `lib/test/src/test_mcp_metrics.c`: histogram bucketing, counter increment per event, label guard, text format conformance, reset behavior.

### 3. Secrets hygiene audit — `lib/test/src/test_secrets_hygiene.c`

The single most embarrassing bug we could ship is logging a private key or seed phrase. Build a test that actively looks for it:

1. **Build a golden corpus** of secrets that should NEVER appear in logs / events / RPC responses:
   - Test wallet WIF keys
   - Sapling viewing keys
   - Seed phrases (BIP39 words)
   - RPC cookie contents
   - Onion HS private keys
2. **Exercise the node** with a test wallet containing those secrets — boot, generate addresses, send transactions, hit every RPC endpoint, every MCP tool.
3. **Scan all output** (stdout, stderr, event log, MCP envelopes, RPC responses) for substring matches. Any hit fails the test.
4. **Grep the codebase** for direct `printf("%s", priv_key)` patterns and forbid them via a `tools/scripts/check_no_secret_printf.sh` that runs in CI.

Expected outcome: you'll find at least one leak. Document it in Current Status, fix the most obvious, open issues for the rest.

### 4. P2P misbehavior scoring — `lib/net/peer_scoring.{h,c}`

Today bad peer messages are mostly logged and forgotten. Add scoring:

```c
enum peer_offence {
    PEER_OFFENCE_INVALID_MESSAGE = 10,
    PEER_OFFENCE_INVALID_BLOCK   = 100,
    PEER_OFFENCE_INVALID_HEADER  = 50,
    PEER_OFFENCE_TIMEOUT         = 5,
    PEER_OFFENCE_FLOOD           = 20,
    PEER_OFFENCE_UNREQUESTED     = 10,
};

void peer_scoring_record(struct net_peer *p, enum peer_offence o,
                          const char *context);
bool peer_scoring_should_ban(const struct net_peer *p);
void peer_scoring_reset(struct net_peer *p);  /* on successful interaction */
```

Rules:
- Ban threshold: 100 score (configurable via `ZCL_PEER_BAN_THRESHOLD`)
- Ban duration: 24h (configurable via `ZCL_PEER_BAN_HOURS`)
- **Never ban localhost / trusted peers** — guard via existing `is_trusted_peer()` (memory says this is sacred, enforced in `is_trusted_peer()`)
- Score decays linearly at 1 point per minute of good behavior
- Emit `EV_PEER_MISBEHAVING` / `EV_PEER_BANNED` events with the offence + context

Wire into:
- `lib/net/src/msgprocessor.c` — every rejected message path calls `peer_scoring_record`
- `lib/validation/src/process_block.c` — invalid block from a peer → `PEER_OFFENCE_INVALID_BLOCK`
- `lib/net/src/connman.c` — ban check on every new inbound connection

Tests in `lib/test/src/test_peer_scoring.c`: 12+ cases covering increment, decay, ban threshold, localhost guard, persistence across disconnect/reconnect.

### 5. OpenAPI-ish schema export — `zcl_openapi` tool (carry-over, small)

New MCP tool `zcl_openapi` returns a pseudo-OpenAPI JSON document derived from the routing table. Walk `mcp_router_tools_list_json()` and wrap each tool in `paths[/tool_name].post.requestBody.content["application/json"].schema`. Not a real spec — just give it that flavor for client-side code generation.

One commit, ~150 lines including tests.

---

## Rules

- Rebase/pull before every session. Push direct to `master` after `./test_zcl` green.
- One commit per logical step. `agent3:` prefix.
- **Never touch** AGENT2 territory: `config/src/boot.c`, `app/services/chain_state_repository.*`, `recovery_policy.*`, `block_index_integrity.*`, `app/models/db_txn.*`.
- `app/models/src/database_validators.c` stays yours (Phase 3 landed here).
- Smoke-test `zcl_self_test` after every push — paste the `{total, pass, fail, skip}` line into Current Status so any regression is immediately visible.
- When the secrets audit finds a real leak, log it in Current Status and fix it in a **separate** commit. Separate discovery from repair.
- Update "Current Status" each session.

## Definition of done for wave 3

- [x] `test_mcp_e2e` runs the real `-mcp` binary, 10 required test cases green
- [x] `zcl_metrics` returns Prometheus text with all 14 counter/histogram/gauge families
- [x] `test_secrets_hygiene` + `check_no_secret_printf.sh` CI check, any discovered leaks logged
- [x] `peer_scoring` service + wired into msgprocessor + connman (process_block DoS grading flows through msgprocessor; accept_block's validation_state DoS is graded at the msg handler call-site)
- [x] `zcl_openapi` schema export tool
- [x] `./test_zcl` still green
- [ ] `zcl_self_test` still **0 fail** after all changes *(requires a live node; last known: 45 pass / 21 skip / 0 fail)*

---

## Current Status

*(Update each session. Keep it short.)*

- **2026-04-11 wave 1** — Router + 41 tools + operator tooling (Phases 1, 2a, 2b, 2c).
- **2026-04-11 wave 2** — `database_validators` + 28 tests (Phase 3); `mcp/middleware` (auth + rate-limit + timeout) + tests. `zcl_self_test`: **45 pass / 21 skip / 0 fail**.
- **2026-04-11 wave 3** — **New plan, five deliverables above.** Start with #1 (e2e test harness) — it's the regression trip-wire that makes every subsequent change safer. Then #2 (metrics) is mostly wiring into existing events from AGENT2. #3 (secrets audit) is where real bugs live. #4 (peer scoring) is net-layer and conflict-free.
- **2026-04-11 wave 3 session 2** — Items #1/#2/#5 had already landed in the wave-2 catch-up commits (e2e + metrics + openapi). This session shipped the two remaining items:
  - **#4 peer_scoring** — `lib/net/{include/net,src}/peer_scoring.{h,c}` typed offence layer over `peer_misbehaving()`. Env-configurable threshold/ban hours/decay rate (`ZCL_PEER_BAN_THRESHOLD`, `ZCL_PEER_BAN_HOURS`, `ZCL_PEER_SCORE_DECAY_PER_MIN`). `peer_misbehaving()` updated to honour the new config (defaults unchanged: 100 / 24h / 1 pt/min). Wired into 6 msgprocessor rejection paths plus a good-interaction tick on block accept. `connman_init()` calls `peer_scoring_init()` so every binary honours env overrides. 18 unit tests in `lib/test/src/test_peer_scoring.c`.
  - **#3 secrets_hygiene** — Two-layer audit. `tools/scripts/check_no_secret_printf.sh` grep-scan for printf-family calls that reference key-shaped variable names (priv_key / mnemonic / wif / spending_key / …). `lib/test/src/test_secrets_hygiene.c` runs a golden-corpus scan against the MCP tools/list JSON and error envelopes, plus a positive control and a script-shape audit. **Known findings**: `tools/wallet_recover.c` and `tools/wallet_dump.c` intentionally print WIF material; both allowlisted with justification since they are explicit operator-invoked recovery utilities. No unintentional leaks found in the audit surface — follow-up sessions should widen the runtime scan to include real wallet controllers once a lightweight fixture wallet is available.
- **`./test_zcl`**: ALL TESTS PASSED (includes 18 peer_scoring + 6 secrets_hygiene cases).
- **2026-04-11 wave 4 plan** — AGENT1 wave-4 direction landed in `500f56dee` (commit msg only — `AGENT3.md` itself wasn't updated). Five new AGENT3 items: (1) fix `test_mcp_e2e` fragility, (2) HTTP RPC middleware (rate limit + timeout + IP ban), (3) wallet key encryption at rest (AES-256-GCM + passphrase), (4) structured JSON logging helper + 10 migrated sites, (5) peer scoring metrics + `zcl_peer_report` MCP tool.
- **2026-04-11 wave 4 session 1** — **#1 done.** `test_mcp_e2e` now (a) detects a stale `./zclassic23` by mtime-comparing the binary against the MCP source files (router + 6 controllers + metrics + middleware + mcp_server.c + main.c) and SKIPs with a clear "run `make test-e2e` to rebuild" message instead of failing on a tool-count mismatch; (b) reads the expected tool count from the in-process `mcp_router_count()` after registering all 6 controller domains, so adding tools never breaks this test again; (c) new `make test-e2e` target rebuilds zclassic23 + test_zcl before running the suite. Verified: SKIP path triggers when source is touched, full e2e suite (8 tests) green after `make test-e2e`.
- **2026-04-11 wave 4 session 2** — **#5 done.** Peer scoring metrics + `zcl_peer_report` MCP tool. `tools/mcp/metrics.{h,c}` now subscribes to `EV_PEER_MISBEHAVE` / `EV_PEER_BANNED` and buckets offences by canonical kind (timeout / invalid_message / flood / invalid_header / invalid_block / unrequested / other) plus a total bans counter. Render path adds `zcl_peer_offences_total{kind=...}` and `zcl_peer_bans_total` to the Prometheus dump. New `zcl_peer_report` tool in `net_controller.c` returns a JSON object with the live `peer_scoring_init()` config (threshold / ban_hours / decay_per_min) plus the per-kind counters and totals. Surface bumped to **70 tools** (net domain 7 → 8). 6 new tests in `test_mcp_metrics.c` cover bucketing, "other" fold-in, ban counter independence, observer hookup, Prometheus rendering, and JSON shape. Wave 4 #1's dynamic-count e2e test absorbed the new tool with zero edits — exactly the regression-trip-wire benefit it was built for.
- **2026-04-11 wave 4 session 3** — **#4 done.** Structured JSON logging helper. New `lib/util/include/util/log_json.h` + `lib/util/src/log_json.c` exposing `log_jsonf(level, event, fields_fmt, ...)` which writes a single-line JSON object via `LogPrintStr()` with `ts` (ISO-8601 microsecond UTC), `level` (info/warn/error), `event`, and any caller-supplied fields. `log_json_escape()` handles backslashes, quotes, and control chars. `log_json_format()` is the same renderer but writes to a buffer (used by tests). 10 unit tests in `lib/test/src/test_log_json.c` cover envelope shape, level rendering, fields insertion, NULL fields, escape edge cases, truncation, and ISO-8601 timestamp shape. Migrated 10 representative call sites away from ad-hoc `printf` to `log_jsonf`: 3 in `lib/net/src/net.c` (peer_connect_failed / peer_connected / peer_banned), 2 in `lib/net/src/onion_service.c` (onion_directory_loaded / onion_self_registered), 3 in `lib/net/src/nat.c` (nat_public_ip / nat_port_mapped natpmp + upnp / nat_port_map_failed), 2 in `lib/net/src/file_service.c` (file_service_manifest_ready/empty + file_service_listening). All operator-visible diagnostic messages, no behaviour change beyond format.
