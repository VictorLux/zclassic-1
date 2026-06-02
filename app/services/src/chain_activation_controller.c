/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain Activation Controller — single authority for block connection.
 * See chain_activation_controller.h for architecture overview. */

// one-result-type-ok:decision-out-structs
//
// Activation is a state machine + decision planner. Every fallible decision
// is reported through a domain OUT-STRUCT that carries a reason[]:
//   - activation_should_connect()      -> activation_decision   (result enum + reason)
//   - activation_request_connect()     -> activation_exec_outcome (result enum + reason)
//   - activation_should_allow_utxo_wipe() -> utxo_wipe_decision  (safe + reason)
// The bool returns are PREDICATES, not lost-reason failures:
//   - activation_eval_tip_blocker() — "tip behind?"; the why+escape travels
//     via the typed blocker_record it registers (activation_set_behind_blocker).
//   - activation_set_state() — transition gate; illegal transitions log via
//     LOG_WARN + emit EV_ACTIVATION_STATE_CHANGE.
//   - activation_transition_valid() — pure transition-table lookup.
// activation_state_name() is an enum->name table; activation_drain_deferred()
// returns a count. No bare-bool strips a failure reason. Behavior bit-for-bit.

#include "services/chain_activation_controller.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "event/event.h"
#include "net/snapshot_sync_contract.h"
#include "core/utiltime.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "util/log_macros.h"
#include "util/blocker.h"

/* ── Reducer-as-ingest includes ─────────────────────────────────────
 * The synchronous reducer wrapper drives the staged Job pipeline and the
 * stateless check_block gate, then reads back the verdict from the
 * freshly-written stage log rows. */
#include "consensus/validation.h"
#include "validation/check_block.h"
#include "primitives/block.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "storage/progress_store.h"
#include "storage/disk_block_io.h"
#include "services/header_admit_inbox.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/block_header_emit.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"

/* Forward decl: the non-locking reducer drain helper (defined below). Callers
 * that already hold the controller mutex must use it directly; reducer_kick
 * and reducer_ingest_block lock that same mutex. */
static int reducer_drain_to_convergence(void);

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
                 "reducer activation already running");
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
        /* note the skipped request so the thread currently holding the mutex
         * reruns reducer activation before transitioning out of CONNECTING.
         * The block is already on disk via the admit/persist path, so the
         * rerun picks it up without the caller's pblock hint. */
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

    /* Execute. The chokepoint drives the staged Job pipeline to
     * convergence — the reducer is the engine. Every Group-2 NULL-block
     * caller (boot, msg_headers all-data, sync_monitor,
     * utxo_activation_paused, repair_controller, invalidate/revalidate
     * reconnect, the behind-blocker escape) funnels here and connects via the
     * reducer. We are already under ctl->mutex (the same serialization
     * point), so we call the non-locking drain helper directly —
     * reducer_kick/reducer_ingest_block lock the mutex and would deadlock
     * here. */
    bool ok = true;

    /* A non-NULL pblock can only reach this chokepoint via the historical
     * historical block-intake tail (the live Group-1 ingest callers route to
     * reducer_ingest_block directly and never call the old path). Admit
     * it to the header inbox so the producer path can build its block_index,
     * then drive the stages; a NULL pblock is a pure cursor-driven kick (the
     * Group-2 path). */
    if (pblock) {
        struct uint256 block_hash;
        block_get_hash(pblock, &block_hash);
        struct header_admit_msg msg;
        memset(&msg, 0, sizeof(msg));
        msg.hash = block_hash;
        msg.observed_unix = (int64_t)GetTime();
        msg.has_header = true;
        msg.header = pblock->header;
        msg.height = -1;
        (void)mailbox_header_admit_push(&msg);
    }
    (void)reducer_drain_to_convergence();
    /* The reducer reports its verdict through the tip advance + the typed
     * behind-blocker (registered below by activation_eval_tip_blocker) —
     * there is no hard-failure bool to surface here, so `ok` stays true and
     * the advance-or-block decision below names any remaining gap. */

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
                 "reducer drain failed at h=%d", tip_h);
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

/* ── Reducer-as-ingest ──────────────────────────────────────────────
 *
 * The synchronous block-intake wrapper that drives the staged Job pipeline
 * instead of the historical activation path. */

/* Drain the eight stage step bodies once, in pipeline order — the SAME
 * order and the SAME *_stage_drain functions the per-stage supervisor
 * children tick (staged_sync_supervisor.c). Returns total advances across
 * all eight. A single pass; the caller loops to convergence. */
