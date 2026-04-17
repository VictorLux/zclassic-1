# Agent 8 — Sapling Crypto Hardening

**Read first:** [`HARDENING_CHECKLIST.md`](../HARDENING_CHECKLIST.md) §R2.2, §R2.3, §R2.8, §R2.9.

**Worktree:** `~/zclassic23-8`
**Branch:** `a8/sapling-hardening`
**Base:** `origin/master`
**Dependencies:** Agent 2's `lib/util/result.h` for D4. Stub locally until A2 merges.

---

## Mission, one sentence

Make the Sapling library safe to feed untrusted bytes to: bounded witness parsing, scrubbed secret material, proper validation instead of `assert`, structured errors on every verification failure, and a fuzz corpus to prove it.

---

## Scope

### Files you own

- `lib/sapling/src/sapling_prover_c23.c` — witness bounds check
- `lib/sapling/src/note_encryption.c` — `memory_cleanse` migration
- `lib/sapling/src/incremental_merkle_tree.c` — assert → return
- `lib/sapling/src/sapling.c` — `zcl_result` migration of verifier functions
- `lib/test/src/test_sapling_witness_bounds.c` **(create)**
- `lib/test/src/test_sapling_memory_cleanse.c` **(create)**
- `lib/test/src/test_sapling_merkle_depth_validation.c` **(create)**
- `lib/test/src/test_sapling_zcl_result.c` **(create)**
- `lib/test/src/test_sapling_fuzz_replay.c` **(create)**
- `lib/test/fuzz_seeds/sapling/` **(create directory)** — corpus files

### Files you MUST NOT touch

- `lib/wallet/`, `app/models/src/wallet*` (agents 2/3)
- `Makefile`, systemd (agent 4)
- `lib/coins/`, `lib/storage/`, `app/models/src/database.c` (agent 5)
- `lib/validation/`, `lib/storage/src/disk_block_io.c`, `app/services/src/snapshot_sync_service.c`, `lib/validation/src/txmempool.c` (agent 6)
- `lib/net/`, `tools/` (agent 7)

Other `lib/sapling/*` files (e.g. `groth16_prover.c`, `sapling_circuit.c`) are technically in your scope but do NOT rewrite proving-path internals — scope discipline. If `groth16_prover.c` needs a tiny fix (e.g. replace `return 0` with a meaningful error for `cs_alloc_var` realloc failure), make it minimal and document in the PR.

---

## Deliverables

### D1. Bound the Sapling witness parser (R2.2)

`lib/sapling/src/sapling_prover_c23.c:167-171`. The function currently reads `witness[0]`, checks `depth == 32`, then indexes up to `witness[1 + 32*33]` = offset 1057 without a length param.

Add `size_t witness_len` to every public entry point that takes a `witness` buffer (search `grep -rn 'const uint8_t \*witness' lib/sapling/`). Minimum validation:

```c
if (witness_len < 1) return ZCL_ERR(SAPLING_ERR_WITNESS_TRUNC, "empty");
uint8_t depth = witness[0];
if (depth != 32) return ZCL_ERR(SAPLING_ERR_BAD_DEPTH, "depth=%u", depth);
if (witness_len < 1 + (size_t)depth * 33)
    return ZCL_ERR(SAPLING_ERR_WITNESS_TRUNC, "have=%zu need=%zu", witness_len, (size_t)1 + depth*33);
```

Every caller must pass the actual length. Update call sites in the same commit.

Regression test (`test_sapling_witness_bounds.c`): feed 100 bytes, assert `SAPLING_ERR_WITNESS_TRUNC`; feed 1057 bytes with `depth=32`, assert success on a well-formed rest.

### D2. Scrub ECDH secrets with `memory_cleanse` (R2.3)

Verified sites in `lib/sapling/src/note_encryption.c`: lines 109, 111, 126, 127, 132, 134, 150, 151 (and maybe more — grep the file for `memset.*, *0, *32`).

Replace every `memset(<secret>, 0, 32)` with `memory_cleanse(<secret>, 32)`. The `memory_cleanse` function is already used correctly elsewhere in the codebase (`groth16_prover.c:598, 788-789`). Include its header wherever needed.

While in this file: add a `#pragma GCC diagnostic warning` or equivalent so future `memset(...,0,32)` on local arrays trips a compile warning. If that's too invasive, add a lint grep to `Makefile` (handoff to Agent 4 for integration).

Regression test (`test_sapling_memory_cleanse.c`): compile a small encrypt-then-zero function under `-O3`; disassemble (or use `volatile`-based check) to confirm the cleanse call was not optimized out. Easier alternative: run under valgrind with `--leak-check=full` and custom wrapper that tags memory — skip if valgrind infra is not present.

### D3. Replace `assert` with bool return in Merkle tree (R2.8)

`lib/sapling/src/incremental_merkle_tree.c:37`:

```c
assert(depth <= MAX_TREE_DEPTH);
```

Change `tree_init` signature to return `bool` (or `int` error code) and propagate. Grep for other `assert(.*depth.*)` and `assert(.*len.*)` in `lib/sapling/` for the same class of bug; fix them in this commit.

