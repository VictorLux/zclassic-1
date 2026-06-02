#!/usr/bin/env bash
# tools/release.sh — Build a tagged release of zclassic23.
#
# Creates:
#   release/zclassic23-v{VERSION}-linux-x86_64.tar.gz
#   release/zclassic23-v{VERSION}-linux-x86_64.sha3
#   release/zclassic23-v{VERSION}-linux-x86_64.sha3.sig  (if GPG key available)
#
# Usage:
#   ./tools/release.sh              # auto-detect version from clientversion.h
#   ./tools/release.sh v0.1.0       # explicit tag
#   ./tools/release.sh --verify     # verify an existing release archive
#   ./tools/release.sh --unsigned   # allow an unsigned release (else hard-fail)
#
# By default a release MUST be GPG-signed: if no secret key is available the
# script aborts. Pass --unsigned (or set ZCL_ALLOW_UNSIGNED=1) to override.
#
# Reproducible: records compiler, build flags (CFLAGS/LDFLAGS), git rev, and
# uname in BUILDINFO.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ---------- helpers ----------------------------------------------------------

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# ---------- version detection ------------------------------------------------

VERSION_H="lib/util/include/util/clientversion.h"
[ -f "$VERSION_H" ] || die "Cannot find $VERSION_H"

V_MAJOR=$(grep '#define CLIENT_VERSION_MAJOR'    "$VERSION_H" | awk '{print $3}')
V_MINOR=$(grep '#define CLIENT_VERSION_MINOR'    "$VERSION_H" | awk '{print $3}')
V_REV=$(grep   '#define CLIENT_VERSION_REVISION' "$VERSION_H" | awk '{print $3}')
V_BUILD=$(grep '#define CLIENT_VERSION_BUILD'    "$VERSION_H" | awk '{print $3}')

AUTO_VERSION="v${V_MAJOR}.${V_MINOR}.${V_REV}-b${V_BUILD}"

# ---------- mode selection ---------------------------------------------------

MODE="build"
TAG=""
ALLOW_UNSIGNED="${ZCL_ALLOW_UNSIGNED:-0}"

# Consume the --unsigned flag wherever it appears; it does not affect --verify.
ARGS=()
for arg in "$@"; do
    if [ "$arg" = "--unsigned" ]; then
        ALLOW_UNSIGNED=1
    else
        ARGS+=("$arg")
    fi
done
set -- "${ARGS[@]+"${ARGS[@]}"}"

if [ "${1:-}" = "--verify" ]; then
    MODE="verify"
    shift
    ARCHIVE="${1:-}"
    [ -n "$ARCHIVE" ] || die "Usage: $0 --verify <archive.tar.gz>"
    [ -f "$ARCHIVE" ]  || die "File not found: $ARCHIVE"
elif [ -n "${1:-}" ]; then
    TAG="$1"
else
    TAG="$AUTO_VERSION"
fi

# ---------- verify mode ------------------------------------------------------

if [ "$MODE" = "verify" ]; then
    info "Verifying release archive: $ARCHIVE"

    SHA3_FILE="${ARCHIVE%.tar.gz}.sha3"
    SIG_FILE="${SHA3_FILE}.sig"

    # Check SHA3-256
    if [ -f "$SHA3_FILE" ]; then
        EXPECTED=$(awk '{print $1}' "$SHA3_FILE")
        ACTUAL=$(openssl dgst -sha3-256 "$ARCHIVE" | awk '{print $NF}')
        if [ "$EXPECTED" = "$ACTUAL" ]; then
            info "SHA3-256: OK ($ACTUAL)"
        else
            die "SHA3-256 MISMATCH!\n  expected: $EXPECTED\n  actual:   $ACTUAL"
        fi
    else
        echo "WARN: No .sha3 file found at $SHA3_FILE"
    fi

    # Check GPG signature
    if [ -f "$SIG_FILE" ]; then
        if command -v gpg >/dev/null 2>&1; then
            if gpg --verify "$SIG_FILE" "$SHA3_FILE" 2>/dev/null; then
                info "GPG signature: OK"
            else
                die "GPG signature verification FAILED"
            fi
        else
            echo "WARN: gpg not installed, cannot verify signature"
        fi
    else
        echo "WARN: No .sig file found at $SIG_FILE (unsigned release)"
    fi

    # Extract and check BUILDINFO
    if tar -tzf "$ARCHIVE" 2>/dev/null | grep -q BUILDINFO; then
        info "BUILDINFO:"
        tar -xzf "$ARCHIVE" --to-stdout "*/BUILDINFO" 2>/dev/null || true
    fi

    info "Verification complete."
    exit 0
fi

# ---------- build mode -------------------------------------------------------

info "Building release: $TAG"

ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
RELEASE_NAME="zclassic23-${TAG}-${OS}-${ARCH}"
RELEASE_DIR="$REPO_ROOT/release"
STAGING="$RELEASE_DIR/$RELEASE_NAME"

