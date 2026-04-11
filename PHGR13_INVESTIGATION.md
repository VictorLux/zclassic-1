# PHGR13 Sync Stall — Investigation Report

**Date:** 2026-04-11
**Author:** agent2 (wave 6 carry-over from wave 5 item #2)
**Status:** hypothesis + fix sketch — **do not commit fix in this session**
**Owner for fix:** AGENT1 review required

---

## TL;DR

Two independent bugs in `lib/sapling/` make every Sprout PHGR13 JoinSplit
fail verification:

1. **Primary — VK file parser is wrong.** `ppzksnark_vk_read` in
   `lib/sapling/src/bn254.c:1687` assumes a flat hand-rolled
   `concat(G1_BE, G2_BE)` layout for `~/.zcash-params/sprout-verifying.key`.
   The file is actually libsnark's native `operator<<` binary format with
   OUTPUT_NEWLINE separators, Montgomery-form Fp limbs in **native (LE) byte
   order**, and a nested `accumulation_vector`/`sparse_vector` structure for
   `encoded_IC_query`. Result: `ppzksnark_vk_read` returns `false`, the
   `phgr_vk` global stays `NULL`, and `sprout_verify_phgr13` short-circuits
   to `return false` on its very first line. Every joinsplit rejected with
   `bad-txns-joinsplit-phgr13-invalid`.

2. **Secondary (would still bite after #1 is fixed) — G2 compressed-point
   decoder misreads `Fq2`.** `bn_g2_decompress` in `bn254.c:1055` reads the
   64-byte x-coordinate as two independent 32-byte BE Fq elements
   (`c1 || c0`). Zcash's `CompressedG2` wire format is FE2IP: the 64 bytes
   are a single **512-bit big-endian integer** `val = c1*q + c0`, and the
   decoder must do `c0 = val mod q`, `c1 = val div q`. See the C++
   reference at `zclassic-cpp/src/zcash/Proof.cpp:85-95` (`fq2_to_bigint`
   and `Fq2::to_libsnark_fq2`).

Consequence: even if we hard-code a correct VK, every proof's `g_B`
decompresses to the wrong curve point and the pairing checks still fail.
Both bugs must land in the same fix.

This matches the observed symptom — the node stuck at h=2,014,948 with
every historical block containing a Sprout joinsplit rejected as
`bad-txns-joinsplit-phgr13-invalid`. (Pre-Sapling blocks still carry
PHGR13 proofs; only v4 Sapling txs use Groth16. See
`lib/primitives/src/transaction.c:509` — `use_groth = tx->overwintered &&
tx->version >= SAPLING_TX_VERSION`.)

---

## Reproduction (static analysis, no running node)

### Step 1 — `phgr_vk` is never loaded

`lib/sapling/src/params_init.c:81-99` reads the 1449-byte
`sprout-verifying.key` and passes it to `ppzksnark_vk_read`. The reader
assumes this layout:

```
  alpha_a_g2     (128 bytes: 4 × 32-byte BE Fq)
  alpha_b_g1     ( 64 bytes: 2 × 32-byte BE Fq)
  alpha_c_g2     (128)
  gamma_g2       (128)
  gamma_beta_g1  ( 64)
  gamma_beta_g2  (128)
  rc_z_g2        (128)   → cumulative offset 768
  ic_len         (  4 bytes uint32 LE)  → offset 772
  ic[0..ic_len]  ( 64 each)
```

At offset 768 in the real file we find:

```
$ dd if=$HOME/.zcash-params/sprout-verifying.key bs=1 skip=768 count=4 | od -An -tx1
 3f 1d c4 1e
```

Decoded as LE uint32 → `ic_count = 0x1ec41d3f = 516,817,727`.

The next line of the reader sanity-checks
`off + ic_count * 64 > len` → `772 + 33,076,334,528 > 1449` → `true`
→ `return false`.

`sapling_init_params` treats this as non-fatal:

```c
/* Non-fatal if missing — PHGR13 proofs just won't be verified */
```

and prints `WARNING: Failed to parse sprout-verifying.key` to stderr —
easy to miss in the 10-second tor-bootstrap noise on startup. After this
moment, `phgr_vk == NULL` forever.

### Step 2 — the first line of `sprout_verify_phgr13` returns `false`

`lib/sapling/src/bn254.c:1909`:

```c
bool sprout_verify_phgr13(const uint8_t proof[296], ...)
{
    if (!phgr_vk)
        return false;   // ← every PHGR13 proof lands here
    ...
}
```

### Step 3 — `contextual_check_tx` rejects

`lib/validation/src/contextual_check_tx.c:178-185`:

```c
REJECT_UNLESS(sprout_verify_phgr13(js->proof, ...),
              state, 100, "bad-txns-joinsplit-phgr13-invalid");
```

DoS score 100 → peer banned, block rejected. The symptom in
AGENT2.md matches exactly.

---

## Why the VK parser assumed the wrong format

`lib/sapling/src/bn254.c:1689-1702` carries a docstring:

```
 * The sprout-verifying.key format from libsnark:
 *   Each G1 point: 64 bytes (Fq x, Fq y as 32-byte BE each)
 *   Each G2 point: 128 bytes (Fq2 x=(c0,c1), Fq2 y=(c0,c1) as 32-byte BE each)
```

That layout is **not** libsnark's binary format. libsnark serializes a
`r1cs_ppzksnark_verification_key` via
`operator<<(ostream&, const r1cs_ppzksnark_verification_key<ppT>&)`
(see `src/snark/libsnark/zk_proof_systems/ppzksnark/r1cs_ppzksnark/r1cs_ppzksnark.tcc:79`):

```cpp
out << vk.alphaA_g2 << OUTPUT_NEWLINE;
out << vk.alphaB_g1 << OUTPUT_NEWLINE;
out << vk.alphaC_g2 << OUTPUT_NEWLINE;
out << vk.gamma_g2 << OUTPUT_NEWLINE;
out << vk.gamma_beta_g1 << OUTPUT_NEWLINE;
out << vk.gamma_beta_g2 << OUTPUT_NEWLINE;
out << vk.rC_Z_g2 << OUTPUT_NEWLINE;
out << vk.encoded_IC_query << OUTPUT_NEWLINE;
```

Differences the C parser doesn't handle:

| What libsnark writes | What the C parser assumes |
|---|---|
| `OUTPUT_NEWLINE` (single byte) separators between every top-level field | No separators |
| `Fp_model::operator<<` — Montgomery-form limbs, 8 × `mp_limb_t` = 32 bytes in **native LE** order | 32 bytes canonical big-endian |
| `G1::operator<<` — writes affine x, y **(and possibly Z in projective form)** | Exactly 2 × 32 bytes, no extra |
| `accumulation_vector<G1>` — `first` G1 followed by a `sparse_vector<G1>` (indices vector + values vector + domain_size + own newlines) | Flat `ic_len:u32le` followed by contiguous 64-byte G1s |

Any one of those mismatches is enough to reject the file; all four miss
simultaneously. The reader's assumption is inherited verbatim from early
bootstrap code and has no unit test — `grep` confirms zero call sites
exercising `sprout_verify_phgr13` from `lib/test/`:

```
$ grep -rn "sprout_verify_phgr\|bn_g2_decompress" lib/test/
(no results)
```

So this path has literally never been checked for round-tripping. It
shipped red.

---

## Why `bn_g2_decompress` is also broken

`lib/sapling/src/bn254.c:1066-1069`:

```c
/* G2 x-coordinate is in Fq2: serialized as c1 (32 bytes) then c0 (32 bytes) */
if (!bn_fq_from_bytes_be(&p->x.c1, data + 1))
    return false;
if (!bn_fq_from_bytes_be(&p->x.c0, data + 33))
    return false;
```

The Zcash wire format for `CompressedG2` is defined in
`zclassic-cpp/src/zcash/Proof.cpp:25-34` (function `fq2_to_bigint`) and
`:85-96` (`Fq2::to_libsnark_fq2`):

```cpp
// encoder
bigint<8> fq2_to_bigint(const curve_Fq2 &e) {
    auto modq = curve_Fq::field_char();
    auto c0   = e.c0.as_bigint();
    auto c1   = e.c1.as_bigint();
    bigint<8> temp = c1 * modq;
    temp += c0;
    return temp;                    // serialized as 64-byte BE in Fq2::data
}

// decoder
curve_Fq2 Fq2::to_libsnark_fq2() const {
    bigint<4> modq      = curve_Fq::field_char();
    bigint<8> combined  = read_bigint<8>(data);   // 64 bytes BE → 512-bit int
    bigint<5> res;
    bigint<4> c0;
    bigint<8>::div_qr(res, c0, combined, modq);   // c0 = combined mod q
    bigint<4> c1 = res.shorten(modq, "element is not in Fq2");
    return curve_Fq2(curve_Fq(c0), curve_Fq(c1));
}
```

So the 64 bytes are one 512-bit BE integer `combined = c1·q + c0`, and
`c0 = combined mod q`, `c1 = combined / q`. The C code instead interprets
the high 32 bytes as `c1` and the low 32 as `c0` — a completely different
map. For BN-254 (`q ≈ 2^254`), `c1·q mod 2^256` is a near-uniform
function of `c1`, so `concat(c1, c0)` almost never equals the FE2IP
encoding of the same `(c0, c1)`.

The y-sign recovery at `bn254.c:1218-1241` then does a lexicographic
`(c1, c0)` compare to decide between `y` and `-y`. This happens to be
equivalent to the C++ `fq2_to_bigint(y) > fq2_to_bigint(-y)` check —
because `fq2_to_bigint = c1·q + c0` is strictly monotone in `c1` for
`c1 < q` — so the sign bit logic would survive the first fix. But the
x-coordinate misread still dooms every call.

---

## Diff vs `zclassic-cpp` (reference)

| Concern | `zclassic-cpp` (libsnark) | `zclassic23-2` (this repo) | Same? |
|---|---|---|---|
| VK file format | libsnark `operator<<` with `OUTPUT_NEWLINE` separators, Montgomery limbs in native byte order, nested accumulation_vector | Flat concat of 32-byte BE Fq elements, u32-LE ic_len prefix | **NO** |
| `CompressedG2` x encoding | FE2IP (`c1*q + c0` as 512-bit BE) with div/mod decode | Direct concat of two 32-byte BE Fq | **NO** |
| `CompressedG2` y sign | `fq2_to_bigint(y) > fq2_to_bigint(-y)` | Lexicographic `(c1, c0)` > `(-c1, -c0)` | Equivalent (monotonicity of FE2IP in c1) |
| `CompressedG1` x encoding | Single 32-byte BE Fq after 1 flag byte | Same | YES |
| `CompressedG1` y sign | `y.as_bigint().data[0] & 1` (LSB of y) | Same | YES |
| Primary-input bit packing (witness_map) | `insert_uint256`/`insert_uint64` (byte-LE, bit-MSB-first) + `pack_bit_vector_into_field_element_vector` (LSB-first within 253-bit chunks) | `bn254_multipack_be` — identical byte ordering and LSB-first-within-limb packing | YES |
| Pairing verification (5 checks) | `r1cs_ppzksnark_online_verifier_weak_IC` — same 5 pairing products | `ppzksnark_verify` — mirrors the same 5 checks with matching sign conventions | YES (modulo the input points it never successfully receives) |

So the packing/pairing arithmetic looks fine. The failure is entirely
upstream at I/O.

---

## Hypothesis (one sentence)

**`sprout-verifying.key` is a libsnark native binary file (`operator<<`
with Montgomery limbs, newline separators, and a nested
`accumulation_vector` for IC), but `ppzksnark_vk_read` parses it as a
flat concat of big-endian canonical Fq elements, so the VK load always
returns `false`, leaving `phgr_vk == NULL` and making every PHGR13
joinsplit verification short-circuit to "invalid".**

A secondary bug in `bn_g2_decompress` (reading `Fq2` as `concat(c1, c0)`
instead of FE2IP `c1·q + c0`) would still break proof verification once
the VK loads correctly, so both must be fixed in the same change.

---

## Fix sketch (5-line summary, do not commit this session)

```
1. Replace ppzksnark_vk_read with a parser tailored to libsnark's binary
   operator<<, OR (preferred) ship a pre-parsed static VK embedded in
   source as a byte array of the 7 fixed fields + 10 IC points in
   canonical (big-endian, non-Montgomery) form. Avoid a runtime
   dependency on a binary format we don't own.

2. Rewrite bn_g2_decompress to decode the 64-byte x-coordinate as
   combined := u512_from_bytes_be(data + 1);
   c0 := combined mod q;  c1 := combined div q;  assert c1 < q;
   (existing sign-bit logic at lines 1218-1241 is correct — keep it.)

3. Add a round-trip test lib/test/src/test_sapling_crypto.c covering:
   - CompressedG1 encode→decode identity (trivial)
   - CompressedG2 encode→decode identity from a known-good Zcash test vector
   - sprout_verify_phgr13 against a real mainnet joinsplit proof extracted
     from a pre-Sapling block. (Tools/extract_phgr13_testvector.c — small
     helper that reads block N from ~/.zclassic-c23/blocks and dumps the
     first joinsplit.) This is the regression test that should have
     existed on day 1.

4. Gate on test: after landing, run ./test_zcl and confirm the node is
   able to advance past h=2,014,948 on a real datadir. (Don't commit
   until the live node clears the stall.)

5. Audit lib/sapling/params_init.c:98 — change "Non-fatal if missing"
   to hard-fail: if mainnet is the active chain and phgr_vk can't load,
   refuse to boot with EV_CHAIN_STATE_INVALID and a clear error message.
   The silent stderr warning is why this bug rotted for a year.
```

---

## What I ruled out

- **Not the bit packing.** `bn254_multipack_be` vs libsnark's
  `pack_bit_vector_into_field_element_vector`: both process 253-bit
  chunks, both take bits MSB-first within each byte and place them
  LSB-first within each scalar limb. The intermediate byte layout
  (uint256 raw 32 bytes, uint64 LE) is identical between
  `insert_uint256`/`insert_uint64` in `zclassic-cpp` and the C
  `input[]` construction in `sprout_verify_phgr13`. No off-by-one here.

- **Not the pairing checks.** The 5 pairing products in `ppzksnark_verify`
  map 1:1 to `r1cs_ppzksnark_online_verifier_weak_IC`: `(A,αA)·(-A',1)`,
  `(αB,B)·(-B',1)`, `(C,αC)·(-C',1)`, `(A+acc,B)·(-H,rCZ)·(-C,1)`, and
  `(K,γ)·(-(A+acc+C),γβ₂)·(-γβ₁,B)`. Sign handling and the G2 generator
  hex constants (bn254.c:1785-1800) also match libsnark.

- **Not the G1 decoder.** `bn_g1_decompress` reads 32 bytes BE after the
  flag byte and uses `y_lsb = data[0] & 1`, which matches
  `CompressedG1::to_libsnark_g1` exactly.

- **Not the tx version gate.** `use_groth = overwintered &&
  version >= SAPLING_TX_VERSION` matches ZClassic consensus. A v4 Sapling
  tx with an embedded Sprout joinsplit correctly goes through
  `sprout_verify_groth16` (which has working Groth16 params loaded from
  `sprout-groth16.params`); only pre-Sapling v2/v3 joinsplits hit
  `sprout_verify_phgr13`.

- **Not the G2 y-sign comparison.** Lexicographic `(c1, c0)` vs `(q-c1,
  q-c0)` is equivalent to libsnark's `fq2_to_bigint(y) > fq2_to_bigint(-y)`
  because FE2IP is strictly monotone in `c1` (since `c0 < q`). Sign-bit
  logic survives.

---

## Open questions for AGENT1

1. Is there a reason PHGR13 verification was marked **non-fatal** at
   startup (`params_init.c:98`)? If the node is a testnet/regtest node
   that never sees pre-Sapling blocks, we could argue for the soft fail;
   for mainnet it should be hard. Is the goal to keep archival-mode
   mainnet nodes bootable on machines without the full params set?

2. Do we want to embed the PHGR13 VK as a static byte constant in the
   source (my preference — removes a file dependency and its parsing
   risk) or keep parsing the file and replace the parser with a
   libsnark-faithful one? The VK is ~1.4 KB so the static-array path is
   cheap.

3. Is there a captured mainnet PHGR13 joinsplit anywhere in the repo we
   can use as a test vector, or does the test need a `tools/extract_phgr13_testvector.c`
   helper that pulls one out of a live datadir?

Please review and pick a direction — I'll do the actual fix in a
dedicated session with the regression test driving it.