Regression test (`test_sapling_merkle_depth_validation.c`): call deserialize with `depth = 255`, assert graceful error in both debug and release builds.

### D4. Migrate verifier functions to `zcl_result` (R2.9)

`lib/sapling/src/sapling.c:484-577` — functions `sapling_check_spend` and `sapling_check_output` (and siblings). Each currently returns `bool`.

Add `enum sapling_err`:
```c
enum sapling_err {
    SAPLING_OK = 0,
    SAPLING_ERR_CV_PARSE,
    SAPLING_ERR_RK_PARSE,
    SAPLING_ERR_PROOF_PARSE,
    SAPLING_ERR_CM_PARSE,
    SAPLING_ERR_GROTH16_VERIFY,
    SAPLING_ERR_WITNESS_TRUNC,
    SAPLING_ERR_BAD_DEPTH,
    SAPLING_ERR_ANCHOR_MISMATCH,
    /* ... as needed ... */
};
```

Return `struct zcl_result` from verifier functions; callers in `lib/validation/` may still boil down to bool but with a specific code logged.

This is a breaking API change within `lib/sapling/`. Migrate all in-tree callers in the same PR. Do not change the Groth16 proving path's internal return values unless strictly necessary.

Regression test (`test_sapling_zcl_result.c`): call each verifier with a known-bad input for each error code, assert the code matches.

### D5. Fuzz corpus (`lib/test/fuzz_seeds/sapling/`)

Create seed files covering:

- `proof_malformed_01.bin` — a Groth16 proof with one bit flipped in each of G1/G2 components (one file per target)
- `point_off_curve_01.bin` — a Jubjub point not on the curve
- `point_small_order_01.bin` — a Jubjub small-order subgroup point (cofactor clearing test)
- `witness_empty.bin` — zero bytes
- `witness_short_100.bin` — 100 bytes (less than 1057)
- `witness_depth_mismatch.bin` — depth byte claims 40, body only 32
- `ciphertext_truncated.bin` — ChaCha20Poly1305 tag missing

`test_sapling_fuzz_replay.c` iterates every seed under `fuzz_seeds/sapling/`, feeds it to the relevant verifier, asserts:
- the call does not abort, segfault, or leak (ASAN sanity),
- the returned `zcl_result` has a non-OK code,
- the code is in the expected SAPLING_ERR_* family (so we don't accept a crash-adjacent false-success).

If there is an existing harness in `lib/test/src/` that wires fuzz corpora, reuse it; otherwise keep the test small and self-contained.

---

## Done when

- [ ] No public function in `lib/sapling/` takes a byte buffer without a length.
- [ ] No `memset(secret, 0, N)` in `note_encryption.c`; all use `memory_cleanse`.
- [ ] No `assert()` on attacker-controlled input in `lib/sapling/`.
- [ ] `sapling_check_spend` / `_output` return `struct zcl_result` with specific error codes.
- [ ] `lib/test/fuzz_seeds/sapling/` has at least 8 corpus files; `test_sapling_fuzz_replay` passes.
- [ ] `make lint` green.
- [ ] `make ci` green.
- [ ] PR title: `a8: sapling hardening — bounds, memory_cleanse, zcl_result, fuzz corpus`

---

## Gotchas

- Do NOT modify circuit logic, field arithmetic, or the proving system itself. Those paths determine consensus; changing them breaks existing shielded transactions. Your changes are wrapper/API only.
- `memory_cleanse` vs `memset`: the compiler's license to elide a `memset` comes from the absence of observable effect on the abstract machine. `memory_cleanse` is implemented with a `volatile` function pointer or inline-asm barrier so the write is observable. Verify the implementation in `lib/util/` before blindly trusting it.
- When migrating to `zcl_result`, callers compiled against the old signature will fail to link. Batch all in-tree callers into the same commit.
- Fuzz seeds must not include a valid proving key or any private data; use known public test vectors (there are Zcash test vectors in the upstream reference if needed). Do not commit anything key-material-shaped.
- If Agent 2's `lib/util/result.h` isn't merged when you start, copy the struct into `lib/sapling/src/sapling_result_stub.h` and delete it in the same PR that follows A2's merge on master.

---

## Hand-off

```
cd ~/zclassic23-8
git push origin a8/sapling-hardening
gh pr create --title "a8: sapling hardening — bounds, memory_cleanse, zcl_result, fuzz corpus" \
             --body "$(cat <<'EOF'
## Summary
Implements HARDENING_CHECKLIST.md §R2.2, §R2.3, §R2.8, §R2.9 + Sapling fuzz corpus.

- Witness parsers now require and validate witness_len
- ECDH secret zeroing uses memory_cleanse (not memset)
- Merkle tree assert() replaced with proper error return
- sapling_check_spend / _output return struct zcl_result with specific codes
- New fuzz corpus under lib/test/fuzz_seeds/sapling/ with 8+ seeds

## Plan
See HARDENING_CHECKLIST.md §R2.2, §R2.3, §R2.8, §R2.9.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
