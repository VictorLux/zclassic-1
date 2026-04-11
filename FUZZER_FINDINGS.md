# Fuzzer & Crash Harness Findings

Every bug found by `fuzz_*`, `crash_recovery_test`, secrets audit, or any other
discovery tool gets logged here before it's fixed. This separates bug discovery
from bug repair per the agent rules.

## Format

| ID | Date       | Location                                  | Severity | Found by           | Owner  | Status  | Fix commit |
|----|------------|-------------------------------------------|----------|--------------------|--------|---------|------------|
| 1  | 2026-04-11 | lib/primitives/src/transaction.c:61-62    | MED leak | agent2 fuzz_block  | agent2 | FIXED   | e53dc59ef  |

## Severity guide

- CRIT: consensus-breaking, UTXO-destroying, crashes the node, or remote code exec
- HIGH: memory corruption, auth bypass, denial-of-service without authentication
- MED:  memory leak, performance pathology, missing error handling
- LOW:  cosmetic, logging noise, style

## Details

### Finding #1 — transaction_alloc zero-size calloc stub leak — FIXED

**Status:** FIXED in `e53dc59ef` (wave 4 session 3, 2026-04-11).

**Actual root cause** (differs from original diagnosis): Partial-parse failures
in `transaction_deserialize` leaked a **1-byte** allocation, not the vin array.
`transaction_alloc(tx, num_vin, 0)` called `calloc(0, sizeof(tx_out))`, which
on glibc returns a unique 1-byte pointer (the standard leaves `calloc(0, _)`
implementation-defined and glibc picks "valid freeable stub"). The
deserializer then unconditionally overwrote `tx->vout` with a fresh
`calloc(num_vout, ...)` once it parsed the real num_vout from the byte stream,
silently leaking the 1-byte stub. On failure paths the stub-that-was-just-
overwritten was never freed.

The vin array itself is freed correctly by `transaction_free` because it's a
flat allocation with no nested heap pointers (`tx_in.script_sig.data` is an
inline MAX_SCRIPT_SIZE buffer, not a heap pointer).

**Fix:** treat zero-size as "no array" in `transaction_alloc` so the pointer
stays NULL and the later overwrite is allocation-neutral:

```c
tx->vin  = num_vin  ? calloc(num_vin,  sizeof(struct tx_in))  : NULL;
tx->vout = num_vout ? calloc(num_vout, sizeof(struct tx_out)) : NULL;
```

**Verification.** 20s runs of all three fuzzers under ASAN+LSAN after the fix:
`fuzz_block` 395k execs, `fuzz_script` 1.47M execs, `fuzz_p2p` 38k execs —
zero leaks, zero crashes. `make fuzz-ci-leaks` is now a green baseline.

**Regression test:** `lib/test/src/test_primitives.c` — "transaction_alloc
zero-size leaves pointers NULL" covers (0,0), (0,N), (N,0), and the
deserializer-style overwrite pattern.

