# Agent 7 — Network, MCP Handlers, Tool Safety

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P3.1–P3.3, §P3 systemd notes, §P4, §R2.4, §R2.6.

**Worktree:** `~/zclassic23-7`
**Branch:** `a7/network-mcp-tools`
**Base:** `origin/master`
**Dependencies:** none.

---

## Mission, one sentence

Fix the verified network hang/silent-fail paths, stop MCP handlers from crashing on their own error paths, and add safety rails to every destructive `tools/*`.

---

## Scope

### Files you own

- `lib/net/src/download.c` — P3.1 negative-age timeout
- `lib/net/src/tor_integration.c` — P3.2 bootstrap-failure event
- `lib/net/src/msgprocessor.c` — P3.3 addr-message localhost reject
- `tools/mcp/controllers/*.c` — R2.4 malloc fallthrough, R2.6 params builder
- `tools/mcp/include/mcp/params_builder.h` **(create)**
- `tools/mcp/src/params_builder.c` **(create)**
- `tools/wal_checkpoint.c` — P4.1 gate destructive DELETE
- `tools/*.sh` and `tools/*.c` — add `--force` / dry-run to destructive ones
- `lib/test/src/test_download_timeout_skew.c` **(create)**
- `lib/test/src/test_tor_bootstrap_fail.c` **(create)**
- `lib/test/src/test_addrman_localhost_reject.c` **(create)**
- `lib/test/src/test_mcp_handler_null_body.c` **(create)**
- `lib/test/src/test_mcp_params_builder.c` **(create)**

### Files you MUST NOT touch

- `lib/wallet/`, `app/models/src/wallet*` (agents 2/3)
- `Makefile`, systemd units (agent 4)
- `lib/coins/`, `lib/storage/`, `app/models/src/database.c` (agent 5)
- `lib/validation/`, `app/services/src/snapshot_sync_service.c`, `lib/storage/src/disk_block_io.c` (agent 6)
- `lib/sapling/` (agent 8)

---

## Deliverables

### D1. Fix download timeout under clock skew (P3.1)

`lib/net/src/download.c:317-318`:
```c
int64_t age = now - s->request_time;
if (age < dl_get_request_timeout_secs()) continue;
```

If `s->request_time` is ever ahead of `now` (clock jump back, corrupted cast), `age` is negative and the slot never times out. Fix:

```c
int64_t age = now - s->request_time;
if (age < 0) age = dl_get_request_timeout_secs();  /* treat skew as "timed out" */
if (age < dl_get_request_timeout_secs()) continue;
```

Regression test: set `request_time` to `now + 3600`, assert the slot is reaped.

### D2. Tor bootstrap failure is observable (P3.2)

`lib/net/src/tor_integration.c:202-244` — when the Tor pthread exits or monitor times out, `g_tor_running` becomes false but nothing else knows.

1. Emit `EV_TOR_BOOTSTRAP_FAILED` on thread exit (add to the event enum).
2. `zcl_onion_status` health check must report `healthy=false` with `reason="tor_thread_exited"`.
3. On failure, schedule one restart attempt after 60s (bounded: max 3 tries within 1 hour). Log each attempt. Do NOT restart indefinitely — if 3 tries fail, stay down and let the operator intervene.

Regression test (`test_tor_bootstrap_fail.c`): test-only hook injects `tor_thread_fail`, assert `EV_TOR_BOOTSTRAP_FAILED` fired, assert onion health went unhealthy.

### D3. addrman rejects peer-supplied localhost (P3.3)

`lib/net/src/msgprocessor.c:750-751` `process_addr()` currently calls `addrman_add(&mgr->addrman, &addr, &source, 0)` with no validation of the supplied address.

1. Before insert: reject `127.0.0.0/8`, `::1/128`, `10/8`, `172.16/12`, `192.168/16`, `169.254/16` (RFC1918 + link-local) unless `is_trusted_peer(source)` returns true.
2. For public addresses where the peer is claiming *its own* address is RFC1918, that's also suspicious; log + drop.
3. Preserve existing behaviour for trusted peers (localhost + configured `-addnode=` entries).

Regression test (`test_addrman_localhost_reject.c`): feed a malicious `addr` message from an untrusted peer claiming `127.0.0.1`, assert addrman size is unchanged.

### D4. MCP handler malloc fallthrough (R2.4)

`tools/mcp/controllers/net_controller.c:124-139` — `h_zcl_onion_health` logs malloc failure and falls through to a NULL dereference. Add `return -1;` after the log.

Grep-and-audit pass: `grep -rn 'zcl_malloc\|malloc' tools/mcp/controllers/`. For every site, verify the branch after a failed allocation returns immediately. Fix all hits. Typical pattern:

```c
if (!body) {
    res->error = MCP_ERR_INTERNAL;
    snprintf(res->error_message, sizeof(res->error_message), "alloc failed");
    LOG_ERR("mcp.xxx", "malloc failed");   /* LOG_ERR already returns -1 per log_macros.h */
}
/* Only add explicit return -1 here if LOG_ERR call is not the macro form. Audit each site. */
```

