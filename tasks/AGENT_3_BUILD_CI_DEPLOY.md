# Agent 3 — Build, CI, Deploy Hardening (next after wallet guardrails)

**Pick up when:** your `a3/wallet-controller-guardrails` PR has merged to master and you've pulled. **It's merged — `93ad65502..e4649ebbb` are on master. Go now.**

**D8 is DONE.** Agent 2 fixed `node_health_service.c` `get_rss_kb` silent `return -1` as a bonus on their persistence-hardening PR (`7a955c0dd`). Verify with `grep -n 'return -1' app/services/src/node_health_service.c` — should be empty. Strike D8 from your plan. Do the other 8.

**Next after this PR merges:** [`AGENT_3_RAILS_CONTROLLERS.md`](./AGENT_3_RAILS_CONTROLLERS.md) — Track C of the Rails parity plan (strong params everywhere, uniform error envelope, before_action filters, after_commit hooks, controller AR migration, OpenAPI from validators).

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P1.1–P1.4, §P3 systemd, §P3.4 tests, §R2.11.

**Worktree:** `~/zclassic23-3`
**Branch:** `a3/build-ci-deploy-hardening`
**Base:** `origin/master`
**Directive:** keep pushing to master; do not stand down between assignments.

---

## Mission, one sentence

Make `make ci` a real gate (lint fails, not warns), make `make deploy` impossible to falsely claim success, and port the live systemd hardening into the repo so it doesn't silently regress.

---

## Scope

### Files you own

- `Makefile`
- `deploy/zclassic23.service`
- `deploy/zclassicd-rhett.service` **(create — copy from `~/.config/systemd/user/zclassicd-rhett.service` first)**
- `lib/util/include/util/ar_step_readonly.h` **(create)**
- `app/services/src/node_health_service.c` — fix the two lint-failing silent `return -1;`s currently on master (`get_rss_kb` around lines 71, 82)
- `tools/deploy_verify.sh` **(create)** — post-restart RPC health check used by `make deploy`
- `lib/test/src/test_make_lint_gates.c` **(create)**
- Add lint-silencing annotations (`// raw-sql-ok: read-only, agent-N`) in any file whose raw `sqlite3_step` reads you don't migrate

### Files you MUST NOT touch

- Anything owned by Agent 2's current scope (`lib/coins/src/utxo_commitment.c`, `lib/storage/src/coins_view_sqlite.c`, `lib/storage/src/dbwrapper.c`, `app/models/src/database.c` quarantine + migration helpers, `lib/wallet/src/wallet_sqlite.c`). If Agent 2 is still in flight, leave their files alone.

---

## Deliverables

### D1. Enable `-DZCL_AR_ENFORCE`

Add `-DZCL_AR_ENFORCE` to `CFLAGS` in `Makefile` (around line 37). Compile. For violations in files you don't own, add `// raw-sql-ok: read-only, <scope>` annotations so master builds clean. The lint pattern already exempts that comment.

### D2. `check-raw-sqlite` must FAIL, not WARN

`Makefile:506-514` currently emits `WARNING:` and exits 0. Change to `echo "FAIL: …"; exit 1`. Must be on the same commit as, or after, D1 so master stays green.

### D3. `AR_STEP_ROW_READONLY` wrapper

Create `lib/util/include/util/ar_step_readonly.h` with a macro that wraps a single `sqlite3_step`, returns the step result, has no side effects, and is exempted by the `check-raw-sqlite` grep. Update the lint grep to exempt it alongside `AR_STEP_ROW` / `AR_STEP_DONE`.

### D4. `make deploy` hardening

Replace the current `deploy:` target (`Makefile:298-302`) with:

```make
deploy: lint zclassic23
	@./tools/wal_checkpoint $(HOME)/.zclassic-c23/node.db || { echo "WAL checkpoint failed"; exit 1; }
	@install -m 644 deploy/zclassic23.service $(HOME)/.config/systemd/user/zclassic23.service
	@systemctl --user daemon-reload
	systemctl --user restart zclassic23
	@./tools/deploy_verify.sh
```

`tools/deploy_verify.sh`: poll `zcl-rpc getblockcount` every 2 s for up to 30 s. On success print `Deployed + RPC live at block N.`; on timeout exit 1. Must be executable (`chmod +x`).

