# Worker Assignment — Phase 5d: Sigstore / cosign release signing

**Worktree:** wt2 OR wt3 (either)
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 5 (Crypto agility + reproducible releases)
**Depends on:** Phase 5b-1 (flake.nix skeleton) ✅ — required so that
the signed binary is the reproducible-Nix-built binary, not a casual
`make` output.
**Status: QUEUED** until 5b-1 merges. Independent of 5c.
**Plan reference:** [`docs/architecture/phase5-crypto-agility-and-releases.md`](../architecture/phase5-crypto-agility-and-releases.md) § 5d

**Owns:**
- EDIT `Makefile` — add `sign-release` + `verify-release` targets
- NEW `tools/release/sign.sh` — wraps cosign sign-blob with keyless OIDC
- NEW `tools/release/verify.sh` — wraps cosign verify-blob + checks identity
- NEW `docs/RELEASE_SIGNING.md` — operator-facing instructions
- EDIT `.github/workflows/release.yml` (if exists) — sign release artifacts on tag push

**MUST NOT touch:**
- The build itself (this is post-build signing only)
- `flake.nix` / `nix/zclassic23.nix` (Phase 5b-1 owns those)
- Source files anywhere — purely operational tooling
- `docs/REFACTOR_STATUS.md`, `docs/FRAMEWORK.md`, `CLAUDE.md`

---

## Why this matters

Phase 5c gave us **trust-minimized** verification: rebuild from source,
compare hashes, no third-party required. That's the right answer for
the paranoid path.

Phase 5d gives us **convenient** verification: download a release,
run `tools/release/verify.sh zclassic23-1.2.3.tar.gz`, get a
yes/no answer in 5 seconds. No source required, no Nix required —
just cosign + the OIDC identity of the publisher.

These are complementary, not redundant:
- A new user who just wants to run a node uses `verify-release`.
- A user with stricter requirements uses `verify-reproducibility`
  for ground-truth check, then trusts the signature for routine
  re-downloads.

Sigstore's **keyless flow** is the key innovation: no long-lived
signing key to lose / steal / rotate. Each signature is bound to an
ephemeral key issued by Fulcio against an OIDC identity (e.g., GitHub
Actions workflow token), and the signing event is logged in the
public Rekor transparency log. Compromised release? Look up the
Rekor entry; the OIDC identity tells you whether the signer was
authorized.

---

## What gets signed + what proves what

For each release artifact:
- `zclassic23-X.Y.Z.tar.gz` — source tarball (already exists)
- `zclassic23-X.Y.Z.linux-x86_64` — reproducible binary from Nix build
- `zclassic23-X.Y.Z.linux-x86_64.sig` — cosign signature blob
- `zclassic23-X.Y.Z.linux-x86_64.pem` — cosign certificate (ephemeral key + OIDC identity)
- `zclassic23-X.Y.Z.SHA256SUMS` — checksums of all artifacts
- `zclassic23-X.Y.Z.SHA256SUMS.sig` — signature over the checksums

The checksums file gets signed (one signature covers everything).
Individual `.sig` files for each binary are optional but cheap.

---

## Identity policy

The signing identity must be pinned in the verify script. The natural
choice is "the GitHub Actions workflow on the canonical repo for this
release tag":

```
--certificate-identity-regexp '^https://github\.com/RhettCreighton/zclassic-c/\.github/workflows/release\.yml@refs/tags/v[0-9]+\.[0-9]+\.[0-9]+$'
--certificate-oidc-issuer 'https://token.actions.githubusercontent.com'
```

A signature from any other identity is rejected. This is the same
threat model as "trust this GitHub release for this repo" — the
signature just makes that trust verifiable without trusting GitHub's
release page UI.

For LOCAL signing (no CI), the signer can use `cosign sign-blob`
with a hardware token + their own OIDC identity (e.g., a personal
Google account). The verifier doc explains both paths.

---

## Tasks (in order)

### Task 1: `sign.sh`

NEW `tools/release/sign.sh`. Behavior:

```bash
#!/usr/bin/env bash
# Usage: sign.sh <artifact-dir>
# Signs all *.tar.gz + binary artifacts in the directory.
# Requires: cosign installed, OIDC identity configured (CI token or
# interactive browser flow).

set -eu
DIR="${1:-.}"
cd "$DIR"

command -v cosign >/dev/null || {
    echo "sign.sh requires cosign (https://docs.sigstore.dev/system_config/installation/)"
    exit 2
}

# Generate SHA256SUMS over everything we plan to sign.
sha256sum *.tar.gz zclassic23-* 2>/dev/null | sort > SHA256SUMS

# Sign the SHA256SUMS file (one signature covers all artifacts).
cosign sign-blob --yes \
    --output-signature SHA256SUMS.sig \
    --output-certificate SHA256SUMS.pem \
    SHA256SUMS

echo "==> Signed: $DIR/SHA256SUMS"
echo "==> Signature: SHA256SUMS.sig"
echo "==> Certificate: SHA256SUMS.pem"
echo ""
echo "Rekor transparency log entry:"
cosign verify-blob \
    --signature SHA256SUMS.sig \
    --certificate SHA256SUMS.pem \
    --certificate-identity-regexp '.*' \
    --certificate-oidc-issuer-regexp '.*' \
    SHA256SUMS 2>&1 | grep -i 'rekor\|tlog' || true
```

