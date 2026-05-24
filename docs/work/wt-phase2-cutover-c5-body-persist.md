# Worker Assignment — Phase 2 CUTOVER C-5: body_persist authoritative + DELETE body_fetch

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 2 (Wave S cutover)
**Depends on:** C-3 (validate_headers) AUTHORITATIVE shipped + 24h soak.
**Status: QUEUED** until C-3 lands.
**Plan reference:** [`docs/architecture/wave-s-cutover.md`](../architecture/wave-s-cutover.md) § C-5

**Owns:**
- EDIT `app/services/include/services/body_persist_stage.h` — add mode flag
- EDIT `app/services/src/body_persist_stage.c` — gate authoritative branch
- EDIT `lib/storage/src/blocks_mmap_reader.c` OR equivalent — add divergence
  guard in the legacy path
- EDIT `config/src/cli_args.c` — `-body-persist-mode=shadow|authoritative`
- EDIT `config/src/boot_services.c` — default mode AUTHORITATIVE after this PR
- **DELETE** `app/services/src/body_fetch_stage.c` — fully redundant once
  body_persist owns the persist path

**MUST NOT touch:**
- `lib/storage/src/event_log.c` (Phase 4a primitive)
- `lib/storage/src/utxo_projection.{c,h}` (Phase 4b)
- Wave S S-1/S-2/S-3 stages (header phase, already cut over via C-2/C-3)
- Wave S S-6/S-7/S-8/S-9 stages (downstream of body_persist; not this PR)
- Phase 3 / 4 / 5 / 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

`body_persist` is the Wave S stage that writes block bodies to disk.
`body_fetch` is the legacy path that does the same — they currently
both write (body_persist as shadow, body_fetch as authoritative). This
PR flips authority to `body_persist` AND DELETES `body_fetch` outright
because there are no other consumers.

After this PR:
- `body_persist_log[H]` is the canonical "block H's body is on disk"
  signal (per the Wave S cursor model).
- `app/services/src/body_fetch_stage.c` (~600 LOC) is **GONE**.
- The legacy fetch path is one fewer thing the supervisor watches.

This is the FIRST Wave S cutover that gets to delete its predecessor
outright (C-2 and C-3 had to keep the legacy code as divergence
guards for soak). body_fetch has no other consumers beyond the
chain_advance_coordinator (which is itself being dissolved in C-9).

---

## The 4-commit pattern

Follow the same pattern that worked for C-2 + C-3:

### Commit 1: Add `body_persist_mode` flag

```c
/* in app/services/include/services/body_persist_stage.h */
typedef enum {
    BODY_PERSIST_MODE_SHADOW = 0,    /* legacy authoritative — current default */
    BODY_PERSIST_MODE_AUTHORITATIVE  /* this PR's eventual default */
} body_persist_mode_t;

void body_persist_set_mode(body_persist_mode_t mode);
body_persist_mode_t body_persist_get_mode(void);
```

Default stays SHADOW so this commit is a pure no-op.

EDIT `config/src/cli_args.c`: parse `-body-persist-mode=shadow|authoritative`.
Default SHADOW.

EDIT `body_persist_stage.c`: wherever it currently records its log
shadow, branch on the mode:
```c
if (body_persist_get_mode() == BODY_PERSIST_MODE_AUTHORITATIVE) {
    /* This is now the canonical write path. Other callers must
     * read from body_persist_log[H] for "is body on disk". */
    body_persist_write_canonical(blk);
    return;
}
/* SHADOW: write to log only; legacy body_fetch is authoritative. */
body_persist_record_shadow(blk);
```

**Acceptance:** make clean. Existing tests PASS unchanged (SHADOW
default preserves legacy behavior).

### Commit 2: Implement the authoritative write path

The shadow path already writes the body — confirm that's bit-identical
to what body_fetch writes. If they differ:
1. Body_fetch's path is the canonical wire format (Bitcoin Core
   block serialization).
2. Body_persist must produce EXACTLY the same bytes (no re-encoding).
3. Add a comparison test (write via both, assert hash match) before
   shipping this commit.

If shadow's bytes ARE canonical: this commit is just verification
+ a guard. No new write code.

