/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Staged-sync supervisor children. Owns liveness contracts for the
 * authoritative eight-stage reducer pipeline in the chain domain.
 *
 * staged_sync_supervisor_register() registers them in pipeline order:
 *   header_admit → validate_headers → body_fetch → body_persist →
 *   script_validate → proof_validate → utxo_apply → tip_finalize. */

#include "supervisors/staged_sync_supervisor.h"
#include "util/log_macros.h"
#include "supervisors/domains.h"

#include "util/supervisor.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

/* Generous progress-quiet window shared by all eight stages:
 * 1800s (30 min) of IDLE before emitting a progress warning.
 * Stages legitimately idle for long stretches when the live chain is
 * waiting on input, so this is intentionally longer than the 900s
 * COORD_ESC_QUIET_US escalation window in chain_supervisor.c. */
#define STAGED_STAGE_QUIET_US ((int64_t)1800 * 1000 * 1000)

/* Header-admit stage supervisor child. */
static struct liveness_contract g_header_admit_contract;
static supervisor_child_id      g_header_admit_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_header_admit_ms = NULL;

static void header_admit_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_header_admit_ms) return;
    /* Drain a bounded batch each tick — keeps progress.kv churn low
     * and avoids starving other supervisor children. */
    (void)header_admit_stage_drain(HEADER_ADMIT_BATCH_PER_TICK);
    supervisor_progress(g_header_admit_id,
                        (int64_t)header_admit_stage_cursor());
    supervisor_tick(g_header_admit_id);
}

static void header_admit_stall(struct liveness_contract *c)
{
    (void)c;
    /* A stall here means header admission is not moving; the condition
     * layer decides whether this is missing input or a live-chain stall. */
    LOG_WARN("supervisor", "[supervisor] staged.header_admit stalled " "(cursor=%llu admitted=%llu) — stage log behind live chain", (unsigned long long)header_admit_stage_cursor(), (unsigned long long)header_admit_stage_admitted_total());
}

static void staged_header_admit_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_header_admit_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    /* Bind the stage to the live chainstate. If progress_store didn't
     * open at boot, init returns false — log and skip supervisor wire
     * so a misconfigured boot doesn't loop on a perma-IDLE child. */
    if (!header_admit_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.header_admit init failed — " "stage not running this boot");
        return;
    }

    g_header_admit_ms = ms;
    liveness_contract_init(&g_header_admit_contract, "staged.header_admit");
    atomic_store(&g_header_admit_contract.period_secs, (int64_t)2);
    /* Generous progress-quiet window: the stage can legitimately be IDLE
     * for long stretches when the live chain is stuck. 30 min before
     * we emit a progress warning. */
    atomic_store(&g_header_admit_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_header_admit_contract.on_tick  = header_admit_tick;
    g_header_admit_contract.on_stall = header_admit_stall;
    g_header_admit_id = supervisor_register_in_domain(g_chain_sup,
                                                      &g_header_admit_contract);
    if (g_header_admit_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.header_admit register failed");
    }
}

/* ── Wave S, S-3: validate_headers stage supervisor child ─────────── */
static struct liveness_contract g_vh_contract;
static supervisor_child_id      g_vh_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_vh_ms = NULL;

static void vh_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_vh_ms) return;
    (void)validate_headers_stage_drain(VH_BATCH_PER_TICK);
    supervisor_progress(g_vh_id,
                        (int64_t)validate_headers_stage_cursor());
    supervisor_tick(g_vh_id);
}

static void vh_stall(struct liveness_contract *c)
{
    (void)c;
    /* Validate can fall behind admit or wait on a stalled live chain; surface
     * the cursor gap and let conditions classify the cause. */
    LOG_WARN("supervisor", "[supervisor] staged.validate_headers stalled " "(cursor=%llu passed=%llu failed=%llu) — validator behind admit", (unsigned long long)validate_headers_stage_cursor(), (unsigned long long)validate_headers_stage_passed_total(), (unsigned long long)validate_headers_stage_failed_total());
}

static void staged_validate_headers_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_vh_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!validate_headers_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.validate_headers init failed — " "validator not running this boot");
        return;
    }

    g_vh_ms = ms;
    liveness_contract_init(&g_vh_contract, "staged.validate_headers");
    atomic_store(&g_vh_contract.period_secs, (int64_t)2);
    /* Same generous quiet window as header_admit — validation
     * tracks admission, which itself can be idle for hours when the
     * live chain is wedged. */
    atomic_store(&g_vh_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_vh_contract.on_tick  = vh_tick;
    g_vh_contract.on_stall = vh_stall;
    g_vh_id = supervisor_register_in_domain(g_chain_sup, &g_vh_contract);
    if (g_vh_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.validate_headers register failed");
    }
}

/* ── Wave S, S-4: body_fetch stage supervisor child ──────────────── */
static struct liveness_contract g_bf_contract;
static supervisor_child_id      g_bf_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_bf_ms = NULL;

