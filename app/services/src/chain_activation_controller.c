/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Activation Controller — single authority for block connection.
 * See chain_activation_controller.h for architecture overview. */

#include "services/chain_activation_controller.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "event/event.h"
#include "services/snapshot_sync_service.h"
#include "core/utiltime.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "util/log_macros.h"
#include "util/blocker.h"

/* The single typed blocker this authority owns. When the active tip is
 * below the most-work *valid-header* chain and this tick could not advance,
 * we MUST name the blocker (height + why + escape) instead of returning to a
 * quiet READY. Going READY/AT_TIP is only legal when genuinely caught up;
 * that transition clears this blocker. The escape action is "drive a fresh
 * connect pass over have-data successors" — the always-on local authority,
 * not a P2P quorum a personal stack can't form.
 *
 * See FRAMEWORK.md Prime Directive + REFACTOR_STATUS.md FOCUS NOW: the
 * reducer must advance-the-tip OR name-a-typed-blocker every tick.
 * ACTIVATION_BEHIND_BLOCKER_ID is defined in the header (shared with tests). */

/* Name the most-precise reason this tick could not advance. */
static const char *
activation_behind_reason(void)
{
    /* Successors are on disk with BLOCK_HAVE_DATA but find_most_work_chain
     * could not connect them this tick (the legacy_body_pull "skipped_have"
     * + activate "behind_peers" deadlock); OR bodies are still missing at
     * H+1. process_block tells us which. */
    if (process_block_active_tip_has_pending())
        return "successor-have-data-but-not-activated";
    return "body-missing-at-successor";
}

/* Register/refresh the typed blocker naming why the tip is behind and what
 * the escape is. TRANSIENT: the local authority should be able to drive the
 * connect; the supervisor escape re-triggers a connect pass on deadline. */
static void
activation_set_behind_blocker(int tip_h, int best_h)
{
    struct blocker_record rec;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "%s: tip=%d best_valid_header=%d gap=%d — drive activation of "
             "have-data successors from the local authority",
             activation_behind_reason(), tip_h, best_h,
             best_h - tip_h);
    if (!blocker_init(&rec, ACTIVATION_BEHIND_BLOCKER_ID,
                      "chain_activation", BLOCKER_TRANSIENT, reason)) {
        LOG_WARN("activation", "blocker_init overflow id=%s",
                 ACTIVATION_BEHIND_BLOCKER_ID);
        return;
    }
    rec.escape_deadline_secs = 120;
    rec.retry_budget = -1; /* keep retrying — the gap is the witness */
    snprintf(rec.escape_action, sizeof(rec.escape_action),
             "activation_drive_connect");
    int rc = blocker_set(&rec);
    if (rc == 0)
        event_emitf(EV_ACTIVATION_STATE_CHANGE, 0,
                    "BLOCKED behind header chain: %s", reason);
}

/* The single source of truth for the advance-or-block decision after an
 * activate pass. Returns true when the tip is BEHIND the most-work
 * valid-header chain (could not advance to tip this tick) → registers the
 * typed blocker. Returns false when genuinely caught up → clears it. This is
 * the structural invariant: READY is honest only when caught up; otherwise a
 * named blocker exists. Exposed (non-static) so the test can drive the exact
 * production decision against the live blocker registry. */
bool activation_eval_tip_blocker(int tip_h, int best_h)
{
    /* best_h == 0 means we have no header chain yet (fresh boot) — not a
     * meaningful "behind" yet; treat as caught up and clear. */
    if (best_h > 0 && tip_h + 100 < best_h) {
        activation_set_behind_blocker(tip_h, best_h);
        return true;
    }
    blocker_clear(ACTIVATION_BEHIND_BLOCKER_ID);
    return false;
}

/* ── State names ───────────────────────────────────────────────── */

static const char *g_activation_state_names[] = {
    [ACTIVATION_IDLE]             = "idle",
    [ACTIVATION_BOOT_PENDING]     = "boot_pending",
    [ACTIVATION_ANCHOR_ACTIVE]    = "anchor_active",
    [ACTIVATION_ANCHOR_CLEARING]  = "anchor_clearing",
    [ACTIVATION_READY]            = "ready",
    [ACTIVATION_CONNECTING]       = "connecting",
    [ACTIVATION_AT_TIP]           = "at_tip",
    [ACTIVATION_FAILED]           = "failed",
};

