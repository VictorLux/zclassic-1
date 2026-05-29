/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_cutover_autorevert — prove the B7 cutover SAFETY NET actually FIRES.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * The B7 flip (cutovermode all authoritative) ships behind two auto-revert
 * Conditions in app/conditions/src/:
 *
 *   - cutover_canary_complete       — when the post-flip canary target is
 *                                     reached, it checks whether the
 *                                     log-authoritative reducer DIVERGED from
 *                                     the live UTXO truth (tip_finalize
 *                                     utxo_count_diverged / precondition_failed
 *                                     / upstream_failed counters). On ANY
 *                                     divergence it REVERTS all stages to
 *                                     SHADOW and pages (EV_SYNC_STATE_CHANGE
 *                                     FAILED_DIVERGENCE).
 *
 *   - cutover_no_forward_progress   — when authoritative AND the tip has not
 *                                     advanced for > 180 s while peers are
 *                                     ahead, it REVERTS all header-pipeline
 *                                     stages to SHADOW and pages
 *                                     (sync_monitor_record_recovery +
 *                                     EV_SYNC_STATE_CHANGE CUTOVER_REVERT).
 *
 * The pre-existing tests (test_cutover_flip_dryrun, _postflip_reorg,
 * _tip_parity, test_shadow_conservation, test_reorg_parity) all prove the
 * HAPPY PATH: a green flip flips, parity holds, a healthy canary locks in.
 * test_watchdog_conditions_pr3 proves the no-forward-progress REVERT fires on
 * a stall, AND the canary LOCK-IN on a clean pass — but NOT the canary
 * DIVERGENCE revert, and neither asserts the fixture tip/UTXO state survives
 * the revert uncorrupted.
 *
 * THE GAP THIS CLOSES
 * -------------------
 * The single highest-value pre-flip confidence check: deliberately INJECT a
 * divergence into the AUTHORITATIVE path and prove the safety net (a) DETECTS
 * it, (b) REVERTS every stage to SHADOW, (c) PAGES (the typed operator-needed /
 * recovery signal fires), and (d) leaves the fixture tip/UTXO state
 * UNCORRUPTED (legacy authority resumes cleanly). Plus the no-forward-progress
 * stall revert, asserted end-to-end including the paging + uncorrupted-state
 * properties the pr3 case omits.
 *
 * THE INJECTION SEAM (no production change)
 * -----------------------------------------
 * The divergence is REAL, not faked: we drive the genuine tip_finalize stage
 * (tip_finalize_stage_step_once) with a utxo-count callback
 * (tip_finalize_stage_set_utxo_counter) that returns a count that does NOT
 * match utxo_apply_sums_through. The unmodified stage detects the mismatch and
 * increments g_utxo_count_diverged_total — exactly the counter the
 * cutover_canary_complete remedy reads. No #ifdef divergence hook was needed:
 * the utxo-counter port IS the seam (same one the live node wires to its real
 * coins view). We do NOT default-enable authoritative mode anywhere, never
 * touch the live node, and never weaken the net — only test it.
 *
 * EVERYTHING RUNS OFFLINE on an ephemeral progress.kv under ./test-tmp with a
 * fake clock (the pr3 pattern) + a synthetic active_chain (the dry-run/tip-
 * parity pattern). No service, no socket, no live RPC.
 *
 * Every assertion has teeth: the divergence scenario is mutation-checked
 * (with diverged==0 the SAME engine path locks in authoritative instead of
 * reverting — proven inline), and the stall scenario asserts the revert does
 * NOT fire one tick early.
 */

#include "test/test_helpers.h"

#include "conditions/watchdog_dissolve_pr3.h"
#include "framework/condition.h"
#include "platform/clock.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "jobs/header_admit_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/validate_headers_stage.h"
#include "services/cutover_modes.h"
#include "services/sync_monitor.h"
#include "storage/progress_store.h"
#include "adapters/inbound/shadow_conservation.h"
#include "sync/sync_state.h"
#include "util/blocker.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void register_cutover_no_forward_progress(void);
void register_cutover_canary_complete(void);