**Acceptance:** make clean. New test asserts body_persist's bytes
exactly equal body_fetch's bytes for a fixture block.

### Commit 3: Add divergence guard in body_fetch

For the brief soak period, body_fetch keeps running but ALSO checks
whether `body_persist_log[H]` already covered the body. If it did,
body_fetch logs a divergence event (it should NEVER fire after
cutover — if it does, that's a bug).

```c
/* in legacy body_fetch path, after a body write */
if (body_persist_get_mode() == BODY_PERSIST_MODE_AUTHORITATIVE &&
    body_persist_log_has(H)) {
    EMIT(EV_LEGACY_BODY_FETCH_DIVERGED,
         "body_fetch wrote H=%lld but body_persist already had it",
         (long long)H);
}
```

The condition `body_persist_log_has(H)` is the "should-not-happen"
case in authoritative mode.

**Acceptance:** make clean. Existing tests PASS. Live node in
shadow mode still doesn't fire the divergence (because body_persist
hasn't been authoritative yet).

### Commit 4: Flip default to AUTHORITATIVE + delete body_fetch

EDIT `config/src/boot_services.c`: change default to
`body_persist_set_mode(BODY_PERSIST_MODE_AUTHORITATIVE)`.

DELETE:
- `app/services/src/body_fetch_stage.c`
- `app/services/include/services/body_fetch_stage.h`
- Any call sites that were ONLY in body_fetch's flow (e.g., a
  pthread start, a registration in boot_services.c).
- Remove body_fetch from the supervisor's child registrations.

The `-body-persist-mode` flag stays (operators can flip back to
SHADOW if needed during soak — but with body_fetch deleted, SHADOW
mode means "no body persistence at all," which is broken on purpose
as the rollback signal).

Actually — the cleaner choice: **don't delete body_fetch in this PR**.
Keep it as the rollback path. Delete it in a follow-up C-5-final-delete
PR after 24h soak. Same pattern as 4b-cutover / 4c-cutover.

**Decision: Defer body_fetch deletion to a C-5-final-delete PR.**
This PR (Commit 4) just flips the default. Soak gates the deletion.

**Acceptance:** boot completes; `body_persist_get_mode() == AUTHORITATIVE`;
no `EV_LEGACY_BODY_FETCH_DIVERGED` events fire on a live node.

---

## Live verification (post-merge)

```bash
# For 24h continuously:
zcl_events --type=EV_LEGACY_BODY_FETCH_DIVERGED --since=24h
# Must return: { "count": 0 }

# Also verify body persistence is still working:
zcl_state subsystem=body_persist | jq '.last_height'
# Must equal the chain tip (or within a few blocks).
```

If divergence events fire: rollback via `-body-persist-mode=shadow`
on the node CLI, file a bug, do NOT proceed with the final-delete PR.

---

## After 24h soak: C-5-final-delete PR

Separate tiny PR:
- DELETE `app/services/src/body_fetch_stage.c` + header
- DELETE the `-body-persist-mode` flag entirely
- DELETE the SHADOW branch in body_persist_stage.c
- Update supervisor registrations to remove body_fetch's entry

Expected savings: ~600 LOC + one fewer mega-service.

---

## What this does NOT do

- Does NOT change block body wire format.
- Does NOT touch UTXO updates (those go through S-8 utxo_apply,
  separate cutover).
- Does NOT touch Phase 4e (block-body migration to event log) —
  that's a separate downstream PR.
- Does NOT change consensus rules.

---

## Risk + rollback

If body_persist has a bug that causes body writes to silently fail,
new blocks would arrive without their bodies on disk. The
chain_tip_watchdog (300s/1200s thresholds) catches this within
20 minutes.

Rollback: `-body-persist-mode=shadow` on node restart returns to
legacy. body_fetch is still there. Operator unaffected after restart.

The C-5-final-delete PR is irreversible without restoring from
backup — that's why 24h soak gates it.

---

## Commit cadence

4 commits, one per pattern step. Push after commit 3.

---

## Status

**QUEUED** — gated on C-3 (validate_headers) shipping +
24h soak.

<!-- Worker: append a Completion section below when done. -->
