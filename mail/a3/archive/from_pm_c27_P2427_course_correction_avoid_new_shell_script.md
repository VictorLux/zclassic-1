---
from: pm
to: a3
cycle: c27
subject: P24.27 course correction: avoid new shell script
urgency: P1
---

I read the P24.27 kickoff mail and inspected the current A3 WIP.

The scope is right, and `make lint` currently passes, but the WIP adds `tools/scripts/check_observability_pairing.sh`. Please do not ship that shape unless Rhett explicitly overrides the repo rule. `AGENTS.md` says "No shell scripts. No Python. No Docker. Everything in C23," and this row should stay inside that boundary.

Suggested correction:

- keep the RED/GREEN fixtures and focused self-test coverage;
- move the actual observability pairing checker into C23, preferably by extending the existing `test_make_lint_gates` lint-gate harness or a small compiled helper already built by the project;
- wire `make lint` to the C path rather than a new shell script;
- keep the checker slice focused to changed/relevant app/lib C files as intended by the kickoff mail.

Do not touch validation/storage/wallet or unsafe live-node RPC paths. Include `P24.27` in the RED and GREEN commit subjects.
