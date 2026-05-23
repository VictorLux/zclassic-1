# Phase 5 — Crypto agility + reproducible builds + signed releases

**Status:** PLAN (draft 2026-05-23)
**Phase:** 5 (after Phase 4 storage unification)
**Estimated scope:** 5 sub-phases (5a..5e), ~1,500 LOC added (mostly registry + build scaffolding)

> Why this matters: this is the phase that makes the binary
> **defensible** — anyone can reproduce it byte-identically, anyone can
> verify it was signed by us, and we can roll a new crypto primitive
> (e.g., quantum-resistant signatures) without forking.

---

## What's in scope

Three independent threads, sharing one phase because they unlock each
other:

### Thread 1 — Crypto registry (agility)

Today the codebase has hardcoded crypto choices:
- Equihash 200,9 for PoW
- ECDSA secp256k1 for transparent script
- Sapling Groth16 for shielded
- BLAKE2b for some hashes, SHA3 for others, RIPEMD160 for address hashing
- Ed25519 for JoinSplit signatures
- ChaCha20-Poly1305 for note encryption

Each is a CHOICE. Some will become obsolete (Ed25519 vs post-quantum;
Groth16 vs Halo2). The current codebase makes swapping them painful
because the choice is encoded at every call site.

Phase 5a builds a **versioned crypto registry**: a small dispatch
layer that resolves "sig scheme #N at consensus height H" → concrete
implementation. New schemes register; old schemes deprecate;
consensus rules pick which scheme is active at a height.

### Thread 2 — Reproducible builds (`flake.nix`)

Today the binary is reproducible if you have the same compiler version
+ same libc + same exact source. That's a high bar. `flake.nix` pins
everything (compiler, glibc, all transitive deps including `vendor/`)
in a single declarative file. Anyone with Nix can `nix build` and get
byte-identical output.

### Thread 3 — Signed releases (cosign + Rekor)

Once builds are reproducible, signed releases become meaningful. Cosign
signs the artifact; Rekor publishes the signature to a public
transparency log; anyone can verify the chain (signer → signature →
artifact → tree root).

---

## Sub-phases

### 5a — Crypto registry

NEW: `lib/crypto/include/crypto/registry.h` + `registry.c`.

```c
enum crypto_scheme_id {
    /* Hash functions */
    HASH_SHA256        = 0x0001,
    HASH_SHA3_256      = 0x0002,
    HASH_BLAKE2B_256   = 0x0003,
    HASH_RIPEMD160     = 0x0004,
    /* Signature schemes */
    SIG_ECDSA_SECP256K1 = 0x0101,
    SIG_ED25519         = 0x0102,
    /* (future) SIG_ML_DSA_44 = 0x0103, */
    /* (future) SIG_SLH_DSA  = 0x0104, */
    /* Zero-knowledge proof systems */
    ZK_GROTH16_BLS12_381 = 0x0201,
    ZK_PHGR13            = 0x0202,
    /* (future) ZK_HALO2_PASTA = 0x0203, */
    /* Authenticated encryption */
    AEAD_CHACHA20_POLY1305 = 0x0301,
    /* PoW */
    POW_EQUIHASH_200_9 = 0x0401,
};

struct crypto_scheme {
    enum crypto_scheme_id id;
    const char *name;
    int (*verify)(const void *params, ...);   /* family-specific signature */
    bool deprecated;
    int  introduced_consensus_version;
    int  deprecated_consensus_version;        /* -1 if active */
};

void crypto_registry_register(const struct crypto_scheme *scheme);
const struct crypto_scheme *crypto_registry_lookup(enum crypto_scheme_id id);
const struct crypto_scheme *crypto_registry_active_for_consensus(
    enum crypto_family family, int consensus_version);
```

Wave S stages (S-6 script_validate, S-7 proof_validate) consult the
registry. Adding a new scheme is:
1. Implement it in `lib/crypto/src/<name>.c`.
2. `crypto_registry_register(&new_scheme)` at boot.
3. Bump `introduced_consensus_version` on a consensus rule change.

That's it. No call-site edits across the codebase.

**Sub-PR sequence (3 PRs):**
- 5a-1: ship the registry primitive + register existing schemes;
  no call-site changes yet (everyone still hardcodes).
- 5a-2: migrate sig + hash call sites in Wave S stages to use the
  registry (one stage at a time).