static void bf_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_bf_ms) return;
    (void)body_fetch_stage_drain(BODY_FETCH_BATCH_PER_TICK);
    supervisor_progress(g_bf_id,
                        (int64_t)body_fetch_stage_cursor());
    supervisor_tick(g_bf_id);
}

static void bf_stall(struct liveness_contract *c)
{
    (void)c;
    /* Stall = body_fetch falling behind validate OR bodies
     * not arriving on disk. Either way, surface but do nothing
     * destructive. */
    LOG_WARN("supervisor", "[supervisor] staged.body_fetch stalled " "(cursor=%llu observed=%llu skipped=%llu) — fetch behind validate", (unsigned long long)body_fetch_stage_cursor(), (unsigned long long)body_fetch_stage_observed_total(), (unsigned long long)body_fetch_stage_skipped_total());
}

static void staged_body_fetch_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_bf_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!body_fetch_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.body_fetch init failed — " "fetch not running this boot");
        return;
    }

    g_bf_ms = ms;
    liveness_contract_init(&g_bf_contract, "staged.body_fetch");
    atomic_store(&g_bf_contract.period_secs, (int64_t)2);
    /* Same generous quiet window as upstream stages — body_fetch
     * naturally idles whenever validate is idle. */
    atomic_store(&g_bf_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_bf_contract.on_tick  = bf_tick;
    g_bf_contract.on_stall = bf_stall;
    g_bf_id = supervisor_register_in_domain(g_chain_sup, &g_bf_contract);
    if (g_bf_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.body_fetch register failed");
    }
}

/* ── Wave S, S-5: body_persist stage supervisor child ────────────── */
static struct liveness_contract g_bp_contract;
static supervisor_child_id      g_bp_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_bp_ms = NULL;

static void bp_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_bp_ms) return;
    (void)body_persist_stage_drain(BODY_PERSIST_BATCH_PER_TICK);
    supervisor_progress(g_bp_id,
                        (int64_t)body_persist_stage_cursor());
    supervisor_tick(g_bp_id);
}

static void bp_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("supervisor", "[supervisor] staged.body_persist stalled " "(cursor=%llu verified=%llu upstream_failed=%llu read_failed=%llu) " "— persist behind body_fetch", (unsigned long long)body_persist_stage_cursor(), (unsigned long long)body_persist_stage_verified_total(), (unsigned long long)body_persist_stage_upstream_failed_total(), (unsigned long long)body_persist_stage_read_failed_total());
}

static void staged_body_persist_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_bp_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!body_persist_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.body_persist init failed — " "persist not running this boot");
        return;
    }

    g_bp_ms = ms;
    liveness_contract_init(&g_bp_contract, "staged.body_persist");
    atomic_store(&g_bp_contract.period_secs, (int64_t)2);
    atomic_store(&g_bp_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_bp_contract.on_tick  = bp_tick;
    g_bp_contract.on_stall = bp_stall;
    g_bp_id = supervisor_register_in_domain(g_chain_sup, &g_bp_contract);
    if (g_bp_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.body_persist register failed");
    }
}

/* ── Wave S, S-6: script_validate stage supervisor child ───────────── */
static struct liveness_contract g_sv_contract;
static supervisor_child_id      g_sv_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_sv_ms = NULL;

static void sv_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_sv_ms) return;
    (void)script_validate_stage_drain(SCRIPT_VALIDATE_BATCH_PER_TICK);
    supervisor_progress(g_sv_id,
                        (int64_t)script_validate_stage_cursor());
    supervisor_tick(g_sv_id);
}

static void sv_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("supervisor", "[supervisor] staged.script_validate stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— script validation behind body_persist", (unsigned long long)script_validate_stage_cursor(), (unsigned long long)script_validate_stage_verified_total(), (unsigned long long)script_validate_stage_upstream_failed_total(), (unsigned long long)script_validate_stage_internal_error_total());
}

static void staged_script_validate_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_sv_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!script_validate_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.script_validate init failed — " "script validation not running this boot");
        return;
    }

    g_sv_ms = ms;
    liveness_contract_init(&g_sv_contract, "staged.script_validate");
    atomic_store(&g_sv_contract.period_secs, (int64_t)2);
    atomic_store(&g_sv_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_sv_contract.on_tick  = sv_tick;
    g_sv_contract.on_stall = sv_stall;
    g_sv_id = supervisor_register_in_domain(g_chain_sup, &g_sv_contract);
    if (g_sv_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.script_validate register failed");
    }
}

/* ── Wave S, S-7: proof_validate stage supervisor child ────────────── */
static struct liveness_contract g_pv_contract;
static supervisor_child_id      g_pv_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_pv_ms = NULL;

static void pv_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_pv_ms) return;
    (void)proof_validate_stage_drain(PROOF_VALIDATE_BATCH_PER_TICK);
    supervisor_progress(g_pv_id,
                        (int64_t)proof_validate_stage_cursor());
    supervisor_tick(g_pv_id);
}

