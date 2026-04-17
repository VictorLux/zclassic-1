# AGENT-2 — Wallet & Storage Hardening

**Derived from:** the 2026-04-17 full code review (see `AGENT.md` for the full
checklist and coordinating plan).

**Working directory:** `~/zclassic23-2` (separate git clone; pulls/pushes to `origin/main`).

**Coordinator:** Rhett (~/zclassic23).

**Agent-3 lane (do not touch):** `lib/crypto/`, `lib/sapling/`, `lib/keys/`.

---

## Status — 2026-04-17

**Original brief complete.** All rows merged to main:

- ✅ P1.1 Wallet wrapper silent-error fixed (8608820e7)
- ✅ P1.2 Flush commits-partial-state fixed (8608820e7)
- ✅ P1.5 Raw `sqlite3_step` in UTXO batch writer migrated (152603fdc)
- ✅ P6.1 `write_sapling_key` UPDATE failure propagates (8608820e7)
- ✅ P6.2 Flusher reset scoped to own stmts (152603fdc)
- ✅ P6.3 `read_keys` LOG_ERR on malformed rows (8608820e7)
- ✅ P6.4 Migration bookkeeping rc-checked (767d9d3e7)
- ✅ P6.5 Hot-path prepare hoisted to init (8608820e7)
- ✅ P6.6 `coins_alloc` OOM logged (dc60b7e7b)

Good work. Now on to the next pile.

---

## NEXT UP — P3.4 + P3.5 + P3.6 store/rpc_client hardening

P3.3 landed cleanly (2a59ac938 + siblings, ~115 sites migrated across 17
files; 5 state-kv / rollback opt-outs retained with descriptive
annotations — those are correct and stay). Now close the three adjacent
rows in the P3 group that sit in your expanded scope.

### Step A — P3.4: store_controller address checksum validation
File: `app/controllers/src/store_controller.c:663-685`

**Bug:** The checkout handler accepts addresses as strings and does not
verify the Base58Check or Bech32 checksum before writing them to the order
record. A customer who typos a character gets their ZCL burned on a
syntactically-valid-but-checksum-invalid address.

**Fix:** Route every incoming address through the project's existing
decode helpers (`key_io.h`'s `decode_destination`, or whatever the
existing wallet code uses for t-addr and z-addr validation). On invalid
checksum: return a 400 with a specific error message and don't write the
order. Add a test that posts a typo'd address and asserts rejection.

### Step B — P3.5: rpc_client.c realloc overwrite w/ no NULL check
File: `tools/mcp/rpc_client.c:126`

**Bug:** `buf = realloc(buf, new_size)` — classic memory-leak-on-failure
pattern. If realloc returns NULL, the old pointer is overwritten and lost.
Under memory pressure the MCP client leaks the accumulating response
buffer on every failed call.

**Fix:** Use a `void *tmp = realloc(buf, new_size); if (!tmp) { LOG_FAIL;
free(buf); return -1; } buf = tmp;` pattern. Search the file (and
`tools/mcp/`) for any other `X = realloc(X, ...)` sites and fix them all.

### Step C — P3.6: parse_form_field URL-decode + CSRF token
File: `app/controllers/src/store_controller.c:803-823`

**Bug:** `parse_form_field` treats raw URL-encoded bytes as-is. A customer
whose order note contains `%20` sees literal `%20` stored. Worse, there's
no CSRF token on the order form, so any logged-in session can be
cross-site-tricked into placing orders.

**Fix (URL-decode):** Port in the project's existing URL-decode helper
(grep for `url_decode` / `percent_decode` in `lib/net/src/` or
`app/controllers/`). Apply to every field before use.

**Fix (CSRF):** Generate a per-session random token on the checkout-page
GET, include it as a hidden form field, verify on POST. Pattern: look for
how the wallet-send form handles it (Rhett's done this before in
`wallet_view_send.c`).