Regression test (`test_mcp_handler_null_body.c`): inject malloc failure via ASAN allocator override (or `__wrap_zcl_malloc` linker trick), call each handler, assert no NULL dereference.

### D5. MCP params builder (R2.6)

Untrusted JSON strings currently flow into fixed-size `snprintf` calls with ignored return values at `tools/mcp/controllers/{app,wallet,ops}_controller.c`.

Create `tools/mcp/src/params_builder.c` exporting:

```c
int mcp_build_params(char *buf, size_t buflen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* Returns 0 on success, -1 on truncation (with buf[0]=0), -2 on JSON-escape failure. */

int mcp_json_escape(char *dst, size_t dst_len, const char *src, size_t src_len);
/* Escapes src for inclusion inside a JSON string literal; returns bytes written or -1 if dst too small. */
```

Migrate every `snprintf(params, ...)` in `tools/mcp/controllers/` to the helper. For string params, prefer the escape helper over `"%s"` to prevent quote-injection in RPC JSON.

Regression test (`test_mcp_params_builder.c`): feed a 2KB string into a 256-byte buffer, assert `-1` return; feed a string with embedded `"` and `\\`, assert escaping; feed clean input, assert round-trip.

### D6. `tools/wal_checkpoint.c` — neuter destructive path (P4.1)

Current `wal_checkpoint.c:51,61-62` can `DELETE FROM utxos`. That is not what a WAL-checkpoint tool should do. Either:

- Rename the offending path to `tools/unsafe_utxo_wipe.c` and require `--i-know-what-im-doing` flag, OR
- Remove it from this tool; leave `wal_checkpoint.c` as a pure `PRAGMA wal_checkpoint(TRUNCATE)` caller.

Prefer removal. Add:
- `--dry-run` flag that prints what would happen.
- Exit 0 only after printing `WAL checkpoint OK (pages=N, size=M)`.
- Refuse to run if the target DB is held by a running process (check with `lsof` or `fcntl(F_GETLK)`).

### D7. Audit all destructive `tools/*`

Grep `tools/*.c` and `tools/*.sh` for `DELETE`, `DROP`, `rm `, `unlink(`, `remove_all`. Each hit:

- Require a `--force` flag (or `-y` short form) to run non-interactively.
- Without `--force`, print what would happen and exit 0.
- Add a one-sentence header comment naming the destructive op.

Minimum targets to re-audit: `tools/backfill_shielded.sh`, `tools/bench_fresh_sync.c/.sh`, `tools/export_snapshot.c`, anything else you find.

---

## Done when

- [ ] `download.c` negative-age comparison cannot skip timeout.
- [ ] Tor bootstrap failure surfaces in `zcl_onion_status.health`; bounded restart attempts.
- [ ] addrman rejects peer-supplied localhost / RFC1918 from untrusted peers.
- [ ] No MCP handler dereferences a possibly-NULL body after a failed malloc.
- [ ] Every MCP RPC params construction goes through `mcp_build_params` and handles truncation.
- [ ] `tools/wal_checkpoint` has no destructive DELETE path.
- [ ] Every destructive tool requires `--force`.
- [ ] 5 new tests pass under `./test_zcl`.
- [ ] `make lint` green.
- [ ] PR title: `a7: network timeouts, MCP safety, destructive-tool guards`

---

## Gotchas

- `LOG_ERR` in `util/log_macros.h:35-39` expands with `return -1;` — which means calling it looks like a statement but actually exits the current function. That is why R2.4 exists: some handlers write `LOG_ERR(...)` without the `do{}while(0)` wrapper and gcc is happy to let the code after it be reachable. Verify each MCP handler's `LOG_ERR` invocation is the macro form, not a function-style call; both are syntactically valid.
- RFC1918 detection needs v4-mapped v6 handling (`::ffff:10.0.0.0`). Use the existing `net_is_private(const struct CService*)` helper if present; if not, add one.
- For D7, `tools/export_snapshot.c` writes to the filesystem but isn't strictly destructive; it's fine to leave it without `--force`. Focus on anything that modifies node.db, wallet.dat, or blk*.dat.
- `test_mcp_handler_null_body.c` may need to run in its own binary if ASAN allocator overrides conflict with production malloc tests. Link it as `test_zcl_mcp_oom` with its own main.

---

## Hand-off

```
cd ~/zclassic23-7
git push origin a7/network-mcp-tools
gh pr create --title "a7: network timeouts, MCP safety, destructive-tool guards" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §P3.1-P3.3, §P4, §R2.4, §R2.6.

- download.c negative-age timeout fix
- Tor bootstrap failure surfaces in onion_status health
- addrman rejects peer-supplied localhost / RFC1918 from untrusted peers
- MCP handler malloc fallthrough audit + fix
- mcp_build_params helper with truncation + JSON escape
- wal_checkpoint.c destructive DELETE removed
- All destructive tools/* now require --force

## Plan
See HARDENING_CHECKLIST.md §P3.1-P3.3, §P4, §R2.4, §R2.6.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
