# ZClassic23 — The Vision (north star)

> The destination. Read this first; it's what every refactor decision serves.
> Execution map + live progress: [`REFACTOR_STATUS.md`](./REFACTOR_STATUS.md).
> Shapes + rules: [`FRAMEWORK.md`](./FRAMEWORK.md).

## What we are building

**One statically-linked C23 binary that is a complete, private, self-hosting
computing surface** — a full shielded ZClassic node, block explorer, wallet, P2P
file market, on-chain name registry (ZNAM), encrypted messaging (ZMSG),
cross-chain atomic swaps (ZSWP), and a P2P game framework — served entirely over
its **own embedded Tor onion**, and **operated by an AI as a first-class
operator** through 100+ typed MCP tools. No servers, no DNS dependence, no cloud,
no accounts. You run the binary; you own the surface.

## The one architectural principle

**It cannot halt silently.** That single property is what makes this a modern
architecture and not just a working pile of features:

- **One canonical path.** Chain progress is a durable **stage cursor on disk**;
  every step either **advances the cursor or names a typed blocker**. There is no
  shadow/legacy duality left to drift apart.
- **Supervised actors.** Every long-lived task lives under a liveness tree with a
  deadman — it advances a progress marker or the supervisor fires a stall.
- **Self-heal or escalate loudly.** Every failure is either repaired by a
  Condition or raised through `EV_OPERATOR_NEEDED` → alert sinks + `zcl_status`
  DEGRADED + sd_notify. A halt can never page nobody.
- **Live truth, not green tests.** Forward progress at the tip is the gate. A
  remedy that returns `ok` must resolve the symptom.

## The promises (and the irreducible cost)

- ⚡ **Fast** — cold-sync to tip in ~30s (FlyClient + MMB proofs + SHA3 UTXO
  snapshot over an append-only event log); hardware crypto (SHA-NI/SSE4.2/io_uring).
- 🪶 **Lean** — slim binary; RSS → 1 GB; profile before optimizing.
- 💪 **Unbreakable** — unhaltable by construction; auto-recovery; alerts reach a human.
- 🔬 **Honest** — deterministic (every bug → a replayable seed tape); reproducible
  bug→fix (postmortem capsule + `make chaos`); live truth from the service.
- 🧩 **Shaped** — 8 code shapes in 8 folders (hexagonal ports/adapters + Rails-MVC
  + Phoenix supervisors + the Condition shape); crypto-agile behind a registry.

**Irreducible:** ~21k lines of zk-SNARK crypto (Groth16/BN254/BLS12-381/PHGR13)
are the price of shielded transactions. A full shielded node is inherently large;
the goal is *no cruft*, not *small*. Verified removable weight is ~17.5k of 388k
(~4.5%), dominated by the two-path duplication that the cutover deletes.

## The dependency spine — why the cutover is everything

The product works and is AI-operable today. What turns it into the *unhaltable*
architecture above is **one move: the Wave-S cutover** (flip the shadow stages
authoritative, C-3→C-9). It pays off four ways at once:

```
  unhalt ✅ → safe-flip guard → C-3 → C-5 → C-6 → C-7 → C-8 → C-9
                                                       │       │
                                  ┌────────────────────┘       │
                                  └► dissolve utxo_recovery     │
                                     dissolve chain_advance + legacy_mirror ◄┘
```

1. the live chain becomes **unhaltable by construction**,
2. **~12.5k LOC** of legacy two-path mega-modules deletes itself (dead by construction),
3. the cold-sync path becomes the **live** path, and
4. Phase 4e (bodies-in-log) and Phase 8 (self-bounding log) unlock.

**Flip it carefully, behind the safe-flip guard, on a healthy node — that is the
finish line for the architecture.**

## Working doctrine (binding)

- **Subtraction first.** Delete and dedup before adding. Less is more.
- **Verify before deleting.** Per-symbol caller check; audits overclaim — confirm
  dead, then remove. (Both prior cruft audits had false positives.)
- **Stay coordinated.** Workers push to origin/main continuously; fetch→rebase
  before committing, push promptly, don't diverge.
- **Canonical functionality in C, in the binary** — MCP/RPC/subcommands, not shell
  that greps logs.