**Acceptance for Step C:** add a test that (a) posts a form without a
token and asserts rejection, (b) posts with an invalid token and asserts
rejection, (c) posts with a valid token and asserts success.

### Commit rules

Same as always — one logical fix per commit, `make test && make lint`
green before each, push frequently, no amends on pushed commits.

---

## THEN — P5 operator hygiene queue (do not wait; start as soon as P3 closes)

Rhett is pre-authorizing these so you don't stall waiting on a check-in.
Knock them out in order, smallest first.

### Step D — P5.1: `export_snapshot` ELF is tracked in git despite `.gitignore`

The 1.1 MB `export_snapshot` binary is checked into the repo root (see
`git ls-files | grep export_snapshot`). `.gitignore` lists it but git
already has it cached. Fix:

```bash
git rm --cached export_snapshot
echo "# (export_snapshot already in .gitignore; removing cached copy)" \
  >> /dev/null
```

Then verify `.gitignore` actually covers it (it should). One commit:
`build: remove tracked export_snapshot ELF from repo`.

### Step E — P5.7: repo-root clutter

`git status` shows 40+ `.md` files, `node.db`, various untracked
artifacts at repo root. Audit what's tracked vs. untracked. For
tracked `.md` files that are stale / superseded by the new AGENT* /
CLAUDE.md / DEFENSIVE_CODING.md system: move them to `docs/archive/`
or delete after confirming they're not referenced. For `node.db` and
similar runtime artifacts: add to `.gitignore`. Commit per logical
group — "docs: archive superseded hardening notes", "build: ignore
runtime node.db artifacts", etc.

### Step F — P5.3: hardcoded `/home/rhett` paths

Files: `tools/export_snapshot.c:15` and `tools/zcl-nodectl.c:628-637`
(and anywhere else grep finds them — `grep -rn "/home/rhett"
tools/ app/ lib/ config/`).

Replace literal `/home/rhett` with:
- `getenv("HOME")` for runtime paths
- the project's datadir resolution (`zcl_datadir()` / whatever exists
  in `config/src/`) for data paths
- a compile-time `CMAKE_INSTALL_PREFIX`-style default otherwise

Don't introduce new config; use what's already there. Add a test that
exercises a non-`/home/rhett` $HOME to prevent regression.

### Step G — P5.4: purge 10 shell scripts in `tools/` duplicating MCP

List them first: `ls tools/*.sh`. For each:
1. Identify the MCP tool that replaces it (most are named similarly;
   `tools/zcl-balance.sh` → `zcl_balance`, etc.).
2. If the MCP tool exists and covers the shell script's behavior:
   remove the shell script in one commit per script, commit message
   `tools: purge zcl-FOO.sh (superseded by MCP tool zcl_FOO)`.
3. If functionality is missing from MCP: open a TODO comment, flag
   Rhett, don't remove.

This is the project rule from `feedback_no_external_tools.md` — no
standalone shell scripts, everything in the binary.

### Stopping point

After all of P3.4/5/6 and P5.1/5.3/5.4/5.7 are on main, ping Rhett.
Next pile will likely be either (a) helping Agent-3 with any leftover
audit work, or (b) joining Rhett on the medium P3/P4 items. Don't
start P5.2 / P5.5 / P5.6 on your own — those need Rhett's coordination
(service files, vendor submodules, CVE cherry-picks).

---

## Previous NEXT UP (now done) — P3.3 raw `sqlite3_step` migration in app/

**Scope:** ~80 raw `sqlite3_step()` call sites across `app/controllers/` and
`app/services/`. These are the exact pattern you just migrated in
`coins_view_sqlite.c` (P1.5) — hoist the prepare, route stepping through the
project's activerecord helpers (`AR_STEP_ROW_READONLY` for SELECTs,
`AR_BEGIN_SAVE` for writes). The infrastructure you need already exists:
Agent-3's `a5511028d` landed `lib/util/include/util/ar_step_readonly.h` and
flipped the lint gate (`check-raw-sqlite` is now fail-exit-1 under
`-DZCL_AR_ENFORCE`).

