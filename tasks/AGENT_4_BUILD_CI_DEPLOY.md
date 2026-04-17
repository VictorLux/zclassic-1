# Agent 4 — Build, CI, Deploy Hardening

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §P1.1–P1.4, §P3 systemd, §P3.4 tests, §R2.11. This doc assumes you have.

**Worktree:** `~/zclassic23-4`
**Branch:** `a4/build-ci-deploy-hardening`
**Base:** `origin/master`
**Dependencies:** none. Go first. Every other agent benefits from lint actually failing on violations.

---

## Mission, one sentence

Turn `make ci` into a real gate (lint fails, not warns), make `make deploy` impossible to falsely claim success, and port the live systemd hardening into the repo so it doesn't silently regress on the next deploy.

---

## Scope

### Files you own

- `Makefile` (top-level)
- `deploy/zclassic23.service`
- `deploy/zclassicd-rhett.service` **(create — copy from `~/.config/systemd/user/zclassicd-rhett.service` first)**
- `lib/util/include/util/ar_step_readonly.h` **(create)**
- `app/services/src/node_health_service.c` — fix the two lint-failing silent `return -1;`s that exist today
- `lib/test/src/test_make_lint_gates.c` **(create)** — trivially shell-outs to `make check-*` targets and asserts exit codes
- `tools/deploy_verify.sh` **(create)** — post-restart RPC health check used by `make deploy`
- `lib/test/CMakeLists.txt` / `lib/test/Makefile` — wire new tests

### Files you MUST NOT touch

- Anything under `lib/wallet/` (agents 2/3)
- Anything under `lib/coins/`, `lib/storage/`, `app/models/src/database.c` quarantine paths (agent 5)
- Anything under `lib/validation/`, `app/services/src/snapshot_sync_service.c` (agent 6)
- Anything under `lib/net/`, `tools/mcp/`, `tools/wal_checkpoint.c` (agent 7)
- Anything under `lib/sapling/` (agent 8)

If migrating a raw `sqlite3_step` read to satisfy the new FAIL gate requires editing files owned by agents 5/6/7/8, **mark the line `// raw-sql-ok: read-only, agent-N scope`** instead. Do not edit across boundaries.

---

## Deliverables

### D1. Enable `-DZCL_AR_ENFORCE`

- `Makefile:37` add `-DZCL_AR_ENFORCE` to `CFLAGS`.
- Compile. Fix any compile failures in files you own.
- For files owned by other agents, do NOT convert — add `// raw-sql-ok: read-only, agent-N` comments so lint passes but the migration is tracked.
- Goal: master compiles clean with the flag set.

### D2. `check-raw-sqlite` FAILS not WARNS

- `Makefile:506-514` — change the WARNING path to `exit 1`.
- At this point master must still pass: do D1's raw-sql-ok annotation pass first, then flip this switch in the same commit.

### D3. Add `AR_STEP_ROW_READONLY` wrapper

- Purpose: give read-only call sites (explorer, factoids, blockchain query) a first-class macro that doesn't trigger raw-sqlite lint and skips write-path hooks.
- Location: `lib/util/include/util/ar_step_readonly.h`
- Behaviour: exactly one `sqlite3_step` call, no side-effects, returns the step result; lint pattern must exempt `AR_STEP_ROW_READONLY`.
- Update the grep in `Makefile:507` to exempt `AR_STEP_ROW_READONLY` as it already does for `AR_STEP_ROW` / `AR_STEP_DONE`.

### D4. `make deploy` hardening

Replace `deploy:` target (currently `Makefile:298-302`) with:

```make
deploy: lint zclassic23
	@./tools/wal_checkpoint $(HOME)/.zclassic-c23/node.db || { echo "WAL checkpoint failed"; exit 1; }
	@install -m 644 deploy/zclassic23.service $(HOME)/.config/systemd/user/zclassic23.service
	@systemctl --user daemon-reload
	systemctl --user restart zclassic23
	@./tools/deploy_verify.sh
```

`tools/deploy_verify.sh`: poll `zcl-rpc getblockcount` every 2 s for up to 30 s; fail-hard (exit 1) if not answering; print `Deployed + RPC live at block N.` on success. Must be executable (`chmod +x`).

Safety: `lint` is now a `.PHONY` dependency of deploy; `wal_checkpoint` failure fails the deploy (does not restart a potentially-sick process).

### D5. Port live systemd hardening into `deploy/zclassic23.service`

Live unit at `~/.config/systemd/user/zclassic23.service` currently has `OOMScoreAdjust=-500` and `MemoryHigh=6G` that are NOT in the repo template. Next `make deploy` silently removes them. Port them and add:

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