If `tools/wal_checkpoint` does not exist (it's referenced in HARDENING_CHECKLIST P4.1 but was also flagged as unsafe), use `sqlite3 $(HOME)/.zclassic-c23/node.db "PRAGMA wal_checkpoint(TRUNCATE);"` inline.

### D5. Port live systemd hardening into `deploy/zclassic23.service`

Live unit at `~/.config/systemd/user/zclassic23.service` has `OOMScoreAdjust=-500` and `MemoryHigh=6G` that are NOT in the repo template. Port them and add:

```
TimeoutStopSec=300
KillMode=control-group
StartLimitIntervalSec=300
StartLimitBurst=3
PrivateTmp=yes
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=%h/.zclassic-c23 %h/zclassic23/vendor/tor/etc
```

Confirm `ProtectSystem=strict` doesn't break Tor's working dir — check `journalctl --user -u zclassic23 -f` during first deploy. If Tor writes to `/tmp/zclassic23-tor` or similar, add that path. If `PrivateTmp=yes` breaks Tor, drop it and note why in the commit.

Do NOT add `WatchdogSec` — requires `sd_notify` plumbing in main loop, out of scope here.

### D6. `zclassicd-rhett.service` lock-conflict guard

The current systemd unit restart-loops against a manual zclassicd instance. Add `ExecStartPre=/usr/bin/pgrep -x zclassicd` so the unit fails fast and clean instead of thrashing. Preserve the `OOMScoreAdjust=-500`, `MemoryHigh=6G`, and `-nofastsync` ExecStart arg.

### D7. Wire `crash_recovery_test` into `make ci`

`tools/crash_recovery_test.c` only runs when `ZCL_CRASH_DATADIR` is set. Add a `test-crash` Makefile target that seeds a throwaway datadir, runs the test, tears down. Make `ci:` depend on `test-crash`.

### D8. ~~Fix the two silent `return -1` lines on master~~ — DONE BY A2

Shipped in `7a955c0dd` as a bonus on Agent 2's persistence-hardening PR. `make lint` is already green.

### D9. Self-test the lint gate

`lib/test/src/test_make_lint_gates.c`: under `#ifdef ZCL_TESTING`, shell out to `make check-raw-sqlite` with a fixture file under `lib/test/fixtures/` that contains a raw `sqlite3_step`; assert exit code 1. Prevents anyone from "fixing" the lint by loosening the grep.

---

## Done when

- [ ] `make lint` exits 0.
- [ ] `make ci` exits 0 and runs `test-crash`.
- [ ] `grep -n 'sqlite3_step' app/ tools/ --include='*.c'` lines each have either `AR_STEP_*` or `// raw-sql-ok:` annotation.
- [ ] `make deploy` with a lint-failing binary exits before touching systemd.
- [ ] `make deploy` runs wal_checkpoint → systemctl restart → deploy_verify; fails loudly if RPC isn't live.
- [ ] `diff deploy/zclassic23.service ~/.config/systemd/user/zclassic23.service` empty after `make deploy`.
- [ ] `zclassicd-rhett.service` doesn't restart-loop against a manual zclassicd.
- [ ] PR title: `a3: build/CI/deploy hardening (P1.1-P1.4, P3.4)`.

---

## Gotchas

- D1 will flood master with compile errors. Resist migrating files outside your scope. `// raw-sql-ok` comments are the right short-term answer; Agent 2's persistence work may migrate several database.c call sites that overlap, so leave their files annotated-only.
- `PrivateTmp=yes` + Tor is a coin-flip. If Tor's unix socket is in `/tmp`, the private-tmp namespacing breaks it. Test once, document, choose.
- If `tools/wal_checkpoint.c` binary exists but is the destructive one flagged in HARDENING §P4.1, don't call it — inline the PRAGMA instead and add a TODO for the tool-safety cleanup (separate scope).
- `ExecStartPre=pgrep` in D6 will fail the unit fast when a manual zclassicd is running. That's the intended outcome. Document the operator recovery path in the commit message: `kill <manual-pid>; systemctl --user reset-failed zclassicd-rhett; systemctl --user start zclassicd-rhett`.

---

## Hand-off

```
cd ~/zclassic23-3
git push origin a3/build-ci-deploy-hardening
gh pr create --title "a3: build/CI/deploy hardening (P1.1-P1.4, P3.4)" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §P1.1-P1.4, §P3 systemd, §P3.4, §R2.11.

- -DZCL_AR_ENFORCE on; raw-sqlite lint FAILs
- AR_STEP_ROW_READONLY wrapper for read sites
- make deploy is lint-gated, WAL-checkpointed, RPC-verified
- deploy/zclassic23.service matches live hardening (OOM, Memory, Timeout, KillMode, StartLimit, PrivateTmp, NoNewPrivileges, ProtectSystem)
- zclassicd-rhett.service won't restart-loop against a manual instance
- crash_recovery_test wired into make ci
- Fixes the two silent return -1 lines currently on master in node_health_service.c

## Plan
See HARDENING_CHECKLIST.md §P1, §P3, §R2.11.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

When this merges, pull master and request the next assignment — don't stand down.