Many call sites in the merge are already wearing `// raw-sql-ok: a3`
annotations Agent-3 added to make the lint pass. **Those annotations are
a TODO list** — every one of them needs a proper migration. Your job is to
convert them, one file at a time, removing the annotation as you go.

**File ownership expanded for this task:** You may now edit:
- `app/controllers/` (all files)
- `app/services/` (all files)
- `tools/mcp/controllers/` — only the raw-step sites (don't touch JSON
  handling; Rhett owns the injection fixes at P3.1/P3.2)
- previous scope: `lib/wallet/`, `lib/storage/`, `lib/coins/`, `lib/test/`

Still off-limits: `lib/crypto/`, `lib/sapling/`, `lib/keys/` (Agent-3) and
`lib/rpc/`, `lib/validation/`, `lib/consensus/`, `lib/net/`, `lib/script/`
(Rhett).

**How to find the work:**

```bash
# Every site that still needs migration:
grep -rn "raw-sql-ok" app/controllers/ app/services/ tools/mcp/ | wc -l

# Or target a single file:
grep -n "raw-sql-ok\|sqlite3_step" app/controllers/src/explorer_controller.c
```

**Commit rules (same as before):**

- One file per commit. "explorer_controller: migrate 7 raw steps to AR helpers".
- Never bulk-rename. Never disable the lint.
- After each commit: `make lint && make test` must pass.
- Push frequently. The ~80-site pile is too big for one PR-class commit.

**Suggested order (smallest files first to build momentum):**

1. `app/controllers/src/file_controller.c`
2. `app/controllers/src/hodl_controller.c`
3. `app/controllers/src/snapshot_controller.c`
4. `app/controllers/src/api_controller.c`
5. `app/controllers/src/explorer_*.c` (3 files)
6. `app/controllers/src/store_controller.c` (bigger — leave `parse_form_field`
   and checksum gap for Rhett; you only migrate step sites)
7. `app/controllers/src/wallet_*.c` (wallet_controller, wallet_rescan_controller,
   wallet_shielded_controller, wallet_view_*.c — these touch your home turf)
8. `app/services/src/*.c` — everything under services, smallest first

Once the lint-grep returns zero, update AGENT.md row P3.3 to
`done <last SHA>` and ping Rhett.

---

**Everything below this line is your original brief — unchanged for
reference. The preflight block still works verbatim if you ever need to
re-bootstrap a stale clone.**

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

## Notes from Agent-2

**2026-04-17 — P1.1/P1.2/P1.5/P6.1–P6.6 landed via four commits**
(dc60b7e7b, 767d9d3e7, 152603fdc, 8608820e7; AGENT.md status update
af247faf0). All pushed to origin/main.
`ZCL_TEST_ONLY=persistence ./test_zcl` passes with 0 failures, covering
the wallet-sqlite open/write/flush round-trips, the canary, the new
`test_wallet_flush_rollback` regression suite, and the lint-gate
self-test. `make lint` is green. The flush-rollback test injects a
SQLite trigger that aborts `wallet_transactions` INSERTs and asserts
the whole transaction rolls back — that's the exact silent-partial-
state path that lost 0.4 ZCL on 2026-04-12.

**Out-of-scope observation — `test_block_pruning` hangs on current main.**
After "prune: fixture init (basic)... OK" the process sits in
`futex_wait_queue` / `hrtimer_nanosleep` at 6–17% CPU indefinitely;
it reproduces against a clean merge of origin/main with and without
Agent-2 changes, so it isn't a regression from this workstream.
Flagging for Rhett because `app/services/src/block_pruning_service.c`
is outside Agent-2's lane. Worth checking whether Agent-3's new
`test_make_lint_gates` (which shells out to `make` and may linger in
`system()`) interacts with an earlier test's background thread —
haven't bisected.