const char *activation_state_name(enum activation_state state)
{
    if (state < 0 || state >= ACTIVATION_NUM_STATES) return "unknown";
    return g_activation_state_names[state];
}

/* ── Transition table ──────────────────────────────────────────── */

static const bool g_activation_transitions[ACTIVATION_NUM_STATES][ACTIVATION_NUM_STATES] = {
    [ACTIVATION_IDLE][ACTIVATION_BOOT_PENDING]             = true,
    [ACTIVATION_IDLE][ACTIVATION_FAILED]                   = true,

    [ACTIVATION_BOOT_PENDING][ACTIVATION_ANCHOR_ACTIVE]    = true,
    [ACTIVATION_BOOT_PENDING][ACTIVATION_READY]            = true,
    [ACTIVATION_BOOT_PENDING][ACTIVATION_FAILED]           = true,

    [ACTIVATION_ANCHOR_ACTIVE][ACTIVATION_ANCHOR_CLEARING] = true,
    [ACTIVATION_ANCHOR_ACTIVE][ACTIVATION_FAILED]          = true,

    [ACTIVATION_ANCHOR_CLEARING][ACTIVATION_READY]         = true,
    [ACTIVATION_ANCHOR_CLEARING][ACTIVATION_FAILED]        = true,

    [ACTIVATION_READY][ACTIVATION_CONNECTING]              = true,
    [ACTIVATION_READY][ACTIVATION_FAILED]                  = true,

    [ACTIVATION_CONNECTING][ACTIVATION_AT_TIP]             = true,
    [ACTIVATION_CONNECTING][ACTIVATION_READY]              = true,
    [ACTIVATION_CONNECTING][ACTIVATION_FAILED]             = true,

    [ACTIVATION_AT_TIP][ACTIVATION_CONNECTING]             = true,
    [ACTIVATION_AT_TIP][ACTIVATION_READY]                  = true,

    [ACTIVATION_FAILED][ACTIVATION_IDLE]                   = true,
};

bool activation_transition_valid(enum activation_state from,
                                 enum activation_state to)
{
    if (from < 0 || from >= ACTIVATION_NUM_STATES) return false;
    if (to < 0 || to >= ACTIVATION_NUM_STATES) return false;
    return g_activation_transitions[from][to];
}

/* Escape action for ACTIVATION_BEHIND_BLOCKER_ID. The supervisor blocker
 * sweep fires this on deadline edge: drive a fresh connect pass over the
 * have-data successors from the always-on local authority. Idempotent — a
 * connect with no new work is a no-op; this is NOT a whack-a-mole "claim
 * success" remedy: the blocker is only cleared by the AT_TIP transition
 * above when the tip genuinely catches up (the witness is the gap closing). */