Verify the `ProtectSystem=strict` + `ReadWritePaths=` combo against Tor's actual data dir. If dynhost writes anywhere else, add it.

Do NOT add `WatchdogSec` yet — plumbing `sd_notify` heartbeat into the main loop is out of scope for this agent.

### D6. `zclassicd-rhett.service` lock-conflict guard

The manual-vs-systemd zclassicd lock conflict is an operational papercut. Add `ExecStartPre=/usr/bin/pgrep -x zclassicd` that refuses to start if a zclassicd already exists, so the systemd unit doesn't restart-loop against a manual instance.

Keep `OOMScoreAdjust=-500`, `MemoryHigh=6G`, `-nofastsync` on ExecStart — those were added after the 2026-04-16 OOM incident.

### D7. Wire `crash_recovery_test` into `make ci`

- `tools/crash_recovery_test.c` already exists but only runs if `$ZCL_CRASH_DATADIR` is set.
- Add a `test-crash` target to `Makefile` that:
  1. Creates a scratch datadir `/tmp/zcl-crash-$(date +%s)`
  2. Runs the test binary with that datadir
  3. Tears down the scratch dir on exit
- Add `test-crash` as a dependency of `ci:` (near `Makefile:546`).

### D8. Fix the two lint-failing lines currently on master

`app/services/src/node_health_service.c:71,82` — `get_rss_kb` has two `return -1;` without a log. Replace with `LOG_ERR("health", "read /proc/self/status: %s", strerror(errno));` pattern or inline the fprintf + return. `make lint` must exit 0 after your commits.

### D9. Test the lint gate itself

`lib/test/src/test_make_lint_gates.c`: fork, exec `make check-raw-sqlite`, assert exit code 1 if a test fixture file with a raw `sqlite3_step` is present (place fixture under `lib/test/fixtures/`). This stops anyone from "fixing" the lint by loosening the grep.

---

## Done when

- [ ] `make lint` exits 0.
- [ ] `make ci` exits 0 and includes `test-crash`.
- [ ] `grep -n 'sqlite3_step' app/ tools/ --include='*.c'` lines all have either `AR_STEP_*` wrapper or `// raw-sql-ok:` annotation with agent owner.
- [ ] `make deploy` with a known-bad binary (break a lint rule) fails before touching systemd.
- [ ] `make deploy` with a good binary runs wal_checkpoint, restarts, and runs deploy_verify.sh; on a simulated no-RPC binary it fails loudly.
- [ ] `diff deploy/zclassic23.service ~/.config/systemd/user/zclassic23.service` is empty on disk after running `make deploy` once.
- [ ] `zclassicd-rhett.service` does not restart-loop when a manual zclassicd is already running (refuses to start cleanly instead).
- [ ] Commit sequence is 6–12 logical commits, imperative subjects.
- [ ] PR title: `a4: build/CI/deploy hardening (P1.1-P1.4, P3.4)`

---

## Gotchas

- D1 will produce a flood of compile errors. Resist the urge to migrate files outside your scope — the `// raw-sql-ok` annotation is the correct short-term answer. Agents 5–8 own their migrations.
- `make lint` has a separate check `check-silent-errors-services` already failing today on `node_health_service.c`. Fix that (D8) in one of your earliest commits so subsequent CI runs are green.
- `PrivateTmp=yes` in systemd may break Tor's unix-domain sockets if any are created in `/tmp`. Test with `journalctl --user -u zclassic23 -f` during first deploy. If it breaks, add `ReadWritePaths=/tmp/zclassic23-tor` or drop to `PrivateTmp=no`.
- The `ExecStartPre=pgrep` gate in D6 will fail the unit fast when pgrep finds another zclassicd. That is the intended behaviour. Document the operator recovery: `kill <manual-pid>; systemctl --user reset-failed zclassicd-rhett; systemctl --user start zclassicd-rhett`.

---

## Hand-off

```
cd ~/zclassic23-4
git push origin a4/build-ci-deploy-hardening
gh pr create --title "a4: build/CI/deploy hardening (P1.1-P1.4, P3.4)" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §P1.1-P1.4 + P3.4 + R2.11.

- -DZCL_AR_ENFORCE on, raw-sqlite lint FAILs
- AR_STEP_ROW_READONLY wrapper for reads
- make deploy is now lint-gated, WAL-checkpointed, RPC-verified
- deploy/zclassic23.service now matches live (OOM, Memory, Timeout, KillMode, StartLimit, PrivateTmp)
- zclassicd-rhett.service won't restart-loop against a manual instance
- crash_recovery_test wired into make ci
- Fixes the two silent return -1 lines already on master

## Plan
See HARDENING_CHECKLIST.md §P1, §P3, §R2.11.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Stop there. Claude reviews, merges.
