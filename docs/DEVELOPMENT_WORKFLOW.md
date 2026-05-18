# Development Workflow — never restart `zclassic23.service`

The main `zclassic23.service` is the public-facing node serving zclnet.net.
Every restart costs peer reputation: remote MagicBean peers see a brief
disconnect, increment their flaky-peer score, and back off reconnect
attempts for 1-6 hours. After ~15 restarts in a day the node spends most
of its time waiting for cooldowns to expire.

**Rule: never restart `zclassic23` during development.** Use the test
instance.

## The test instance

`zclassic23-test.service` is already provisioned with separate ports +
datadir + flags:

| Setting | Main | Test |
|---|---|---|
| Unit | `zclassic23.service` | `zclassic23-test.service` |
| P2P port | 8033 | 8035 |
| RPC port | 18232 | 18234 |
| Datadir | `~/.zclassic-c23` | `~/.zclassic-c23-test` |
| External IP | `-externalip=74.50.74.102` | none (local only) |
| Tor onion | published | local-only |
| Background validation | on | off (`-nobgvalidation`) |
| Peering | full network + zclassicd | `-addnode=127.0.0.1:8033` only |

The test instance does NOT advertise to the public network. It connects
only to the main node (and through it sees the rest of the chain). Other
peers don't see the test node — restarting it has zero impact on the main
node's peer reputation.

## Daily dev loop

```bash
# 1. Edit code, save.

# 2. Build (overwrites ~/zclassic23/zclassic23 on disk; running main
#    process keeps the old image until restart).
make -j$(nproc) lint

# 3. Restart ONLY the test instance.
systemctl --user restart zclassic23-test

# 4. Verify against test instance:
#    - RPC: zcl-rpc -rpcport=18234 -datadir=$HOME/.zclassic-c23-test <cmd>
#    - Onion: query the test onion (zcl_onion_status on the test RPC)
#    - Logs:  journalctl --user -u zclassic23-test -f

# 5. Once confident, DEPLOY to main:
systemctl --user restart zclassic23
#    Expect ~3-5 min of WAL replay + block_index load on big datadirs.
#    Then leave it alone for at least 24 h to recover peer reputation.
```

## Page-rendering changes — no restart at all

Static templates (`app/views/templates/*.chtml`) and the explorer CSS
(`app/views/css/style.css`) are reloaded from disk on every request —
see `serve_css()` in `app/controllers/src/explorer_controller_pages.c`.
For HTML emitted by C controllers (factoids, hodl, stats, etc.) you DO
need a rebuild + restart of the test instance. Use the test instance.

## When you must restart the main node

- Critical security or consensus fix.
- A correctness fix the public site needs immediately.
- After fixing a runtime bug actually crashing the node.

In all other cases, the test instance is the right path. Tonight's
peer-reputation backlog took 15 restarts to accumulate; it'll take
12-48 hours to fully recover via addrman gossip refresh.

## Stop-timeout budget

`TimeoutStopSec=300` (5 minutes). The previous 90s ceiling was breached
under shutdown load (WAL fsync + Tor teardown + thread joins) — every
overage became a `SIGKILL` and remote peers logged a flaky disconnect.
With 300s headroom, normal stops complete cleanly; a stop that genuinely
hangs past 300s warrants a debugger attach before the next bump.

## Multiple-test instances?

`zclassic23-test.service` can be cloned for parallel test instances —
distinct ports + datadirs. Convention: `zclassic23-testN.service` with
ports `8033+N`, `18232+N`. Each runs the same `~/zclassic23/zclassic23`
binary; the OS handles concurrent reads of the file.

## Anti-patterns

- ❌ Editing code → `make deploy` → testing on main.
- ❌ `systemctl restart zclassic23` to "see if the change worked".
- ❌ Running performance tests against main.
- ❌ Manual schema migrations on main without first running them on a
     copy of the datadir.