Make executable. **Acceptance:** run on a directory with fake
artifacts, get a SHA256SUMS.sig + .pem out. Don't actually verify
in CI for this PR — verification gets its own task.

### Task 2: `verify.sh`

NEW `tools/release/verify.sh`. Behavior:

```bash
#!/usr/bin/env bash
# Usage: verify.sh <artifact-dir-or-checksums-file>
# Verifies that the SHA256SUMS were signed by the canonical identity
# AND that each listed artifact matches its checksum.

set -eu
TARGET="${1:?usage: verify.sh <dir-or-SHA256SUMS-path>}"

if [ -d "$TARGET" ]; then
    cd "$TARGET"
elif [ -f "$TARGET" ]; then
    cd "$(dirname "$TARGET")"
fi

[ -f SHA256SUMS ] && [ -f SHA256SUMS.sig ] && [ -f SHA256SUMS.pem ] || {
    echo "✗ Missing SHA256SUMS, .sig, or .pem"; exit 2; }

command -v cosign >/dev/null || {
    echo "verify.sh requires cosign (https://docs.sigstore.dev/system_config/installation/)"
    exit 2
}

echo "==> Verifying signature..."
cosign verify-blob \
    --signature SHA256SUMS.sig \
    --certificate SHA256SUMS.pem \
    --certificate-identity-regexp '^https://github\.com/RhettCreighton/zclassic-c/\.github/workflows/release\.yml@refs/tags/v[0-9]+\.[0-9]+\.[0-9]+$' \
    --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
    SHA256SUMS

echo "==> Verifying checksums..."
sha256sum -c SHA256SUMS

echo "✓ Release verified. Signed by canonical CI identity."
```

**Acceptance:** running on the output of a `sign.sh` invocation
(possibly with an identity override flag for local testing) passes.

### Task 3: Makefile integration

EDIT `Makefile`:

```makefile
.PHONY: sign-release verify-release
sign-release: release
	@./tools/release/sign.sh release/

verify-release:
	@./tools/release/verify.sh release/
```

(Assumes `make release` already exists and populates `release/`. If
not, this task adds a stub `release:` target that requires the operator
to manually copy binaries first.)

**Acceptance:** `make sign-release` runs end-to-end on a test
directory; `make verify-release` confirms the signature.

### Task 4: `docs/RELEASE_SIGNING.md`

Operator-facing doc covering:
1. Why this exists (convenience vs trust-minimized; pair with 5c)
2. How to verify a release as a user (`tools/release/verify.sh <file>`)
3. How to sign a release as a maintainer (`make sign-release`)
4. How to verify the OIDC identity in a Rekor entry
5. What to do if verification fails (don't run the binary; report
   issue)

Keep under 150 lines.

**Acceptance:** doc exists, no dead links, accurate to current
cosign CLI flags.

### Task 5: CI workflow (if applicable)

If `.github/workflows/release.yml` exists, add a step that runs
`make sign-release` after the binary is built. The signing happens
automatically using the GitHub Actions OIDC token.

If `release.yml` does NOT exist, skip — the operator can run
`make sign-release` manually after a release. Add a note to
RELEASE_SIGNING.md.

**Acceptance:** workflow file present (if applicable), next release
tag triggers signing.

### Task 6: Final verify + push

```bash
make -j$(nproc)
make lint
./test_parallel --jobs=$(nproc)
# Don't run sign-release here — it requires OIDC identity setup.
git pull --rebase origin main
git push origin main
```

Append Completion section.

---

## What this does NOT do

- Does NOT pin a hardware key (cosign supports it; not used here for
  zero-overhead operations).
- Does NOT sign individual binary artifacts (the SHA256SUMS approach
  covers everything in one signature — cleaner + atomic).
- Does NOT integrate with apt/dpkg/rpm package signing — separate
  toolchain, future work.
- Does NOT cover reproducibility (5c does that).
- Does NOT replace 5c — they are complementary.

---

## Risk + rollback

This PR is purely additive tooling. The existing release flow keeps
working (operators just don't get signatures). Worst-case bugs in
sign.sh produce bad signatures — verify.sh catches them.

If a release is published with a bad signature: re-sign and re-upload
the SHA256SUMS.sig + .pem. The binary itself is unaffected; only the
signature file needs replacement.

---

## Commit cadence

One commit per task. Push after task 4.

---

## Status

**QUEUED** — gated on Phase 5b-1 (so signed binaries are
reproducible-Nix-built). Once 5b-1 lands, this is READY for any
worker.

<!-- Worker: append a Completion section below when done. -->