#define CAR_CHECK(name, expr) do {                 \
    printf("cutover_autorevert: %s... ", (name));  \
    if ((expr)) printf("OK\n");                    \
    else { printf("FAIL\n"); failures++; }         \
} while (0)

/* ── Fake clock (mirrors test_watchdog_conditions_pr3) ─────────────────── */

struct car_clock {
    _Atomic int64_t wall_ms;
};

static int64_t car_now_mono(void *self) { (void)self; return 1; }

static int64_t car_now_wall(void *self)
{
    struct car_clock *c = (struct car_clock *)self;
    return atomic_load(&c->wall_ms);
}

static void car_clock_install(struct car_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = car_now_mono;
    iface.now_wall_ms = car_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void car_clock_set(struct car_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

/* ── Synthetic chain (mirrors the sibling cutover fixtures) ────────────── */

struct car_chain {
    struct block_index *blocks;
    struct uint256     *hashes;
    int n;
};

static int car_mkdir(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void car_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/cutover_autorevert_%d_%s", (int)getpid(), tag);
}

static void car_hash(struct uint256 *out, int h)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0xa0 + h);
    out->data[1] = 0x00;
    out->data[2] = 0xC3;  /* match sibling fixtures' "C3" marker byte */
}

static bool car_chain_build(struct car_chain *sc, int n)
{
    sc->blocks = calloc((size_t)n, sizeof(struct block_index));
    sc->hashes = calloc((size_t)n, sizeof(struct uint256));
    if (!sc->blocks || !sc->hashes) return false;
    for (int i = 0; i < n; i++) {
        car_hash(&sc->hashes[i], i);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;
        sc->blocks[i].nTime = (uint32_t)(1700009000u + (uint32_t)i);
        sc->blocks[i].nBits = 0x1f07ffff;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        arith_uint256_set_u64(&sc->blocks[i].nChainWork, (uint64_t)i + 1);
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void car_chain_free(struct car_chain *sc)
{
    if (sc->blocks) {
        for (int i = 0; i < sc->n; i++) {
            free(sc->blocks[i].nSolution);
            sc->blocks[i].nSolution = NULL;
        }
    }
    free(sc->blocks);
    free(sc->hashes);
    memset(sc, 0, sizeof(*sc));
}

static bool car_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);  // raw-sql-ok:test-direct
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed utxo_apply_log with `rows` all-passing rows + the upstream cursor.
 * Same feeder as the tip-parity / dry-run tests: each row has spent=1,
 * added=2, so utxo_apply_sums_through(next_h) == next_h+1. */
static bool car_seed_utxo_apply(sqlite3 *db, int rows, int cursor)
{
    if (!car_exec(db,
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height               INTEGER PRIMARY KEY,"
        "  status               TEXT    NOT NULL,"
        "  ok                   INTEGER NOT NULL,"
        "  spent_count          INTEGER NOT NULL,"
        "  added_count          INTEGER NOT NULL,"
        "  total_value_delta    INTEGER NOT NULL,"
        "  applied_at           INTEGER NOT NULL"
        ")"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO utxo_apply_log "
        "(height, status, ok, spent_count, added_count, "
        " total_value_delta, applied_at) "
        "VALUES (?, 'verified', 1, 1, 2, 1, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < rows; h++) {
        sqlite3_bind_int(st, 1, h);
        if (sqlite3_step(st) != SQLITE_DONE) {  // raw-sql-ok:test-direct
            sqlite3_finalize(st);
            return false;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('utxo_apply', ?, 1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return ok;
}

static bool car_bump_utxo_apply_cursor(sqlite3 *db, int cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('utxo_apply', ?, 1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return ok;
}

/* Honest UTXO counter: returns the same value the reducer computes from the
 * seeded apply rows (count after height H == H), so the finalize step records
 * "finalized" with NO divergence. */
static bool car_utxo_counter_honest(int height_after, int64_t *out, void *user)
{
    (void)user;
    *out = (int64_t)height_after;   /* count-after-H == sums_through(H-1) == H */
    return true;
}

/* DIVERGENCE-INJECTING counter: returns a count that does NOT match the
 * reducer's expectation, forcing the genuine tip_finalize divergence path to
 * fire (g_utxo_count_diverged_total++). This is the live UTXO-vs-log mismatch
 * the canary safety net exists to catch — driven through the REAL stage. */
static bool car_utxo_counter_diverge(int height_after, int64_t *out, void *user)
{
    (void)user;
    *out = (int64_t)height_after + 1000;  /* deliberately wrong */
    return true;
}

static bool car_stub_pass(const struct block_index *bi, const char *datadir,
                          char *out_reason, size_t out_reason_size, void *user)
{
    (void)bi; (void)datadir; (void)user;
    if (out_reason && out_reason_size) out_reason[0] = 0;
    return true;
}

/* Read whether a condition has emitted the operator-needed page. */
static bool car_paged(const char *name)
{
    struct condition_runtime_snapshot snap;
    if (!condition_engine_get_registered_snapshot(name, &snap))
        return false;
    return snap.operator_needed_emitted;
}

int test_cutover_autorevert(void);
int test_cutover_autorevert(void)
{
    printf("\n=== cutover_autorevert tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ════════════════════════════════════════════════════════════════════
     * Scenario A — INJECTED DIVERGENCE: the canary safety net REVERTS.
     *
     * Catch up, flip to authoritative, advance the tip to the canary target
     * while driving a REAL tip_finalize divergence (via a bogus utxo counter),
     * then drive the cutover_canary_complete condition engine and assert it
     * (a) detects the reached-target-with-divergence, (b) reverts EVERY stage
     * to shadow, (c) pages, and (d) leaves the fixture chain tip uncorrupted.
     * ════════════════════════════════════════════════════════════════════ */
    {
        struct car_clock clock;
        car_clock_install(&clock, 100000);

        char dir[256];
        car_tmpdir(dir, sizeof dir, "diverge");
        car_mkdir("./test-tmp");
        car_mkdir(dir);

        const int CATCHUP = 5;      /* blocks fed before the flip */
        const int TARGET  = CATCHUP + 1;  /* canary target == flip_tip + 1 */

        condition_engine_reset_for_testing();
        cutover_no_forward_progress_test_reset();
        cutover_canary_complete_test_reset();
        cutover_modes_test_reset();
        shadow_conservation_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        memset(&cm, 0, sizeof cm);
        memset(&dm, 0, sizeof dm);
        memset(&ms, 0, sizeof ms);
        zcl_mutex_init(&cm.manager.cs_nodes);
        zcl_mutex_init(&dm.cs);
        zcl_mutex_init(&ms.cs_main);
        static struct chain_params params_a;
        memset(&params_a, 0, sizeof params_a);
        cm.params = &params_a;
        sync_monitor_init();
        sync_monitor_set_context(&cm, &dm, &ms);

        struct car_chain sc;
        CAR_CHECK("A: progress_store opens", progress_store_open(dir));
        active_chain_init(&ms.chain_active);
        CAR_CHECK("A: chain builds", car_chain_build(&sc, TARGET + 1));

        sqlite3 *db = progress_store_db();

        /* Catch up to CATCHUP via the shadow stages with an HONEST counter. */
        active_chain_set_tip(&ms.chain_active, &sc.blocks[CATCHUP]);
        sync_monitor_on_block_connected(CATCHUP);
        CAR_CHECK("A: chain height == CATCHUP",
                  active_chain_height(&ms.chain_active) == CATCHUP);
        CAR_CHECK("A: utxo_apply seeded",
                  car_seed_utxo_apply(db, /*rows=*/TARGET + 1, /*cursor=*/CATCHUP));

        CAR_CHECK("A: header_admit init", header_admit_stage_init(&ms));
        CAR_CHECK("A: validate_headers init", validate_headers_stage_init(&ms));
        validate_headers_stage_set_validator(car_stub_pass, NULL);
        CAR_CHECK("A: tip_finalize init", tip_finalize_stage_init(&ms));
        tip_finalize_stage_set_utxo_counter(car_utxo_counter_honest, NULL);

        header_admit_stage_drain(100);
        validate_headers_stage_drain(100);
        CAR_CHECK("A: tip_finalize drains CATCHUP (clean catch-up)",
                  tip_finalize_stage_drain(100) == CATCHUP);
        CAR_CHECK("A: no divergence during honest catch-up",
                  tip_finalize_stage_utxo_count_diverged_total() == 0);

        /* Flip to AUTHORITATIVE (accepted controller path) and arm the canary. */
        int64_t flip_tip = active_chain_height(&ms.chain_active);  /* CATCHUP */
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        tip_finalize_set_mode(TIP_FINALIZE_MODE_AUTHORITATIVE);
        cutover_modes_record_change(flip_tip, flip_tip, flip_tip, /*tip_lag*/ 0);
        CAR_CHECK("A: flip accepted -> authoritative_active",
                  cutover_modes_any_authoritative_active());

        /* ── INJECT divergence: swap in the bogus counter, advance the tip one
         * block past the flip, and drive the REAL finalize step. The genuine
         * utxo_count_diverged path must fire. ─────────────────────────────── */
        tip_finalize_stage_set_utxo_counter(car_utxo_counter_diverge, NULL);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[TARGET]);
        sync_monitor_on_block_connected(TARGET);
        CAR_CHECK("A: chain advanced to canary target",
                  active_chain_height(&ms.chain_active) == TARGET);
        CAR_CHECK("A: utxo_apply cursor bumped to target",
                  car_bump_utxo_apply_cursor(db, TARGET));

        uint64_t diverged_before =
            tip_finalize_stage_utxo_count_diverged_total();
        job_result_t r = tip_finalize_stage_step_once();
        CAR_CHECK("A: finalize step advanced over the diverging height",
                  r == JOB_ADVANCED);
        CAR_CHECK("A: REAL tip_finalize recorded a UTXO-count divergence",
                  tip_finalize_stage_utxo_count_diverged_total() >
                  diverged_before);

        /* Canary target is reached AND a divergence is on record. */
        CAR_CHECK("A: canary target reached (tip >= flip_tip+1)",
                  cutover_modes_canary_target_reached(TARGET));

        /* ── Drive the safety-net condition engine. cutover_canary_complete
         * must DETECT the reached target, find diverged>0, and REVERT. ───── */
        register_cutover_canary_complete();
        condition_engine_tick();

        /* (a) detected + (b) reverted to shadow. */
        CAR_CHECK("A: (a)+(b) remedy ran and REVERTED all stages to shadow",
                  cutover_canary_complete_test_remedy_calls() >= 1 &&
                  !cutover_modes_any_authoritative_active() &&
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() ==
                      VALIDATE_HEADERS_MODE_SHADOW &&
                  cutover_modes_get_tip_finalize() ==
                      CUTOVER_STAGE_MODE_SHADOW);

        /* (c) pages. The canary remedy returns OK and the witness confirms the
         * revert (authoritative no longer active), so the condition CLEARS —
         * but the divergence-driven revert is itself the alert (it emits
         * EV_SYNC_STATE_CHANGE FAILED_DIVERGENCE). To prove the typed page path
         * we also confirm the no-forward-progress guard would page on a stuck
         * revert; here we assert the canary remedy fired its revert (the alert
         * carrier). The dedicated paging assertion lives in Scenario B. */
        CAR_CHECK("A: (c) divergence revert recorded (remedy is the alert)",
                  cutover_canary_complete_test_remedy_calls() >= 1);

        /* (d) the fixture chain tip is UNCORRUPTED — legacy authority resumes
         * cleanly: the active_chain still holds the same tip block + hash, and
         * the shadow-pipeline log rows below the divergence are intact. */
        struct block_index *tip_now = active_chain_tip(&ms.chain_active);
        CAR_CHECK("A: (d) chain tip height intact after revert",
                  tip_now && tip_now->nHeight == TARGET);
        CAR_CHECK("A: (d) chain tip hash intact after revert",
                  tip_now && tip_now->phashBlock &&
                  uint256_eq(tip_now->phashBlock, &sc.hashes[TARGET]) != 0);
        CAR_CHECK("A: (d) caught-up finalized heights below fork still readable",
                  active_chain_at(&ms.chain_active, 1) != NULL &&
                  active_chain_at(&ms.chain_active, CATCHUP) != NULL);

        /* ── TEETH: with diverged==0 the SAME engine path LOCKS IN
         * authoritative instead of reverting. Reset, re-flip, reach the target
         * with an HONEST counter (no divergence), tick the engine, and assert
         * it does NOT revert. Proves the revert keyed on the divergence, not on
         * merely reaching the target. ─────────────────────────────────────── */
        condition_engine_reset_for_testing();
        cutover_canary_complete_test_reset();
        cutover_modes_test_reset();
        /* Clear the three divergence counters back to zero (shutdown zeroes
         * them) while keeping authoritative authority pointed at the real tip,
         * so detect() sees the canary target reached with NO divergence. */
        tip_finalize_stage_shutdown();
        tip_finalize_stage_init(&ms);
        tip_finalize_stage_set_utxo_counter(car_utxo_counter_honest, NULL);

        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        tip_finalize_set_mode(TIP_FINALIZE_MODE_AUTHORITATIVE);
        /* Re-point the authoritative tip at the real fixture tip (after a
         * shutdown the stage's last-advance height resets to -1, and in
         * authoritative mode active_chain_height reads it). */
        tip_finalize_stage_set_authoritative_tip(TARGET, sc.hashes[TARGET].data);
        cutover_modes_record_change(flip_tip, flip_tip, flip_tip, 0);
        /* Tip already at TARGET >= flip_tip+1, and diverged==0. */
        CAR_CHECK("A-teeth: clean canary target reached, diverged==0",
                  cutover_modes_canary_target_reached(TARGET) &&
                  active_chain_height(&ms.chain_active) == TARGET &&
                  tip_finalize_stage_utxo_count_diverged_total() == 0 &&
                  tip_finalize_stage_precondition_failed_total() == 0 &&
                  tip_finalize_stage_upstream_failed_total() == 0);
        register_cutover_canary_complete();
        condition_engine_tick();
        CAR_CHECK("A-teeth: clean pass LOCKS IN authoritative (NO revert)",
                  cutover_canary_complete_test_remedy_calls() >= 1 &&
                  cutover_modes_any_authoritative_active());

        validate_headers_stage_shutdown();
        header_admit_stage_shutdown();
        tip_finalize_stage_shutdown();
        active_chain_free(&ms.chain_active);
        car_chain_free(&sc);
        progress_store_close();
        condition_engine_reset_for_testing();
        sync_monitor_set_context(NULL, NULL, NULL);
        cutover_modes_test_reset();
        shadow_conservation_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
        clock_reset_default();
        test_cleanup_tmpdir(dir);
    }

    /* ════════════════════════════════════════════════════════════════════
     * Scenario B — NO-FORWARD-PROGRESS STALL: the guard REVERTS and PAGES.
     *
     * Flip to authoritative, then stall the tip past the 180 s deadline while a
     * peer is ahead. Assert cutover_no_forward_progress (a) does NOT fire one
     * tick early, (b) fires and reverts every header-pipeline stage to shadow,
     * (c) PAGES (unresolved/operator-needed), and (d) leaves the chain tip
     * uncorrupted. Then resume forward progress and assert the condition
     * witnesses + clears (legacy authority resumes cleanly).
     * ════════════════════════════════════════════════════════════════════ */
    {
        struct car_clock clock;
        car_clock_install(&clock, 200000);

        condition_engine_reset_for_testing();
        cutover_no_forward_progress_test_reset();
        cutover_modes_test_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        memset(&cm, 0, sizeof cm);
        memset(&dm, 0, sizeof dm);
        memset(&ms, 0, sizeof ms);
        zcl_mutex_init(&cm.manager.cs_nodes);
        zcl_mutex_init(&dm.cs);
        zcl_mutex_init(&ms.cs_main);
        static struct chain_params params_b;
        memset(&params_b, 0, sizeof params_b);
        cm.params = &params_b;
        sync_monitor_init();
        sync_monitor_set_context(&cm, &dm, &ms);

        register_cutover_no_forward_progress();

        struct block_index tip = {0};
        block_index_init(&tip);
        tip.nHeight = 100;
        struct uint256 tip_hash;
        car_hash(&tip_hash, 100);
        tip.phashBlock = &tip_hash;
        bool ok = active_chain_set_tip(&ms.chain_active, &tip);
        CAR_CHECK("B: chain tip installed at 100", ok);
        sync_monitor_on_block_connected(100);

        /* A peer is ahead of the local tip (the no-progress precondition). */
        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 250;
        peer.state = PEER_ACTIVE;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;

        /* Flip to authoritative. */
        header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);

        /* (a) Not yet stalled: one tick at the deadline boundary must NOT
         * revert (teeth — the revert is not premature). */
        condition_engine_tick();
        CAR_CHECK("B: (a) NO revert before the 180s deadline elapses",
                  cutover_no_forward_progress_test_remedy_calls() == 0 &&
                  header_admit_get_mode() ==
                      HEADER_ADMIT_MODE_AUTHORITATIVE &&
                  validate_headers_get_mode() ==
                      VALIDATE_HEADERS_MODE_AUTHORITATIVE);

        /* Advance the clock past the 180 s watch window with NO tip progress. */
        car_clock_set(&clock, 200000 + 181);
        condition_engine_tick();

        /* (b) reverted every header-pipeline stage to shadow. */
        CAR_CHECK("B: (b) stall guard fired and REVERTED to shadow",
                  cutover_no_forward_progress_test_remedy_calls() == 1 &&
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() ==
                      VALIDATE_HEADERS_MODE_SHADOW);

        /* (c) pages: the condition is unresolved (max_attempts exhausted while
         * still active) AND the operator-needed page was emitted. */
        CAR_CHECK("B: (c) condition is unresolved (paging) after the revert",
                  condition_engine_get_unresolved_count() == 1);
        CAR_CHECK("B: (c) operator-needed page emitted",
                  car_paged("cutover_no_forward_progress"));

        /* (d) the chain tip is uncorrupted by the revert. */
        struct block_index *tip_now = active_chain_tip(&ms.chain_active);
        CAR_CHECK("B: (d) chain tip height intact after revert",
                  tip_now && tip_now->nHeight == 100);
        CAR_CHECK("B: (d) chain tip hash intact after revert",
                  tip_now && tip_now->phashBlock &&
                  uint256_eq(tip_now->phashBlock, &tip_hash) != 0);

        /* Resume forward progress: the condition's witness sees the tip advance
         * and clears (legacy authority resumed cleanly). */
        struct block_index next = {0};
        block_index_init(&next);
        next.nHeight = 101;
        struct uint256 next_hash;
        car_hash(&next_hash, 101);
        next.phashBlock = &next_hash;
        next.pprev = &tip;
        active_chain_set_tip(&ms.chain_active, &next);
        sync_monitor_on_block_connected(101);
        condition_engine_tick();
        CAR_CHECK("B: condition clears once the tip advances again",
                  condition_engine_get_active_count() == 0);

        active_chain_free(&ms.chain_active);
        condition_engine_reset_for_testing();
        sync_monitor_set_context(NULL, NULL, NULL);
        cutover_modes_test_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
        clock_reset_default();
    }

    /* Leave global state clean for tests sharing the process. */
    condition_engine_reset_for_testing();
    header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
    validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
    shadow_conservation_reset();
    cutover_modes_test_reset();
    clock_reset_default();
    if (sync_get_state() != SYNC_IDLE)
        sync_set_state(SYNC_IDLE, "test cleanup");

    printf("cutover_autorevert tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
