# zclassic23 — MVP target

**MVP = "someone we don't know can run zclassic23 + use it for a week
without intervention."** Eight binary acceptance criteria, each with
a CI test. The MVP Readiness Score (MRS) is the count of passing
criteria; MVP is achieved at 8/8.

## Acceptance criteria

| # | Criterion | How we verify | Status |
|---|---|---|---|
| 1 | **Single-binary install on clean Ubuntu/Debian** | CI: clean container, `make install && systemctl --user start zclassic23`, exit 0 | ☐ |
| 2 | **Tor onion bootstrap in <60s** | `zcl_onion_status` returns `bootstrap_state=ready` within 60s of start — test: `lib/test/src/test_onion_bootstrap.c` (`ZCL_STRESS_TESTS=1`) | ☐ |
| 3 | **Cold-start sync to tip in <10 min** | Fresh datadir → `zcl_syncstate.phase=ready` within 10 min on 100 Mbps link — test: `lib/test/src/test_cold_start_sync.c` (`ZCL_STRESS_TESTS=1`) | ☐ |
| 4 | **Receive shielded payment end-to-end** | Test wallet receives 1 ZCL to a z-addr, balance reflects within 2 blocks | ☐ |
| 5 | **List + sell file via store** | Operator lists product → buyer pays shielded → buyer receives file | ☐ |
| 6 | **7-day soak with zero operator intervention** | Live node + synthetic load for 168h: no manual restarts, RSS plateau | ☐ |
| 7 | **Recover from `kill -9` in <2 min** | Chaos test: kill -9 mid-block, restart, caught up to peer-tip within 2 min — test: `lib/test/src/test_kill9_recovery.c` (`ZCL_STRESS_TESTS=1`) | ☐ |
| 8 | **Consensus parity with zclassicd** | Continuous diff service: 0 mismatches over the 7-day soak window — **no such service exists yet (net-new build)** | ☐ |

**Actually met (manual only): ~2 / 8** — #1 (single-binary install)
and #7 (kill-9 recovery) are demonstrated by hand. #2's onion is live
but the <60s timing isn't measured; #3/#4/#5 are partial (acceptance
gates exist but are opt-in, env/param-dependent, not run); #6 is
**regressing** (no soak — the node is wedged at tip); #8 is unmet (no
parity service exists). **CI-verified MRS: 0 / 8.** NONE of the
acceptance tests run under `make ci`: #2/#3/#4/#5/#7 all gate on
`ZCL_STRESS_TESTS=1`, which `make ci` (`Makefile:1110`) and
`.github/workflows/ci.yml` never set, and #1/#6/#8 have no CI test at
all. The ✅ marks previously on #2/#3/#7 were wrong — a test that SKIPs
in CI is not a green gate.

**Update rule:** flip ☐ → ✅ ONLY when a criterion's acceptance test
runs and passes in CI (not opt-in). To make #2/#3/#4/#5/#7 real gates,
set `ZCL_STRESS_TESTS=1` for them in `make ci`; #1 and #8 need net-new
CI jobs.

**THE plan to drive MRS to 8/8 is [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md).**

**MVP achieved when:** MRS = 8/8.

## Why these and not others

- **Operator UX over feature breadth.** ZNAM, ZMSG, Market, Swaps,
  P2P games are great features, but they don't define MVP. MVP is
  "the chain works, payments work, the operator can leave it
  alone." Differentiated features come after MVP.
- **Decentralized commerce as the headline value.** The store flow
  (criterion 5) is the wedge that makes zclassic23 different from
  any other Zcash node — pay via shielded, receive via .onion.
- **Soak time as the hardest gate.** Criterion 6 (7-day uninterrupted
  operation) is what proves we're past firefighting. Today: ~3h
  between operator restarts. MVP requires a 56× improvement.
