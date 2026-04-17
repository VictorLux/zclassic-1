# Agent 2 Task: Wave 29 — PHGR13 Fix (SHIP IT)

## Status
- Your HTLC swap tests merged (21 tests passing). Good work.
- Node at tip (3,079,215), healthy, Tor working, 3 peers.
- PHGR13 is the LAST remaining validation gap. You wrote the investigation in wave 6. Now implement the fix.

## Priority Order

### Task 1: PHGR13 VK — Embed Static VK (MANDATORY, DO FIRST)

You wrote `PHGR13_INVESTIGATION.md`. The diagnosis is solid. Now implement fix sketch items 1+2:

**Option chosen: embed VK as static bytes.** Do NOT try to parse the libsnark binary format.

1. Read `~/.zcash-params/sprout-verifying.key` (1449 bytes)
2. Write a small extraction tool OR manually extract the 7 G1/G2 fields + 10 IC points using the libsnark format you documented (Montgomery LE limbs, OUTPUT_NEWLINE separators, accumulation_vector)
3. Convert each field to canonical big-endian bytes
4. Embed as a `static const uint8_t phgr13_vk_data[]` array in `lib/sapling/src/bn254.c`
5. Replace `ppzksnark_vk_read` call in `params_init.c` with a direct load from the embedded data
6. Verify: `phgr_vk` must be non-NULL after init

**Reference files:**
- `PHGR13_INVESTIGATION.md` — your own investigation, has exact byte offsets
- `lib/sapling/src/bn254.c:1687` — current broken VK reader
- `lib/sapling/src/params_init.c:81-99` — VK load call site
- `lib/sapling/src/sprout.c` — `sprout_verify_phgr13` entry point

### Task 2: Fix G2 Decompression (MANDATORY, SAME COMMIT AS TASK 1)

From your investigation, `bn_g2_decompress` at `bn254.c:1055` is broken:

**Current (wrong):** reads 64 bytes as `concat(c1_32B, c0_32B)`
**Correct:** reads 64 bytes as single 512-bit BE integer, then `c0 = val mod q`, `c1 = val / q`

You need a `u512_divmod_u256` or equivalent. The BN-254 field modulus q is:
`21888242871839275222246405745257275088696311157297823662689037894645226208583`

Both bugs MUST be fixed together — fixing VK alone still fails because G2 points decompress wrong.

### Task 3: PHGR13 Regression Test

Add to `lib/test/src/test_phgr13_fix.c` (already exists):
- Test that `phgr_vk != NULL` after init
- Test G2 compress→decompress roundtrip with a known Zcash test vector
- If possible: extract a real PHGR13 proof from a pre-Sapling block and verify it passes

### Task 4: Make VK Load Hard-Fail on Mainnet

In `lib/sapling/src/params_init.c:98` — change the "Non-fatal if missing" to a hard failure for mainnet. If `phgr_vk == NULL` after load, print a clear error and exit. Silent failures are why this bug survived for months.

## Rules
- `git pull origin master` before starting
- `make -j$(nproc) && make test` before every push
- Commit with `wave 29 task 1-2:` prefix (tasks 1+2 MUST be one commit)
- Tasks 3-4 can be separate commits