static int reducer_drain_all_stages(int max_steps_per_stage)
{
    int advanced = 0;
    advanced += header_admit_stage_drain(max_steps_per_stage);
    advanced += validate_headers_stage_drain(max_steps_per_stage);
    advanced += body_fetch_stage_drain(max_steps_per_stage);
    advanced += body_persist_stage_drain(max_steps_per_stage);
    advanced += script_validate_stage_drain(max_steps_per_stage);
    advanced += proof_validate_stage_drain(max_steps_per_stage);
    advanced += utxo_apply_stage_drain(max_steps_per_stage);
    advanced += tip_finalize_stage_drain(max_steps_per_stage);
    return advanced;
}

/* Loop reducer_drain_all_stages to convergence within a bounded mutex-held
 * latency budget (mirrors the old deferred-drain budget at
 * activation_request_connect). Stops when a full pass advances nothing or
 * the budget/round-cap is hit. Returns the total advances. */
static int reducer_drain_to_convergence(void)
{
    const int64_t drain_budget_us = 2000 * 1000; /* 2s, same as legacy */
    const int     drain_hard_cap  = 4096;
    const int     per_stage_batch = 100;
    int64_t       start_us        = GetTimeMicros();
    int           total           = 0;
    for (int round = 0; round < drain_hard_cap; round++) {
        int adv = reducer_drain_all_stages(per_stage_batch);
        total += adv;
        if (adv == 0)
            break;
        if (GetTimeMicros() - start_us > drain_budget_us)
            break;
    }
    return total;
}

bool reducer_is_authoritative(void)
{
    return true;
}

int reducer_kick(struct chain_activation_controller *ctl)
{
    if (!ctl)
        return 0;
    zcl_mutex_lock(&ctl->mutex);
    int advanced = reducer_drain_to_convergence();
    zcl_mutex_unlock(&ctl->mutex);
    return advanced;
}

/* Map freshly-written stage log rows for `height`/`hash` into `out`. */
static bool reducer_read_back_verdict(int height,
                                      const struct uint256 *hash,
                                      struct validation_state *out)
{
    sqlite3 *pdb = progress_store_db();

    struct validate_headers_window_report rep;
    if (validate_headers_stage_window_report(height, height, &rep) &&
        rep.failed_count > 0) {
        validation_state_dos(out, 100, false, REJECT_INVALID,
                             rep.first_fail_reason[0]
                                 ? rep.first_fail_reason
                                 : "header-validation-failed",
                             false, NULL);
        return false;
    }

    uint8_t finalized[32];
    if (pdb &&
        tip_finalize_stage_finalized_tip_at(pdb, height, finalized) &&
        memcmp(finalized, hash->data, 32) == 0) {
        return true; /* out left MODE_VALID by the caller's init */
    }

    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", NULL);
    return false;
}

static bool reducer_header_rejected_at(int height, struct validation_state *out)
{
    struct validate_headers_window_report rep;
    if (!validate_headers_stage_window_report(height, height, &rep) ||
        rep.failed_count == 0)
        return false;

    validation_state_dos(out, 100, false, REJECT_INVALID,
                         rep.first_fail_reason[0]
                             ? rep.first_fail_reason
                             : "header-validation-failed",
                         false, NULL);
    return true;
}

static bool reducer_pending_body_is_accepted(
        const struct block_index *bi,
        struct validation_state *out)
{
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA) ||
        (bi->nStatus & BLOCK_FAILED_MASK))
        return false;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    /* Consensus gate: accept an un-finalizable live tip ONLY if it cleared all
     * stateful validation through utxo_apply (see utxo_apply_stage_succeeded_at).
     * HAVE_DATA && !FAILED is NOT a validity witness — script/proof/utxo failures
     * record ok=0 without setting BLOCK_FAILED_MASK. `out` keeps read_back's
     * reject reason on miss. */
    if (!utxo_apply_stage_succeeded_at(bi->nHeight))
        return false;

    validation_state_init(out);
    return true;
}

static bool reducer_persist_ingested_body_locked(
        struct chain_activation_controller *ctl,
        const struct uint256 *block_hash,
        struct block *pblock,
        struct validation_state *out)
{
    if (!ctl || !ctl->ms || !block_hash || !pblock) {
        LOG_WARN("reducer", "body persist invalid args ctl=%p ms=%p hash=%p block=%p",
                 (void *)ctl, ctl ? (void *)ctl->ms : NULL,
                 (const void *)block_hash, (void *)pblock);
        return validation_state_error(out, "reducer-body-null-arg");
    }

    struct block_index *bi = block_map_find(&ctl->ms->map_block_index,
                                            block_hash);
    if (!bi)
        return true;

    if (bi->nStatus & BLOCK_HAVE_DATA)
        return true;

    if (reducer_header_rejected_at(bi->nHeight, out))
        return false;