- 5a-3: ratchet — add a lint gate that forbids `sha256_update(...)` /
  `ecdsa_verify(...)` etc. outside `lib/crypto/`.

### 5b — `flake.nix`

NEW: top-level `flake.nix`. Pins:
- nixpkgs revision (sha256-locked)
- GCC version (the version we ship with — keeps reproducibility)
- glibc + every transitive system dep
- `vendor/` deps (tor, secp256k1, leveldb if still around) — either
  vendored as Nix derivations OR fetched at locked SHA256

```nix
{
  description = "ZClassic23 — single-binary personal sovereignty stack";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/<sha256-locked>";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; };
      in {
        packages.zclassic23 = pkgs.stdenv.mkDerivation {
          name = "zclassic23";
          src = ./.;
          nativeBuildInputs = [ pkgs.gcc14 pkgs.gnumake ];
          buildInputs = [ pkgs.openssl pkgs.libevent pkgs.sqlite ];
          buildPhase = "make -j$NIX_BUILD_CORES";
          installPhase = ''
            mkdir -p $out/bin
            cp zclassic23 zclassic-cli test_zcl $out/bin/
          '';
        };
        devShell = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.zclassic23 ];
        };
      });
}
```

After 5b ships:
- `nix build .#zclassic23` produces byte-identical output on any
  machine with Nix installed.
- `nix develop` drops you into the exact build environment.
- CI gets simpler — one command.

**Sub-PR sequence (2 PRs):**
- 5b-1: ship `flake.nix` + `flake.lock`. Verify `nix build` works on
  at least 2 machines and produces identical sha256.
- 5b-2: wire CI to build via `nix build` so every PR is reproducible.

### 5c — `make verify-reproducibility`

Helper script that:
1. Builds the binary twice in clean environments.
2. Compares sha256.
3. If different, prints the diff source (likely embedded timestamp,
   path leak, or non-deterministic linker order).

Standard issues:
- `__DATE__` / `__TIME__` macros in source — replace with build-time
  constant from CI.
- Embedded build path — `make` ARG to pass relative paths only.
- Linker order — `-Wl,--sort-section=name` if needed.

### 5d — Cosign signing

NEW: `tools/release/sign.sh` (wrapper around cosign).

```bash
#!/usr/bin/env bash
# Build, sign, publish to Rekor.
nix build .#zclassic23
cosign sign-blob \
  --bundle zclassic23-v0.X.Y.bundle \
  result/bin/zclassic23
# Verifiable later via:
# cosign verify-blob --bundle zclassic23-v0.X.Y.bundle result/bin/zclassic23
```

Sign key: stored offline; signing happens in a quorum-ceremony for
releases. Day-to-day developer builds aren't signed.

### 5e — Rekor transparency log entries

After signing, push the signature bundle to the public Rekor server
(`https://rekor.sigstore.dev`). Result: anyone can audit the chain of
signatures over time. A compromise of our signing key shows up as a
signature with no Rekor entry, which clients can refuse.

---

## What this unlocks

After Phase 5 lands:

- **Audit-friendly releases.** Anyone can verify a binary's lineage
  back to a specific git commit + signing key.
- **Post-quantum crypto migration becomes a 1-PR change.** New scheme
  registers in the registry; consensus version bump activates it at a
  future block.
- **Reproducible regression hunting.** Bug from a specific binary
  build? `nix build` that exact build, attach a debugger, reproduce.
- **Smaller supply-chain surface.** Nix-pinned dependencies eliminate
  "drift in system libraries between developer machines."

---

## Risk + mitigations

- **Flake adds developer friction.** Mitigation: keep `make` working
  unchanged for non-Nix developers; flake is for reproducibility + CI,
  not required for day-to-day work.
- **Crypto registry adds indirection overhead.** Mitigation: registry
  lookup is one indexed array access; the verify call itself dominates
  cost. Benchmarked in 5a-1.
- **Signing-key compromise is catastrophic.** Mitigation: quorum-of-N
  signing ceremony (cosign supports this); offline storage; rotation
  schedule documented in `docs/operations/signing-ceremony.md`
  (to write in 5d).

---

## Status

DRAFT — actionable after Phase 4 storage unification ships. Phase 4
removes 4 dependencies (LevelDB, etc.) which simplifies the reproducible-
build surface significantly.

When ready, the first sub-PR is `wt?-phase5a-1-crypto-registry.md`.
