# lib/framework/

Framework primitives used by the eight `app/` shapes.

This directory is intentionally small. It contains C runtime primitives that are
already real, not a placeholder DSL:

| File | Purpose |
|---|---|
| `include/framework/condition.h` + `src/condition.c` | Condition registry and engine: `{detect, remedy, witness}` structs with polling, backoff, witnessed recovery, and operator escalation. |
| `include/framework/mailbox.h` | Re-export of the typed mailbox primitive used by supervised actors. |
| `include/framework/projection.h` | Re-export of the projection primitive used by rebuildable read models. |

Do not add macro-only scaffold here. The canonical form is a plain struct plus
registration function that can be grepped, stepped through, and linted. See
[`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) for the architecture and
[`docs/REFACTOR_STATUS.md`](../../docs/REFACTOR_STATUS.md) for active debt.