static void pv_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("supervisor", "[supervisor] staged.proof_validate stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— proof validation behind script_validate", (unsigned long long)proof_validate_stage_cursor(), (unsigned long long)proof_validate_stage_verified_total(), (unsigned long long)proof_validate_stage_upstream_failed_total(), (unsigned long long)proof_validate_stage_internal_error_total());
}

static void staged_proof_validate_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_pv_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!proof_validate_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.proof_validate init failed — " "proof validation not running this boot");
        return;
    }

    g_pv_ms = ms;
    liveness_contract_init(&g_pv_contract, "staged.proof_validate");
    atomic_store(&g_pv_contract.period_secs, (int64_t)2);
    atomic_store(&g_pv_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_pv_contract.on_tick  = pv_tick;
    g_pv_contract.on_stall = pv_stall;
    g_pv_id = supervisor_register_in_domain(g_chain_sup, &g_pv_contract);
    if (g_pv_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.proof_validate register failed");
    }
}

/* ── Wave S, S-8: utxo_apply stage supervisor child ───────────────── */
static struct liveness_contract g_uv_contract;
static supervisor_child_id      g_uv_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_uv_ms = NULL;

static void uv_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_uv_ms) return;
    (void)utxo_apply_stage_drain(UTXO_APPLY_BATCH_PER_TICK);
    supervisor_progress(g_uv_id,
                        (int64_t)utxo_apply_stage_cursor());
    supervisor_tick(g_uv_id);
}

static void uv_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("supervisor", "[supervisor] staged.utxo_apply stalled " "(cursor=%llu verified=%llu upstream_failed=%llu internal_error=%llu) " "— UTXO apply behind proof_validate", (unsigned long long)utxo_apply_stage_cursor(), (unsigned long long)utxo_apply_stage_verified_total(), (unsigned long long)utxo_apply_stage_upstream_failed_total(), (unsigned long long)utxo_apply_stage_internal_error_total());
}

static void staged_utxo_apply_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_uv_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!utxo_apply_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.utxo_apply init failed — " "UTXO apply not running this boot");
        return;
    }

    g_uv_ms = ms;
    liveness_contract_init(&g_uv_contract, "staged.utxo_apply");
    atomic_store(&g_uv_contract.period_secs, (int64_t)2);
    atomic_store(&g_uv_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_uv_contract.on_tick  = uv_tick;
    g_uv_contract.on_stall = uv_stall;
    g_uv_id = supervisor_register_in_domain(g_chain_sup, &g_uv_contract);
    if (g_uv_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.utxo_apply register failed");
    }
}

/* ── Wave S, S-9: tip_finalize stage supervisor child ─────────────── */
static struct liveness_contract g_tf_contract;
static supervisor_child_id      g_tf_id = SUPERVISOR_INVALID_ID;
static struct main_state       *g_tf_ms = NULL;

static void tf_tick(struct liveness_contract *c)
{
    (void)c;
    if (!g_tf_ms) return;
    (void)tip_finalize_stage_drain(TIP_FINALIZE_BATCH_PER_TICK);
    supervisor_progress(g_tf_id,
                        (int64_t)tip_finalize_stage_cursor());
    supervisor_tick(g_tf_id);
}

static void tf_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("supervisor", "[supervisor] staged.tip_finalize stalled " "(cursor=%llu finalized=%llu upstream_failed=%llu reorg=%llu) " "— tip finalize behind utxo_apply or live tip", (unsigned long long)tip_finalize_stage_cursor(), (unsigned long long)tip_finalize_stage_finalized_total(), (unsigned long long)tip_finalize_stage_upstream_failed_total(), (unsigned long long)tip_finalize_stage_reorg_detected_total());
}

static void staged_tip_finalize_register(struct main_state *ms)
{
    if (!ms) return;
    if (g_tf_id != SUPERVISOR_INVALID_ID) return;  /* idempotent */

    if (!tip_finalize_stage_init(ms)) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.tip_finalize init failed — " "tip finalize not running this boot");
        return;
    }

    g_tf_ms = ms;
    liveness_contract_init(&g_tf_contract, "staged.tip_finalize");
    atomic_store(&g_tf_contract.period_secs, (int64_t)2);
    atomic_store(&g_tf_contract.progress_max_quiet_us,
                 STAGED_STAGE_QUIET_US);
    g_tf_contract.on_tick  = tf_tick;
    g_tf_contract.on_stall = tf_stall;
    g_tf_id = supervisor_register_in_domain(g_chain_sup, &g_tf_contract);
    if (g_tf_id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("supervisor", "[supervisor] WARN staged.tip_finalize register failed");
    }
}

void staged_sync_supervisor_register(struct main_state *ms)
{
    if (!ms) return;
    supervisor_domains_init();
    /* Pipeline order — identical to the original boot_services.c
     * registration sequence (S-2 → S-9). */
    staged_header_admit_register(ms);
    staged_validate_headers_register(ms);
    staged_body_fetch_register(ms);
    staged_body_persist_register(ms);
    staged_script_validate_register(ms);
    staged_proof_validate_register(ms);
    staged_utxo_apply_register(ms);
    staged_tip_finalize_register(ms);
}
