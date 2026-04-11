# Fuzzer & Crash Harness Findings

Every bug found by `fuzz_*`, `crash_recovery_test`, secrets audit, or any other
discovery tool gets logged here before it's fixed. This separates bug discovery
from bug repair per the agent rules.

## Format

| ID | Date       | Location                                  | Severity | Found by           | Owner  | Status  | Fix commit |
|----|------------|-------------------------------------------|----------|--------------------|--------|---------|------------|
| 1  | 2026-04-11 | lib/primitives/src/transaction.c:451      | MED leak | agent2 fuzz_block  | agent2 | OPEN    | —          |

## Severity guide

- CRIT: consensus-breaking, UTXO-destroying, crashes the node, or remote code exec
- HIGH: memory corruption, auth bypass, denial-of-service without authentication
- MED:  memory leak, performance pathology, missing error handling
- LOW:  cosmetic, logging noise, style

## Details

### Finding #1 — transaction_deserialize vin leak

Partial-parse failures in `transaction_deserialize` leak `tx->vin` because
`transaction_free` doesn't walk partially-initialised vin contents. A fuzz input
that successfully parses the first N-1 inputs then fails mid-way through input
N leaks the first N-1 allocations.

Discovered on the first fuzzer run. Assigned to AGENT2.
Reproduction: replay `lib/test/fuzz_seeds/block/header_size.bin` or similar
truncated blob against `fuzz_block`.

