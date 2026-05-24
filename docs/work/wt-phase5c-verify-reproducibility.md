# Worker Assignment — Phase 5c: `make verify-reproducibility`

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 5 (Crypto agility + reproducible releases)
**Depends on:** Phase 5b-1 (flake.nix skeleton) ✅ — required to build
the binary in two locations under identical inputs.
**Status: QUEUED** until 5b-1 merges.
**Plan reference:** [`docs/architecture/phase5-crypto-agility-and-releases.md`](../architecture/phase5-crypto-agility-and-releases.md) § 5c

**Owns:**
- EDIT `Makefile` — add `verify-reproducibility` target
- NEW `tools/verify/diff_binaries.sh` (or `.py`) — produces a structured diff report
- EDIT `.github/workflows/ci.yml` (if it exists) — add a nightly reproducibility job
- NEW `docs/RELEASE_REPRODUCIBILITY.md` — operator-facing instructions
- EDIT `Makefile.help` (if present) — describe new target

**MUST NOT touch:**
- The actual build (no source changes — this verifies, doesn't fix)
- `flake.nix` / `nix/zclassic23.nix` (Phase 5b-1 owns those)
- Wave S, Phase 3, Phase 4, Phase 6 code paths
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 5b-1 wraps the build in Nix so inputs (compiler version, vendor
versions, build flags) are pinned. But that's only USEFUL if we
actually verify the same inputs produce the same outputs — otherwise
the wrapper is just ceremony.

`make verify-reproducibility` does the obvious thing: build the binary
twice (or compare with a friend), diff the resulting bytes, and report
any divergence. If the divergence is non-zero, something nondeterministic
crept into the build: a timestamp, an `__FILE__` path leak, an unstable
sort, a randomized hash bucket. Each of these is fixable, but only if
we notice. This PR makes "did the binary change for non-source reasons"
a one-command question.

**Why this matters for users:** a reproducible binary means a user can
download a release, run `make verify-reproducibility` from source,
and confirm "yes, this binary is the one produced by this commit by
this compiler" — no third-party trust required, no signing key
involved. Combined with 5d (cosign signing for convenience), it gives
both the trust-minimized path AND the convenient default.

---

## Behavior

```bash
$ make verify-reproducibility
==> Building zclassic23 in build1/ via Nix flake...
==> Building zclassic23 in build2/ via Nix flake...
==> Diffing binaries:
    build1/zclassic23: 27,134,592 bytes (sha256: a1b2c3...)
    build2/zclassic23: 27,134,592 bytes (sha256: a1b2c3...)
==> REPRODUCIBLE ✓ — both builds produced identical bytes.
```

When NOT reproducible:

```bash
==> Diffing binaries:
    build1/zclassic23: 27,134,592 bytes (sha256: a1b2c3...)
    build2/zclassic23: 27,134,608 bytes (sha256: d4e5f6...)
==> NOT REPRODUCIBLE ✗ — divergence detected.
==> Generating diff report → reports/reproducibility-2026-05-24.txt
==> First divergence at offset 0x4f3a08 (within .rodata)
==> Likely source: __FILE__ path, timestamp, or build-id (see report)
```

A second build under Nix should give bit-identical output if the
flake fully pins inputs. Failure modes typically include:
1. `__DATE__` / `__TIME__` macros (always replace with a fixed string)
2. `__FILE__` containing build-tree paths (use `-fdebug-prefix-map`)
3. Linker's `.note.gnu.build-id` (deterministic by default in
   modern ld; can be reproduced explicitly with `--build-id=sha1` over
   normalized inputs)
4. Random hash bucket ordering in symbol tables (rare; passes
   `-Wl,--sort-section=name`)

---

## Tasks (in order)

### Task 1: `make verify-reproducibility` skeleton

EDIT `Makefile`. Add the target:

```makefile
.PHONY: verify-reproducibility
verify-reproducibility:
	@command -v nix >/dev/null || { \
	  echo "verify-reproducibility requires Nix (see flake.nix in 5b-1)."; \
	  echo "Install: https://nixos.org/download"; \
	  exit 2; }
	@mkdir -p build1 build2 reports
	@echo "==> Building zclassic23 in build1/ via Nix flake..."
	@rm -rf build1/zclassic23
	@nix build .#zclassic23 -o build1/result 2>&1 | tee reports/build1.log
	@cp -L build1/result/bin/zclassic23 build1/zclassic23
	@echo "==> Building zclassic23 in build2/ via Nix flake..."
	@rm -rf build2/zclassic23
	@nix build .#zclassic23 -o build2/result 2>&1 | tee reports/build2.log
	@cp -L build2/result/bin/zclassic23 build2/zclassic23
	@./tools/verify/diff_binaries.sh build1/zclassic23 build2/zclassic23
```

**Acceptance:** target exists, errors clearly if Nix is missing,
attempts both builds (success or failure depends on 5b-1 quality).

### Task 2: `diff_binaries.sh` (or `.py`)

NEW `tools/verify/diff_binaries.sh`. Behavior:

```bash
#!/usr/bin/env bash
# Usage: diff_binaries.sh <file1> <file2>
# Outputs structured report to stdout + reports/reproducibility-<date>.txt.
# Exit 0 if identical, 1 if divergent, 2 if I/O error.

set -eu
F1="$1"; F2="$2"
B1=$(stat -c %s "$F1")
B2=$(stat -c %s "$F2")
H1=$(sha256sum "$F1" | cut -d' ' -f1)
H2=$(sha256sum "$F2" | cut -d' ' -f1)

cat <<EOF
==> Diffing binaries:
    $F1: $B1 bytes (sha256: ${H1:0:16}...)
    $F2: $B2 bytes (sha256: ${H2:0:16}...)
EOF

if [ "$H1" = "$H2" ]; then
    echo "==> REPRODUCIBLE ✓ — both builds produced identical bytes."
    exit 0
fi

echo "==> NOT REPRODUCIBLE ✗ — divergence detected."
REPORT="reports/reproducibility-$(date +%Y-%m-%d).txt"
mkdir -p "$(dirname "$REPORT")"

# First differing offset via cmp -l (octal output).
FIRST=$(cmp -l "$F1" "$F2" 2>/dev/null | head -1 | awk '{print $1}')
if [ -n "$FIRST" ]; then
    OFF=$((FIRST - 1))
    echo "==> First divergence at offset 0x$(printf '%x' "$OFF")"
fi

# Try to identify the section via readelf.
if command -v readelf >/dev/null; then
    SECTION=$(readelf -S "$F1" 2>/dev/null | \
        awk -v off="$OFF" '
            /\[.*\]/ {
                gsub(/[\[\]]/, "", $1);
                if ($1 ~ /^[0-9]+$/) {
                    sect=$2; addr=strtonum("0x" $4); size=strtonum("0x" $6);
                    if (off >= addr && off < addr + size) print sect;
                }
            }')
    [ -n "$SECTION" ] && echo "==> Within ELF section: $SECTION"
fi

# Full diff to report file.
{
    echo "Binary 1: $F1 ($B1 bytes, sha256 $H1)"
    echo "Binary 2: $F2 ($B2 bytes, sha256 $H2)"
    echo "First differing offset: 0x$(printf '%x' "$OFF" 2>/dev/null || echo unknown)"
    echo
    echo "=== cmp -l (first 100 differing bytes) ==="
    cmp -l "$F1" "$F2" 2>/dev/null | head -100
    echo
    echo "=== diffoscope (if installed) ==="
    if command -v diffoscope >/dev/null; then
        diffoscope "$F1" "$F2" 2>&1 | head -200
    else
        echo "(diffoscope not installed — install for deeper analysis)"
    fi
} > "$REPORT"
echo "==> Full report: $REPORT"
exit 1
```

Make executable. **Acceptance:** invoking on two identical files
prints "REPRODUCIBLE ✓" + exits 0. Invoking on two slightly
different files prints "NOT REPRODUCIBLE ✗" + writes a report.

### Task 3: `docs/RELEASE_REPRODUCIBILITY.md`

Write operator-facing doc covering:
1. What it means (deterministic build = no trust required)
2. How to verify a release: download tarball, run
   `make verify-reproducibility`, compare hash to release notes
3. Known divergence sources + fixes (link to upstream debugging)
4. How to report a non-reproducible build (file an issue with the
   `reports/reproducibility-*.txt`)

Keep it under 100 lines — this is a reference card, not a treatise.

**Acceptance:** doc exists, is reasonable, doesn't duplicate
content in `flake.nix` comments.

### Task 4: Optional — CI nightly job

If `.github/workflows/` exists, add a `reproducibility.yml` workflow
that runs nightly on a Nix-enabled runner:

```yaml
name: reproducibility
on:
  schedule: [{cron: '0 3 * * *'}]  # 3 AM UTC daily
jobs:
  verify:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: DeterminateSystems/nix-installer-action@main
      - run: make verify-reproducibility
      - if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: reproducibility-report
          path: reports/
```

If `.github/workflows/` does NOT exist, skip this task (don't create
the directory — the user may have intentionally not used GHA).

**Acceptance:** workflow file present (if applicable), nightly run
green on a stable revision.

### Task 5: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
# Don't run verify-reproducibility here — it requires Nix + ~10 min.
# The next nightly CI run will exercise it. Manual smoke test optional.
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## What this does NOT do

- Does NOT FIX a non-reproducible build. If divergence is detected,
  this PR's report points to the offending section/file; a follow-up
  PR adjusts compiler/linker flags to eliminate the source.
- Does NOT sign releases. Phase 5d covers cosign-based signing for
  the user-convenient path.
- Does NOT cover cross-platform reproducibility (Linux build matches
  Linux build — macOS / Windows would need their own flake outputs).
- Does NOT build releases automatically — `make release-tarball`
  remains a separate target.

---

## Risk + rollback

This PR is a pure verifier. Worst case: the script has a bug and
prints incorrect divergence info. Rollback is `git revert`, no
operational risk.

The dependency on Nix means this target won't run on machines
without Nix installed. Mitigation: the makefile guards with a clear
error message + install link. CI nightly job runs on a known-good
Nix environment.

---

## Commit cadence

One commit per task. Push after task 3.

---

## Status

**QUEUED** — gated on Phase 5b-1 (flake.nix skeleton) merging.
Once `nix build .#zclassic23` produces a working binary, this PR is
READY.

<!-- Worker: append a Completion section below when done. -->
