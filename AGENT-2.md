# AGENT-2 — Wallet & Storage Hardening

**Derived from:** the 2026-04-17 full code review (see `AGENT.md` for the full
checklist and coordinating plan).

**Working directory:** `~/zclassic23-2` (separate git clone; pulls/pushes to `origin/main`).

**Coordinator:** Rhett (~/zclassic23).

**Agent-3 lane (do not touch):** `lib/crypto/`, `lib/sapling/`, `lib/keys/`.

---

## Preflight — run verbatim, do not ask questions

Execute the block below top to bottom. Each command is defensive and idempotent.
Do NOT pause to ask about branch state, stash, or divergence — the block handles
all of it. Only stop if `make` or `./test_zcl` fails (broken baseline — tell Rhett).

```bash
cd ~/zclassic23-2

# 1. Preserve any local-only work to its current branch so nothing is stranded
git add -A
git diff --staged --quiet || git commit -m "a2: wip checkpoint before AGENT-2.md workstream"

# 2. If on a feature branch, push it as backup, then move to main
CURRENT=$(git branch --show-current)
if [ "$CURRENT" != "main" ]; then
    git push origin "$CURRENT" 2>/dev/null || true   # backup; ok if no perms
    git checkout main
fi

# 3. Sync main
git pull origin main --rebase=false

# 4. Confirm clean baseline
git status
git branch --show-current            # must print "main"

# 5. Read the rules and the checklist
cat CLAUDE.md
cat DEFENSIVE_CODING.md
cat AGENT.md                          # your rows: P1.1, P1.2, P1.5, P6.1–P6.6

# 6. Confirm green baseline — STOP and report if either fails
make -j"$(nproc)"
./test_zcl
```

Once that all passes, start Step 1 below. Commit/push after each logical fix.

---

## Your mission

Fix the wallet / storage / coins subsystem so that:
1. Every failed write is observable (no more `LOG_FAIL` followed by `return true`).
2. Every write goes through the project's activerecord path — no raw `sqlite3_step`.
3. Flushes either commit all work atomically or roll back cleanly — never
   commit partial state.
4. Schema migrations cannot silently advance the version pointer past a failed step.

These findings re-introduce the exact bug class that lost 0.4 ZCL on 2026-04-10
and 1.3M UTXOs on 2026-04-12. That is why they are P1.

---

## Files you own

You may edit anything under:
- `lib/wallet/`
- `lib/storage/`
- `lib/coins/`
- `lib/test/` — but only to add tests that cover your changes

You may read anything else, but do not edit outside these trees.

---

## Workstream (do in this order)

### Step 1 — P1.1 + P1.2: Wallet wrapper silent-error pattern
File: `lib/wallet/src/wallet_sqlite.c`

**Bug:** Every `bool wallet_sqlite_*_r` wrapper logs a failure via `LOG_FAIL(...)`
then unconditionally falls through to `return true;`. The flusher at
`:1054-1072` then commits partial state because it ignores the rc.

Sites to fix: `:259, 439, 571, 600, 661, 703, 759, 835, 938, 984, 1103` and the
flusher that calls them.

**How to fix (pattern):**
- Change every wrapper so the `LOG_FAIL` path returns `false`.
- In the flusher, capture the rc of every writer, and if any returned false,
  `ROLLBACK TRANSACTION` (or the activerecord equivalent) and propagate the
  failure to the caller.
- Verify the `keystore_count != row_count` detector still fires on the
  post-rollback state.

**Acceptance:** add a test in `lib/test/` that (a) forces a write to fail
mid-flush (e.g. inject SQLITE_IOERR via a hook or use a synthetic BUSY) and
asserts the whole transaction rolls back AND the function returns false.

### Step 2 — P1.5: Raw sqlite3_step in UTXO batch writer
File: `lib/storage/src/coins_view_sqlite.c:461, 474, 509, 557`

**Bug:** The UTXO batch writer — the very file DEFENSIVE_CODING.md §1 names as
the motivating case for the rule — calls `sqlite3_step()` directly with no
`ZCL_AR_RAW_SQL` opt-out.

**How to fix:**
- Replace the raw `sqlite3_step` calls with the project's activerecord helper
  (search for `AR_STEP_` / `AR_BEGIN_SAVE` in `util/` or `lib/storage/` to
  find the canonical wrappers).
