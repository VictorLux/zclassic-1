# Chaos Harness

The Phase 6c chaos harness runs declarative scenarios through
`zclassic23-chaos`. It is intentionally small: each non-comment line is one
command, arguments are whitespace-separated, and assertions use simple integer
comparisons.

Run every checked-in scenario:

```bash
make chaos
```

Run one scenario with command-level progress:

```bash
make zclassic23-chaos
./zclassic23-chaos --scenario=tools/sim/scenarios/peer_churn.scenario --verbose
```

## Scenario Format

Comments start with `#`. Blank lines are ignored. A minimal scenario looks like:

```text
seed        0x0000000000000001
boot_phase  idb_complete
peer_count  0
advance_clock +60s

expect      no_crash
expect      consensus_rejects == 0
```

Keep scenarios short and deterministic. Prefer explicit seeds and concrete
assertions over broad smoke checks.

## Commands

`seed HEX`
: Sets the scenario seed. Hex and decimal values are accepted.

`boot_phase idb_complete|listening|mempool_open`
: Selects the simulated boot phase reached before injected events run.

`peer_count N`
: Creates `N` in-process simulated peers.

`at_event HEIGHT COMMAND [ARGS...]`
: Records a scheduled event and dispatches the nested command immediately.
  This gives scenarios the same shape as future height-driven replay while the
  current harness remains single-pass.

`kill_peer ID`
: Disconnects a configured simulated peer.

`send_block peer=I file=PATH`
: Reads a non-empty fixture file from a connected peer, records the simulated
  send, and advances `tip_height`. Full consensus validation is future work.

`send_malformed_block peer=I type=ENUM`
: Simulates a bad block from a connected peer and increments
  `consensus_rejects`. Valid types are `invalid_pow`, `bad_merkle`,
  `bad_timestamp`, `bad_size`, `bad_coinbase`, `duplicate_tx`, `bad_bits`, and
  `bad_nonce`.

`advance_clock +DURATION`
: Moves the virtual platform clock forward. Durations use `s`, `m`, `h`, or
  `d`, for example `+30s` or `+1h`.

`trigger_oom_at LABEL`
: Arms the one-shot safe allocation fault hook and verifies the next allocation
  at `LABEL` fails.

`partition_network for=DURATION`
: Arms the network partition hook until the virtual wall clock passes the
  duration.

`expect no_crash`
: Asserts that the scenario did not mark itself crashed.

`expect METRIC OP VALUE`
: Compares a metric with `==`, `!=`, `>=`, `<=`, `>`, or `<`. Current metrics:
  `tip_height`, `reorg_count`, `consensus_rejects`, `mempool_prune_runs`,
  `active_peers`, `killed_peers`, `blocks_sent`, `malformed_blocks`,
  `clock_advance_count`, `scheduled_events`, `alloc_faults`, and `sim_time`.

## Adding Fault Injection

Add the production hook first, defaulting to inactive and cheap on the hot path.
Then add a narrow chaos command that arms the hook and proves it fired. Keep
the command deterministic, update `test_chaos_harness`, and add a scenario only
after the command has focused unit coverage.

Existing examples:

- Allocation faults: `lib/util/src/safe_alloc.c` plus `trigger_oom_at`.
- Network partitions: `lib/net/src/net_fault.c` plus `partition_network`.

## From Capsule To Scenario

When a postmortem capsule exposes a replayable failure, convert it into the
smallest scenario that preserves the causal shape:

1. Use the capsule seed as the scenario `seed`.
2. Map the boot state to `boot_phase`.
3. Convert peer disconnects, clock movement, malformed inputs, OOM labels, and
   network stalls into chaos commands.
4. Add the assertion that would have caught the bug, usually `expect no_crash`
   plus a specific metric.
5. Check in the scenario as a permanent regression.

## Debugging Failures

Start with `--verbose`; the harness prints each accepted command as it runs.
If a scenario fails after a production crash, inspect the postmortem capsule
beside the scenario and reduce the command list until the failure is minimal.
For parser failures, the `chaos:LINE:` prefix points to the offending line.

Before committing a scenario, run:

```bash
ZCL_TEST_ONLY=chaos_harness ./test_zcl
make chaos
```
