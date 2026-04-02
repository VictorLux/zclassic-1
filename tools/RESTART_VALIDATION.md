# Restart Validation

Use `zcl-nodectl verify-follow` for the operational question that matters:

- can `zclassic23` restart
- catch up to legacy `zclassicd`
- reach the same tip
- stay healthy there for repeated samples

## What It Checks

The tool:

1. reads legacy `zclassicd` tip from RPC
2. optionally restarts `zclassic23`
3. waits for `zclassic23` RPC to come back
4. polls:
   - `getblockchaininfo`
   - `syncstate`
   - `healthcheck`
5. requires all of the following for several consecutive samples:
   - `blocks >= legacy_tip`
   - `headers >= legacy_tip`
   - `syncstate == at_tip`
   - `healthcheck.healthy == true`

That is intentionally stricter than “process is running”.

## Usage

```bash
make zcl-nodectl
./zcl-nodectl verify-follow
./zcl-nodectl verify-follow --restart
./zcl-nodectl verify-follow --restart --timeout 1800 --poll 15 --stable 3
```

Credentials can come from:

- `~/.zclassic/.cookie`
- `~/.zclassic/zclassic.conf`
- `~/.zclassic-c23/.cookie`
- `~/.zclassic-c23/zclassic.conf`

## Why This Exists

The refactor work tightened:

- persisted tip authority
- `SYNC_AT_TIP` transitions
- health degradation when headers are ahead of the active chain

This tool is the next layer up: an operator-facing proof that restart behavior
matches the actual production expectation instead of just compiling and passing
unit tests.