- If an opt-out is genuinely needed (this file is infrastructure), document
  it with `#define ZCL_AR_RAW_SQL` and a one-line comment explaining why.

**Acceptance:** `make lint` must pass with Rhett's P0.1 patch in place (flip
`check-raw-sqlite` from warn to fail). Coordinate with Rhett before pushing
this step — Rhett may land P0.1 first so you can see lint fail then pass.

### Step 3 — P6.1: Sapling child-index race
File: `lib/wallet/src/wallet_sqlite.c:822-830`

**Bug:** `wallet_sqlite_write_sapling_key` runs an inline
`UPDATE wallet_seed SET next_child=...` via prepare/step/finalize on every key
write, ignoring rc. A single `SQLITE_BUSY` desyncs `next_child`, causing later
child-index reuse and address collisions.

**Fix:** check rc on every statement op; if the UPDATE fails, propagate failure
and rollback. Add a regression test that simulates SQLITE_BUSY and asserts no
`next_child` regression after failure.

### Step 4 — P6.2: Flusher resets all shared-conn statements
File: `lib/storage/src/coins_view_sqlite.c:419-426, 587-590`

**Bug:** `sqlite3_next_stmt()` walks ALL statements on the shared connection
and resets them, including statements owned by readers mid-iteration
(`:289, 320` are readers holding `stmt_get` in `SQLITE_ROW`). Flushing
rewinds the reader iterator silently.

**Fix:** track your own statements in a local list and reset only those, OR
add a reader lock that excludes flush while a reader iterator is open.

### Step 5 — P6.3: read_keys silently skips malformed rows
File: `lib/wallet/src/wallet_sqlite.c:533-553`

**Fix:** replace the silent `continue;` with a `LOG_ERR` that includes the
rowid and column that failed decode; increment a corrupt-row counter surfaced
via `getwalletinfo.persistence`. Do not drop keys without an operator-visible
event.

### Step 6 — P6.4: Migration framework unchecked writes
File: `lib/storage/src/schema_migration.c:134, 169, 230`

**Fix:** check rc on every `node_db_exec` in migration bookkeeping; if the
version INSERT fails, roll back the whole migration.

### Step 7 — P6.5: Hot-path prepare/finalize churn
File: `lib/wallet/src/wallet_sqlite.c:642-705`

**Fix:** move the prepare into `wallet_sqlite_init` and stash the compiled
statements on `ws->`; reset+bind on each call; finalize on shutdown.

### Step 8 — P6.6: coins_alloc OOM silent
File: `lib/coins/src/coins.c:54-55, 106-110`

**Fix:** return NULL on alloc failure (not a struct with `num_vout=0`), and
make every caller check. Log the OOM via `LOG_FAIL`.

---

## Commit protocol

- One logical fix per commit. Good: "wallet: propagate flush failure through
  wrapper return values". Bad: "wallet + storage + migrations".
- Every commit: `make test` must pass. Every push: `make ci` must pass.
- Commit message format:
  ```
  wallet: <one-line summary>

  <why — cite file:line from AGENT.md>

  Fixes P1.1, P1.2 (AGENT.md checklist).
  ```
- After each push, update the corresponding row in `AGENT.md`:
  `open` → `in-progress` → `done (SHA abc1234)`.
- Push frequently (per step or per commit) so Rhett can review incrementally.
- Never `--amend` a pushed commit. Never `--force-push`.

---

## Coordination with Agent-3 and Rhett

- Agent-3 is in the crypto/sapling lane. You should not see their files in your
  diffs, and they should not see yours.
- If you need something Rhett owns (e.g. P0.1 lint flip), note it in your
  commit message and wait for Rhett's change to land — don't front-run it.
- Surprising discoveries (bugs outside your scope, unclear tradeoffs) → add a
  paragraph at the end of this file under `## Notes from Agent-2` instead of
  going silent or expanding scope.

## Done criteria

- All P1.1, P1.2, P1.5, and P6.1–P6.6 rows in `AGENT.md` show `done <SHA>`.
- `make ci` green on the final pushed commit.
- At least one new test in `lib/test/` per bug class demonstrating the
  previous failure mode is now caught.
- No new files created outside `lib/wallet/`, `lib/storage/`, `lib/coins/`,
  `lib/test/`.