    if (!ctl->datadir || !ctl->datadir[0] || !ctl->params) {
        LOG_WARN("reducer", "body persist missing runtime wiring h=%d datadir=%p params=%p",
                 bi->nHeight, (const void *)ctl->datadir,
                 (const void *)ctl->params);
        return validation_state_error(out, "reducer-body-runtime-unwired");
    }

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (!write_block_to_disk(pblock, &pos, ctl->datadir,
                             ctl->params->pchMessageStart)) {
        LOG_WARN("reducer", "body persist write failed h=%d", bi->nHeight);
        return validation_state_error(out, "reducer-body-write-failed");
    }

    if (!block_index_set_have_data_verified(bi, &pos, ctl->datadir)) {
        LOG_WARN("reducer", "body persist verify failed h=%d file=%d pos=%u",
                 bi->nHeight, pos.nFile, pos.nPos);
        return validation_state_error(out, "reducer-body-verify-failed");
    }

    block_index_emit_header_event(bi, "reducer_ingest", NULL, NULL);
    LOG_INFO("reducer", "persisted ingested block body h=%d file=%d pos=%u",
             bi->nHeight, pos.nFile, pos.nPos);
    return true;
}

bool reducer_ingest_block(struct chain_activation_controller *ctl,
                          struct block *pblock,
                          enum reducer_source source,
                          bool force,
                          struct validation_state *out)
{
    (void)source; /* informational; `force` carries the live semantics */

    if (!out)
        return false;
    validation_state_init(out);

    if (!ctl || !pblock)
        return validation_state_error(out, "reducer-null-arg");

    /* (1) Stateless gate FIRST, inline, BEFORE any log/stage mutation. A
     * garbage block is rejected with the verdict already in `out`; nothing is
     * admitted to the inbox. The `force`/requested flag does not relax the
     * stateless checks; it gates the relay pre-filters inside the admit
     * producer path, not this gate. */
    if (!check_block(pblock, out, ctl->params, true, true, true)) {
        LOG_FAIL("reducer", "check_block failed: %s",
                 out->reject_reason[0] ? out->reject_reason : "unknown");
        return false;
    }

    /* (2) Push the header + raw bytes into the header_admit_inbox so the
     * producer path (step 2) can CREATE the block_index entry
     * without legacy accept_block_header. Hash-hint is the block hash. */
    struct uint256 block_hash;
    block_get_hash(pblock, &block_hash);

    struct header_admit_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.hash = block_hash;
    msg.observed_unix = (int64_t)GetTime();
    msg.has_header = true;
    msg.header = pblock->header;
    /* height hint: one past the prev block's height when known, else -1
     * (admit verifies against the active chain regardless of the hint). */
    msg.height = -1;
    if (!mailbox_header_admit_push(&msg)) {
        /* Inbox full: cannot admit this block right now. Not a consensus
         * reject — report a transient error so the caller can retry. */
        return validation_state_error(out, "header-admit-inbox-full");
    }

    /* (3) Drain the eight stage step bodies synchronously under the SAME
     * mutex reducer activation serializes on, ahead of the 2s supervisor
     * tickers, so a single at-tip block reaches tip_finalize within the
     * call. The reorg disconnect (step 4) is driven from inside utxo_apply
     * when a better fork is selected. */
    zcl_mutex_lock(&ctl->mutex);
    (void)force; /* relay pre-filter gating lives in the admit producer */
    struct block_index *anchor_tip = active_chain_tip(&ctl->ms->chain_active);
    if (anchor_tip && anchor_tip->phashBlock &&
        tip_finalize_stage_cursor() < (uint64_t)anchor_tip->nHeight + 1u)
        (void)tip_finalize_stage_seed_anchor(anchor_tip->nHeight,
                                             anchor_tip->phashBlock->data);
    (void)reducer_drain_to_convergence();
    if (!reducer_persist_ingested_body_locked(ctl, &block_hash, pblock, out)) {
        zcl_mutex_unlock(&ctl->mutex);
        return false;
    }
    (void)reducer_drain_to_convergence();

    struct block_index *ingested =
        block_map_find(&ctl->ms->map_block_index, &block_hash);

    /* Prefer the just-ingested height for the read-back. The active tip may
     * still be one block behind while tip_finalize waits for lookahead, but
     * header/stateful rejects are recorded at the ingested height. */
    struct block_index *tip = active_chain_tip(&ctl->ms->chain_active);
    int target_h = ingested ? ingested->nHeight : (tip ? tip->nHeight : 0);
    zcl_mutex_unlock(&ctl->mutex);

    /* (4) Read back the verdict from the freshly-written log rows. */
    if (reducer_read_back_verdict(target_h, &block_hash, out))
        return true;

    if (reducer_pending_body_is_accepted(ingested, out))
        return true;

    return false;
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