# Clean previous staging
rm -rf "$STAGING"
mkdir -p "$STAGING"

# Build from clean
info "Running: make clean && make zclassic23 zclassic-cli"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" zclassic23 zclassic-cli 2>&1 | tail -3

# Verify binaries exist
[ -f zclassic23 ]   || die "Build failed: zclassic23 not found"
[ -f zclassic-cli ] || die "Build failed: zclassic-cli not found"

# Collect build metadata
GIT_REV=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY=$(git diff --quiet 2>/dev/null && echo "clean" || echo "dirty")

# The build flags live in the Makefile, so learn them the way the build does:
# ask make to print its resolved variable definitions. The load-bearing
# reproducibility fields are -march (CFLAGS) and -flto (CFLAGS + LDFLAGS).
make_var() { make -pn 2>/dev/null | grep -E "^$1 = " | head -1 | cut -d= -f2- | sed 's/^ //'; }
CC="${CC:-$(make_var CC)}"
CC="${CC:-cc}"
CFLAGS=$(make_var CFLAGS)
LDFLAGS=$(make_var LDFLAGS)
COMPILER=$($CC --version 2>/dev/null | head -1 || echo "unknown")
BUILD_DATE=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > "$STAGING/BUILDINFO" <<BUILDINFO
ZClassic23 Release: $TAG
Build date:   $BUILD_DATE
Git revision: $GIT_REV ($GIT_BRANCH, $GIT_DIRTY)
Compiler:     $COMPILER
CFLAGS:       $CFLAGS
LDFLAGS:      $LDFLAGS
Platform:     $(uname -srm)
Binary size:  $(stat -c%s zclassic23 2>/dev/null || stat -f%z zclassic23) bytes
CLI size:     $(stat -c%s zclassic-cli 2>/dev/null || stat -f%z zclassic-cli) bytes
BUILDINFO

info "BUILDINFO:"
cat "$STAGING/BUILDINFO"

# Copy binaries
cp zclassic23 zclassic-cli "$STAGING/"

# Strip debug symbols for release (keep a copy)
if command -v strip >/dev/null 2>&1; then
    strip "$STAGING/zclassic23"
    strip "$STAGING/zclassic-cli"
    info "Stripped binaries"
fi

# Copy essential files
cp LICENSE "$STAGING/" 2>/dev/null || true
cp README.md "$STAGING/" 2>/dev/null || true

# Create tarball
TARBALL="$RELEASE_DIR/${RELEASE_NAME}.tar.gz"
info "Creating archive: $TARBALL"
(cd "$RELEASE_DIR" && tar -czf "${RELEASE_NAME}.tar.gz" "$RELEASE_NAME")

# SHA3-256 hash
SHA3_FILE="$RELEASE_DIR/${RELEASE_NAME}.sha3"
HASH=$(openssl dgst -sha3-256 "$TARBALL" | awk '{print $NF}')
echo "$HASH  ${RELEASE_NAME}.tar.gz" > "$SHA3_FILE"
info "SHA3-256: $HASH"

# GPG detached signature (required unless explicitly waived)
SIG_FILE="${SHA3_FILE}.sig"
if command -v gpg >/dev/null 2>&1 && gpg --list-secret-keys 2>/dev/null | grep -q sec; then
    info "Signing with GPG..."
    gpg --detach-sign --armor -o "$SIG_FILE" "$SHA3_FILE"
    info "Signature: $SIG_FILE"
elif [ "$ALLOW_UNSIGNED" = "1" ]; then
    echo "WARN: No GPG secret key found; producing an UNSIGNED release (waived)."
    echo "      To sign: gpg --detach-sign --armor -o ${SIG_FILE} ${SHA3_FILE}"
else
    die "No GPG secret key found — refusing to produce an unsigned release.\n  Install/import a signing key, or re-run with --unsigned (or ZCL_ALLOW_UNSIGNED=1) to override."
fi

# Git tag (if not already tagged)
if git rev-parse "$TAG" >/dev/null 2>&1; then
    info "Tag $TAG already exists, skipping git tag"
else
    info "Creating git tag: $TAG"
    git tag -a "$TAG" -m "Release $TAG"
fi

# Clean up staging directory
rm -rf "$STAGING"

# Summary
echo ""
echo "============================================"
echo "  Release $TAG complete"
echo "============================================"
echo "  Archive:   $TARBALL"
echo "  SHA3-256:  $SHA3_FILE"
[ -f "$SIG_FILE" ] && echo "  Signature: $SIG_FILE"
echo "  Tag:       $TAG"
echo ""
echo "To verify:   ./tools/release.sh --verify $TARBALL"
echo "To push tag: git push origin $TAG"
echo "============================================"
