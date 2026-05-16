# zclassic23 — MVP target

**MVP = "someone we don't know can run zclassic23 + use it for a week
without intervention."** Eight binary acceptance criteria, each with
a CI test. The MVP Readiness Score (MRS) is the count of passing
criteria; MVP is achieved at 8/8.

## Acceptance criteria

| # | Criterion | How we verify | Status |
|---|---|---|---|
| 1 | **Single-binary install on clean Ubuntu/Debian** | CI: clean container, `make install && systemctl --user start zclassic23`, exit 0 | ☐ |
| 2 | **Tor onion bootstrap in <60s** | `zcl_onion_status` returns `bootstrap_state=ready` within 60s of start — test: `lib/test/src/test_onion_bootstrap.c` (`ZCL_STRESS_TESTS=1`) | ✅ |
| 3 | **Cold-start sync to tip in <10 min** | Fresh datadir → `zcl_syncstate.phase=ready` within 10 min on 100 Mbps link — test: `lib/test/src/test_cold_start_sync.c` (`ZCL_STRESS_TESTS=1`) | ✅ |
| 4 | **Receive shielded payment end-to-end** | Test wallet receives 1 ZCL to a z-addr, balance reflects within 2 blocks | ☐ |
| 5 | **List + sell file via store** | Operator lists product → buyer pays shielded → buyer receives file | ☐ |
| 6 | **7-day soak with zero operator intervention** | Live node + synthetic load for 168h: no manual restarts, RSS plateau | ☐ |
| 7 | **Recover from `kill -9` in <2 min** | Chaos test: kill -9 mid-block, restart, caught up to peer-tip within 2 min — test: `lib/test/src/test_kill9_recovery.c` (`ZCL_STRESS_TESTS=1`) | ✅ |
| 8 | **Consensus parity with zclassicd** | Continuous diff service: 0 mismatches over the 7-day soak window | ☐ |

**Estimated MRS today: 3 / 8** (criteria 1, 2, 4 likely pass on
manual test; 3 partial; 5/6/7/8 fail or untested). **CI-verified
MRS: 3 / 8** — criteria #2 (`test_onion_bootstrap.c`), #3
(`test_cold_start_sync.c`), and #7 (`test_kill9_recovery.c`) are
green CI gates; the rest remain estimate-only.

**Update rule:** when a CI test for a criterion goes green, flip ☐
to ✅ in this file.

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