static void activation_drive_connect_escape(const struct blocker_snapshot *snap)
{
    (void)snap;
    struct chain_activation_controller *ctl = boot_activation_controller();
    if (!ctl) {
        LOG_WARN("activation", "escape no controller id=%s",
                 ACTIVATION_BEHIND_BLOCKER_ID);
        return;
    }
    struct activation_exec_outcome ao;
    activation_request_connect(ctl, ACTIVATION_SRC_HEADERS_ALL_DATA, NULL, &ao);
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

void activation_controller_init(struct chain_activation_controller *ctl,
                                struct main_state *ms,
                                struct coins_view_cache *coins_tip,
                                const struct chain_params *params,
                                const char *datadir)
{
    memset(ctl, 0, sizeof(*ctl));
    atomic_store(&ctl->state, ACTIVATION_IDLE);
    atomic_store(&ctl->deferred_pending, 0);
    zcl_mutex_init(&ctl->mutex);
    ctl->ms = ms;
    ctl->coins_tip = coins_tip;
    ctl->params = params;
    ctl->datadir = datadir;

    /* Wire the escape for the typed behind-blocker so the supervisor sweep
     * can re-drive activation on deadline. Idempotent across re-init. */
    blocker_register_escape("activation_drive_connect",
                            activation_drive_connect_escape);
}

int activation_drain_deferred(struct chain_activation_controller *ctl)
{
    if (!ctl) return 0;
    return atomic_exchange(&ctl->deferred_pending, 0);
}

void activation_controller_destroy(struct chain_activation_controller *ctl)
{
    if (!ctl) return;
    zcl_mutex_destroy(&ctl->mutex);
}

/* ── State machine ─────────────────────────────────────────────── */

enum activation_state activation_get_state(
    const struct chain_activation_controller *ctl)
{
    return (enum activation_state)atomic_load(&ctl->state);
}

bool activation_set_state(struct chain_activation_controller *ctl,
                          enum activation_state new_state,
                          const char *reason)
{
    enum activation_state old =
        (enum activation_state)atomic_load(&ctl->state);

    if (old == new_state)
        return true;

    if (!activation_transition_valid(old, new_state)) {
        LOG_WARN("chain", "activation ILLEGAL transition %s->%s (%s)", activation_state_name(old), activation_state_name(new_state), reason ? reason : "");
        event_emitf(EV_ACTIVATION_STATE_CHANGE, 0,
                    "ILLEGAL %s->%s: %s",
                    activation_state_name(old),
                    activation_state_name(new_state),
                    reason ? reason : "");
        return false;
    }

    atomic_store(&ctl->state, (int)new_state);
    printf("activation: %s->%s (%s)\n",
           activation_state_name(old),
           activation_state_name(new_state),
           reason ? reason : "");
    event_emitf(EV_ACTIVATION_STATE_CHANGE, 0,
                "%s->%s: %s",
                activation_state_name(old),
                activation_state_name(new_state),
                reason ? reason : "");
    return true;
}

void activation_set_anchor_active(struct chain_activation_controller *ctl,
                                  const char *reason)
{
    activation_set_state(ctl, ACTIVATION_ANCHOR_ACTIVE, reason);
}

void activation_clear_anchor(struct chain_activation_controller *ctl,
                             const char *reason)
{
    if (activation_get_state(ctl) != ACTIVATION_ANCHOR_ACTIVE)
        return;
    activation_set_state(ctl, ACTIVATION_ANCHOR_CLEARING, reason);
    activation_set_state(ctl, ACTIVATION_READY, "anchor_cleared");
}

void activation_boot_complete(struct chain_activation_controller *ctl,
                              const char *reason)
{
    enum activation_state cur = activation_get_state(ctl);
    if (cur == ACTIVATION_BOOT_PENDING)
        activation_set_state(ctl, ACTIVATION_READY, reason);
}

/* ── Planning (pure) ───────────────────────────────────────────── */

void activation_should_connect(struct activation_decision *out,
                               const struct activation_request *req)
{
    memset(out, 0, sizeof(*out));

    if (req->shutdown_requested) {
        out->result = ACTIVATION_SKIP_SHUTDOWN;
        snprintf(out->reason, sizeof(out->reason), "shutdown requested");
        return;
    }

    if (req->current_state == ACTIVATION_ANCHOR_ACTIVE ||
        req->anchor_active) {
        out->result = ACTIVATION_SKIP_ANCHOR_BLOCKS;
        snprintf(out->reason, sizeof(out->reason),
                 "anchor active — block connection forbidden");
        return;
    }

    if (req->current_state == ACTIVATION_ANCHOR_CLEARING) {
        out->result = ACTIVATION_SKIP_ANCHOR_BLOCKS;
        snprintf(out->reason, sizeof(out->reason),
                 "anchor clearing in progress");
        return;
    }

    if (req->awaiting_utxos) {
        out->result = ACTIVATION_SKIP_AWAITING_UTXOS;
        snprintf(out->reason, sizeof(out->reason),
                 "awaiting UTXO set from P2P");
        return;
    }

    if (req->current_state == ACTIVATION_CONNECTING) {
        out->result = ACTIVATION_SKIP_ALREADY_RUNNING;
        snprintf(out->reason, sizeof(out->reason),
                 "activate_best_chain already running");
        return;
    }

    if (req->current_state != ACTIVATION_READY &&
        req->current_state != ACTIVATION_AT_TIP) {
        out->result = ACTIVATION_SKIP_WRONG_STATE;
        snprintf(out->reason, sizeof(out->reason),
                 "state=%s, need ready or at_tip",
                 activation_state_name(req->current_state));
        return;
    }

    out->result = ACTIVATION_DO_CONNECT;
    out->should_activate = true;
    snprintf(out->reason, sizeof(out->reason), "approved (tip=%d)",
             req->chain_tip_height);
}

/* ── Execution ─────────────────────────────────────────────────── */

void activation_request_connect(struct chain_activation_controller *ctl,
                                enum activation_request_source source,
                                struct block *pblock,
                                struct activation_exec_outcome *out)
{
    memset(out, 0, sizeof(*out));

    /* Build request from current state */
    struct activation_request req = {
        .source = source,
        .current_state = activation_get_state(ctl),
        .shutdown_requested = false, /* caller can check externally */
        .anchor_active = (snapsync_get_anchor() != NULL),
        .awaiting_utxos = snapsync_awaiting_utxos(),
        .chain_tip_height = active_chain_height(&ctl->ms->chain_active),
    };

    /* Check external shutdown flag */
    extern volatile sig_atomic_t g_shutdown_requested;
    req.shutdown_requested = (g_shutdown_requested != 0);

    /* Plan */
    struct activation_decision dec;
    activation_should_connect(&dec, &req);

    if (!dec.should_activate) {
        out->result = ACTIVATION_EXEC_SKIPPED;
        snprintf(out->reason, sizeof(out->reason), "%s", dec.reason);
        ctl->skip_count++;
        /* note the skipped request so the thread currently
         * holding the mutex reruns activate_best_chain before
         * transitioning out of CONNECTING. The block is already on
         * disk via accept_block, so the rerun picks it up without the
         * caller's pblock hint. */
        if (dec.result == ACTIVATION_SKIP_ALREADY_RUNNING)
            atomic_fetch_add(&ctl->deferred_pending, 1);
        return;
    }

    /* Acquire mutex — only one thread connects at a time */
    zcl_mutex_lock(&ctl->mutex);

    /* Re-check state under mutex (another thread may have changed it) */
    enum activation_state cur = activation_get_state(ctl);
    if (cur != ACTIVATION_READY && cur != ACTIVATION_AT_TIP) {
        zcl_mutex_unlock(&ctl->mutex);
        out->result = ACTIVATION_EXEC_SKIPPED;
        snprintf(out->reason, sizeof(out->reason),
                 "state changed to %s under contention",
                 activation_state_name(cur));
        return;
    }

    /* Transition to CONNECTING */
    activation_set_state(ctl, ACTIVATION_CONNECTING,
                         source == ACTIVATION_SRC_BOOT ? "boot" :
                         source == ACTIVATION_SRC_UTXO_REPLAY ? "replay" :
                         source == ACTIVATION_SRC_NEW_BLOCK ? "new_block" :
                         "p2p_trigger");

    /* Execute */
    struct validation_state vs;
    validation_state_init(&vs);
    bool ok = activate_best_chain(&vs, ctl->ms, ctl->coins_tip,
                                  ctl->params, pblock, ctl->datadir);

    /* Drain deferred activation requests that arrived while we were
     * holding the mutex. Each pass is activate_best_chain(pblock=NULL)
     * — the newly-accepted block is on disk, so the disk-read path
     * picks it up. find_most_work_chain is idempotent when no new
     * work arrived, so the loop converges quickly.
     *
     * replace the 8-round cap with a millisecond
     * budget. A 2,500-block gap with one block arriving per drain
     * cannot complete in 8 rounds; the ms budget lets the loop run
     * to convergence within bounded mutex-held latency. Boot path
     * still skips the drain (source==BOOT) — boot must return
     * quickly to open the RPC port. */
    if (source != ACTIVATION_SRC_BOOT) {
        const int64_t drain_budget_us = 2000 * 1000; /* 2s */
        const int     drain_hard_cap  = 4096;        /* belt + suspenders */
        int64_t       drain_start_us  = GetTimeMicros();
        int           drain_rounds    = 0;
        while (drain_rounds < drain_hard_cap) {
            /* also drain when activate_best_chain returned
             * early because of tip_child_connect_limit — otherwise we
             * stall the chain until the next P2P block arrival.
             * The OR is short-circuit, so the atomic_exchange on
             * deferred_pending still resets it whenever it is set. */
            bool deferred = atomic_exchange(&ctl->deferred_pending, 0) != 0;
            bool more_pending = process_block_active_tip_has_pending();
            if (!deferred && !more_pending)
                break;
            struct validation_state vs_r;
            validation_state_init(&vs_r);
            bool ok_r = activate_best_chain(&vs_r, ctl->ms, ctl->coins_tip,
                                             ctl->params, NULL, ctl->datadir);
            if (!ok_r) ok = false;
            drain_rounds++;
            if (GetTimeMicros() - drain_start_us > drain_budget_us)
                break;
        }
    }

    struct block_index *tip = active_chain_tip(&ctl->ms->chain_active);
    int tip_h = tip ? tip->nHeight : 0;

    ctl->last_activation_us = GetTimeMicros();
    ctl->last_tip_height = tip_h;
    ctl->activation_count++;

    /* Transition out of CONNECTING */
    if (!ok) {
        activation_set_state(ctl, ACTIVATION_READY, "activation_failed");
        out->result = ACTIVATION_EXEC_FAILED;
        snprintf(out->reason, sizeof(out->reason),
                 "activate_best_chain failed at h=%d", tip_h);
    } else {
        /* Don't declare at_tip if we're far behind the best known
         * header — blocks may not be downloaded yet (nChainTx==0
         * hides them from find_most_work_chain). Stay in READY to
         * keep the download pipeline active. */
        int best_h = ctl->ms->pindex_best_header
                   ? ctl->ms->pindex_best_header->nHeight : 0;
        /* Single advance-or-block decision: behind → typed blocker, caught
         * up → clear. Going to a bare READY without this was the silent-ready
         * hole — the reducer would report "ready" while behind, naming no
         * actionable reason and reaching no operator sink. */
        if (activation_eval_tip_blocker(tip_h, best_h)) {
            /* BEHIND the most-work valid-header chain; blocker now names
             * WHY + height + escape (visible in zcl_state subsystem=blocker).
             * Stay READY to keep the download/connect pipeline active, but it
             * is NOT a silent ready — the blocker is the truth. */
            activation_set_state(ctl, ACTIVATION_READY,
                                 ACTIVATION_BEHIND_BLOCKER_ID);
            out->result = ACTIVATION_EXEC_OK;
            out->new_tip_height = tip_h;
            out->reached_tip = false;
            snprintf(out->reason, sizeof(out->reason),
                     "BLOCKED %s tip=%d best_valid_header=%d gap=%d",
                     activation_behind_reason(),
                     tip_h, best_h, best_h - tip_h);
        } else {
            /* Genuinely caught up: tip == most-work header tip — the only
             * state where reporting at_tip is honest. */
            activation_set_state(ctl, ACTIVATION_AT_TIP, "at_tip");
            out->result = ACTIVATION_EXEC_OK;
            out->new_tip_height = tip_h;
            out->reached_tip = true;
            snprintf(out->reason, sizeof(out->reason), "tip=%d", tip_h);
        }
    }

    zcl_mutex_unlock(&ctl->mutex);
}

/* ── UTXO Wipe Protection ──────────────────────────────────────── */

void activation_should_allow_utxo_wipe(struct utxo_wipe_decision *out,
                                       enum activation_state state,
                                       bool anchor_active)
{
    memset(out, 0, sizeof(*out));

    if (state == ACTIVATION_ANCHOR_ACTIVE ||
        state == ACTIVATION_ANCHOR_CLEARING ||
        anchor_active) {
        out->safe_to_wipe = false;
        snprintf(out->reason, sizeof(out->reason),
                 "anchor active — imported UTXOs must be preserved");
        return;
    }

    out->safe_to_wipe = true;
    snprintf(out->reason, sizeof(out->reason), "no anchor, wipe allowed");
}
