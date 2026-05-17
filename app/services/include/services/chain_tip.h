/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chain_tip — canonical owner of "set the active chain tip" semantics.
 *
 * Before this module, ~30 call sites across snapshot_sync_service,
 * chain_restore_service, process_block, msg_headers, utxo_recovery,
 * and boot.c invoked `active_chain_set_tip(&ms->chain_active, new_bi)`
 * directly. Each site decided independently whether to:
 *   - emit EV_TIP_UPDATED / EV_CHAIN_TIP_COMMIT events
 *   - call csr_apply_commit for SQLite persistence
 *   - update pindex_best_header
 *   - log a "[tip]" line for observability
 *
 * The result: events fire from some paths but not others, the log is
 * inconsistent, and adding a new invariant (e.g. an integrity assert)
 * means editing 30 sites — a recipe for drift.
 *
 * This module exposes a single function that all of those sites
 * SHOULD call. The bare `active_chain_set_tip` survives as a low-level
 * primitive used by this module itself; new callers must use
 * `chain_set_active_tip`. */

#ifndef ZCL_CHAIN_TIP_H
#define ZCL_CHAIN_TIP_H

#include <stdbool.h>

struct main_state;
struct block_index;

/* Where the tip change originated. Used in the structured `[tip]`
 * log line so operators can see at a glance which subsystem moved
 * the chain. */
enum tip_source {
    TIP_FROM_UNSPECIFIED  = 0,
    TIP_FROM_CONNECT,      /* connect_tip in activate_best_chain */
    TIP_FROM_DISCONNECT,   /* disconnect_tip / reorg rollback */
    TIP_FROM_SNAPSHOT,     /* FlyClient+SHA3 snapshot installation */
    TIP_FROM_RESTORE,      /* boot-time chain-restore */
    TIP_FROM_BOOT_REPAIR,  /* boot.c misc fixups */
    TIP_FROM_P2P_REPAIR,   /* msg_headers post-activation repair */
    TIP_FROM_UTXO_REPAIR,  /* utxo_recovery_service */
    TIP_FROM_TEST,         /* unit/integration tests */
};

const char *tip_source_name(enum tip_source src);

/* Set the active chain tip. Wraps the low-level `active_chain_set_tip`
 * and adds:
 *   - structured `[tip] h=H hash=hex16... src=... reason=...` log line
 *   - EV_TIP_UPDATED event with hash + height payload
 *   - EV_CHAIN_TIP_COMMIT event with from/to/reason payload
 *
 * Returns true on success, false if `active_chain_set_tip` returns
 * false (typically realloc OOM at very high heights).
 *
 * `new_tip` may be NULL to clear the tip (returns true; emits a
 * "[tip] CLEARED" log line). `reason` may be NULL.
 *
 * Does NOT take any locks — the caller is responsible for serializing
 * with other chain mutations (typically `ms->cs_main` or the
 * activation_controller mutex). */
bool chain_set_active_tip(struct main_state *ms,
                          struct block_index *new_tip,
                          enum tip_source src,
                          const char *reason);

#endif /* ZCL_CHAIN_TIP_H */
