# Agent Protocol — Worker Startup & Completion

**Every worker agent in a worktree (`~/github/zclassic23-N`) MUST follow
this protocol exactly.** It is the contract between the orchestrator and
the workers. Deviating breaks coordination and risks data loss.

---

## Startup ritual (run on EVERY fresh session)

```
1. Identify yourself
   $ pwd                                  # e.g., /home/rhett/github/zclassic23-2
   → worktree ID = "wt2"  (extract suffix after "zclassic23-")
   → if no suffix → you are the orchestrator; read the orchestrator section
                    of REFACTOR_STATUS.md, NOT a worker assignment

2. Sync from origin
   $ git fetch origin
   $ git checkout main
   $ git pull --ff-only origin main

3. Load the architecture
   $ cat docs/FRAMEWORK.md                # canonical architecture
   $ cat docs/REFACTOR_STATUS.md          # current phase + your row in "In flight"

4. Load your assignment
   $ ls docs/work/wt<N>-*.md              # your assignment(s)
   $ cat docs/work/wt<N>-<slug>.md        # full spec — branch name, scope, tasks

5. Verify assignment is unclaimed / in-progress for you
   - Check the "Status" section at the bottom of your assignment doc
   - If status is "DONE" → nothing to do; report to user
   - If status is "BLOCKED" or "FAILED" → re-read the issue, decide whether
     to retry or escalate
   - If status is "READY" or "IN PROGRESS (wt<N>)" → proceed

6. Branch off main
   $ git checkout -b <branch-name-from-assignment>

7. Mark in-progress
   - Edit the assignment doc's Status section to "IN PROGRESS (wt<N>)"
   - Commit + push (small commit, marks ownership)
```

After this, **execute the assignment's Tasks section in order**.

---

## Per-task discipline

Each Task in an assignment has:

- A **scope** — exact files to create/edit
- An **acceptance test** — concrete check that proves done

For each task:

1. Implement.
2. Run the acceptance test (`make test_parallel`, build, lint, etc.).
3. **If green:** `git add` the specific files, `git commit -m "<task description>"`.
4. **If red:** debug. Do NOT commit a broken state. If stuck > 30 min, append a `BLOCKED` note to the assignment.
5. After every 2-3 tasks: `git pull --rebase origin main` to stay current; `git push origin <branch>` to back up work.

---

## Commit discipline

- **Small commits.** Each task is its own commit. No 5-task megacommits.
- **Subject line < 70 chars.** Imperative mood: "add CONDITION macro", not "added".
- **Body** explains the *why*. The *what* is in the diff.
- **Co-author trailer:** every commit ends with
  ```
  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  ```
- **No `--no-verify`.** Pre-commit hooks exist for a reason.
- **No `--amend`** after push. Always create new commits.

---

## Push discipline (the part we MUST NOT FORGET)

Push at three points:

1. **At in-progress mark** (step 7 of startup): so the orchestrator can see you're working.
2. **Every 2-3 task commits**: backup.
3. **At completion**: final push of the branch.

```bash
git push origin <branch-name>
```

If push fails due to non-fast-forward:
```bash
git pull --rebase origin <branch-name>
# resolve any conflicts
git push origin <branch-name>
```

**Never** `git push --force` to main. **Never** `git push` directly to main.

---

## Completion ritual

When all tasks pass and acceptance criteria are met:

1. **Run the full test suite** one more time:
   ```bash
   make test_parallel    # fork-based runner, ~1 min
   make lint             # all 17+ gates
   ```
   All green = ready. Any red = back to debugging.

2. **Update the assignment doc** — append a Completion section:

   ```markdown
   ## Completion (wt<N>, YYYY-MM-DD)

   ### Summary
   <1-3 sentence summary of what shipped>

   ### Commits
   - <sha> <subject>
   - <sha> <subject>
   - ...

   ### Files added/modified
   - app/conditions/block_failed_mask_at_tip.c   (NEW, 48 LOC)
   - lib/framework/condition.h                    (NEW, 102 LOC)
   - ...

   ### Acceptance verification
   - [x] Test 1: <description> — PASS
   - [x] Test 2: <description> — PASS
   - [x] Live check: <description> — PASS

   ### Surprises / follow-ups
   <anything orchestrator should know — found bug elsewhere, scope change, etc.>

   ### Status
   DONE — branch wt<N>/<slug> pushed to origin, ready for orchestrator merge.
   ```

3. **Push the completion update**:
   ```bash
   git add docs/work/wt<N>-<slug>.md
   git commit -m "wt<N>: complete <slug>"
   git push origin <branch-name>
   ```

4. **Report to the user**:
   > Completed assignment `wt<N>/<slug>`. Branch pushed to origin. Ready for orchestrator merge.

5. **Do NOT merge to main yourself.** Orchestrator handles that.

---

## Memory discipline

After completing the assignment:

- **Save a project memory** if the work shipped a non-obvious primitive or surprising finding. Use `feedback_*` for guidance, `project_*` for things that shipped.
- **Do NOT save memory for routine completions** — the assignment doc + git log are the record.

---

## What to do if you're confused

In this priority order:

1. Re-read `docs/FRAMEWORK.md` for the architectural shape.
2. Re-read your assignment doc — it has a Tasks section in order.
3. Re-read `docs/REFACTOR_STATUS.md` to see where you fit in the bigger picture.
4. Check the existing codebase for examples (e.g., for Job shape, look at `app/services/src/header_admit_stage.c` — the canonical stage adopter).
5. If still stuck > 30 min: append `BLOCKED: <reason>` to your assignment, push, report to user. Do NOT guess and ship questionable code.

---

## Forbidden moves

- ❌ Editing `docs/REFACTOR_STATUS.md` directly (workers don't touch it; orchestrator only).
- ❌ Touching files outside your assignment's scope.
- ❌ `git push --force` on any branch.
- ❌ Merging your own branch to main.
- ❌ Deleting another worker's branch.
- ❌ `--no-verify`, `--amend` after push, skipping tests "just this once".
- ❌ Editing CLAUDE.md without orchestrator sign-off (it's auto-loaded into every session).
- ❌ Writing code that doesn't match one of the 8 framework shapes.

---

## End-of-session

Whether you finished or not:

1. `git status` — verify clean (or known WIP).
2. `git push origin <branch>` — back up.
3. If finished: update assignment Status to `DONE`.
4. If not finished: update assignment Status to `IN PROGRESS (wt<N>) — paused at task <X>`.
5. Report briefly to user.
