/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Activation Controller — single authority for block connection.
 *
 * Problem: activate_best_chain() was called from 5 places across 3 threads
 * with zero coordination. This controller is the SINGLE entry point.
 *
 * Architecture:
 *   State machine: IDLE → BOOT_PENDING → ANCHOR_ACTIVE → READY → CONNECTING → AT_TIP
 *   Planning pattern: activation_should_connect() is pure, no side effects
 *   Execution: activation_request_connect() serializes through mutex
 *   Transition table: validates every state change, rejects illegal ones
 *
 * Key invariant: while state is ANCHOR_ACTIVE, activate_best_chain NEVER runs.
 * No exceptions, no bypasses, no scattered boolean overrides. */

#ifndef ZCL_CHAIN_ACTIVATION_CONTROLLER_H
#define ZCL_CHAIN_ACTIVATION_CONTROLLER_H

#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

struct main_state;
struct coins_view_cache;
struct chain_params;
struct block;
struct validation_state;

/* ── State Machine ─────────────────────────────────────────────── */

enum activation_state {
    ACTIVATION_IDLE = 0,          /* not initialized */
    ACTIVATION_BOOT_PENDING,      /* boot in progress, connection forbidden */
    ACTIVATION_ANCHOR_ACTIVE,     /* snapshot/LDB anchor, connection forbidden */
    ACTIVATION_ANCHOR_CLEARING,   /* headers past anchor, transitioning */
    ACTIVATION_READY,             /* safe to connect blocks */
    ACTIVATION_CONNECTING,        /* activate_best_chain running (mutex held) */
    ACTIVATION_AT_TIP,            /* caught up, waiting for new blocks */
    ACTIVATION_FAILED,            /* unrecoverable */
    ACTIVATION_NUM_STATES
};

const char *activation_state_name(enum activation_state state);
bool activation_transition_valid(enum activation_state from, enum activation_state to);

/* ── Controller ────────────────────────────────────────────────── */

struct chain_activation_controller {
    _Atomic int state;
    zcl_mutex_t mutex;          /* serializes activate_best_chain execution */

    /* Context (set once at init, read-only after) */
    struct main_state *ms;
    struct coins_view_cache *coins_tip;
    const struct chain_params *params;
    const char *datadir;

    /* Diagnostics */
    int64_t last_activation_us;
    int     last_tip_height;
    int     activation_count;
    int     skip_count;

    /* deferred-activation counter. A request that hits
     * SKIP_ALREADY_RUNNING (another thread is inside activate_best_chain
     * under the mutex) increments this atomically instead of dropping
     * the work. The thread currently holding the mutex drains it before
     * releasing so the newly-arrived-but-skipped block gets connected
     * without waiting for the next P2P arrival. */
    _Atomic int deferred_pending;
};

void activation_controller_init(struct chain_activation_controller *ctl,
                                struct main_state *ms,
                                struct coins_view_cache *coins_tip,
                                const struct chain_params *params,
                                const char *datadir);
void activation_controller_destroy(struct chain_activation_controller *ctl);

enum activation_state activation_get_state(
    const struct chain_activation_controller *ctl);
bool activation_set_state(struct chain_activation_controller *ctl,
                          enum activation_state new_state,
                          const char *reason);

/* Convenience transitions */
void activation_set_anchor_active(struct chain_activation_controller *ctl,
                                  const char *reason);
void activation_clear_anchor(struct chain_activation_controller *ctl,
                             const char *reason);
void activation_boot_complete(struct chain_activation_controller *ctl,
                              const char *reason);

/* ── Planning (pure, no side effects) ──────────────────────────── */

enum activation_request_source {
    ACTIVATION_SRC_BOOT = 0,
    ACTIVATION_SRC_UTXO_REPLAY,
    ACTIVATION_SRC_BLOCK_FILE_SCAN,
    ACTIVATION_SRC_HEADERS_ALL_DATA,
    ACTIVATION_SRC_NEW_BLOCK,
};

struct activation_request {
    enum activation_request_source source;
    enum activation_state current_state;
    bool shutdown_requested;
    bool anchor_active;
    bool awaiting_utxos;
    int  chain_tip_height;
};

enum activation_decision_result {
    ACTIVATION_DO_CONNECT = 0,
    ACTIVATION_SKIP_SHUTDOWN,
    ACTIVATION_SKIP_ANCHOR_BLOCKS,
    ACTIVATION_SKIP_AWAITING_UTXOS,
    ACTIVATION_SKIP_ALREADY_RUNNING,
    ACTIVATION_SKIP_WRONG_STATE,
    ACTIVATION_SKIP_AT_TIP,
};

struct activation_decision {
    enum activation_decision_result result;
    bool should_activate;
    char reason[128];
};

/* Pure function: ALL callers use this. No globals, no side effects. */
void activation_should_connect(struct activation_decision *out,
                               const struct activation_request *req);

/* ── Execution (serialized) ────────────────────────────────────── */

enum activation_exec_result {
    ACTIVATION_EXEC_OK = 0,
    ACTIVATION_EXEC_SKIPPED,
    ACTIVATION_EXEC_FAILED,
};

struct activation_exec_outcome {
    enum activation_exec_result result;
    int  new_tip_height;
    bool reached_tip;
    char reason[128];
};

/* Single entry point replacing all direct activate_best_chain calls.
 * Thread-safe: mutex ensures only one execution at a time. */
void activation_request_connect(struct chain_activation_controller *ctl,
                                enum activation_request_source source,
                                struct block *pblock,
                                struct activation_exec_outcome *out);

/* atomically read-and-reset the deferred-activation counter.
 * Returns the number of SKIP_ALREADY_RUNNING requests that arrived
 * while another thread held the activation mutex, since the last
 * drain. Used by the activator (under mutex) to decide whether to
 * rerun activate_best_chain before transitioning out of CONNECTING.
 * Also exposed for diagnostics and tests. */
int activation_drain_deferred(struct chain_activation_controller *ctl);

/* ── UTXO Wipe Protection ──────────────────────────────────────── */

struct utxo_wipe_decision {
    bool safe_to_wipe;
    char reason[128];
};

/* Pure function: decides if UTXO wipe is safe.
 * NEVER wipes while ANCHOR_ACTIVE or ANCHOR_CLEARING. */
void activation_should_allow_utxo_wipe(struct utxo_wipe_decision *out,
                                       enum activation_state state,
                                       bool anchor_active);

/* ── Global accessor ───────────────────────────────────────────── */

struct chain_activation_controller *boot_activation_controller(void);

#endif
