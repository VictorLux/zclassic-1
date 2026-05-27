/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_cutover_flip_dryrun — exercise the FULL shadow->authoritative flip
 * (docs/work/cutover.md "B7") end-to-end on an EPHEMERAL fixture datadir.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * Before the maintainer performs the real `cutovermode all authoritative`
 * flip against the live node, we want a CI proof that the flip MECHANISM —
 * the gate that refuses an unsafe flip, the pairing requirement, the actual
 * mode transition, the post-flip canary, and the log-derived-tip parity it
 * promises — all work end-to-end, without ever touching the live node or
 * ~/.zclassic-c23. Everything here runs against a throwaway progress.kv in
 * ./test-tmp and a synthetic in-memory active_chain; no service, no socket,
 * no consensus mutation, no live RPC.
 *
 * WHAT IS DRIVEN, AND THE ONE HONEST BOUNDARY
 * -------------------------------------------
 * The flip is gated by `diag_rpc_cutovermode`, which refuses authoritative
 * unless `diag_rpc_cutoverpreflight` reports ready==true. That preflight
 * ANDs seven gates. FIVE of them are fixture-driveable because they read
 * progress.kv / the in-memory chain / the process-global conservation
 * ledger / the cutover mode flags:
 *     - header_admit diff CONVERGED + cursor caught up   (progress.kv)
 *     - validate_headers window clean + cursor caught up (progress.kv)
 *     - shadow-pipeline conservation fed == diffed        (global ledger)
 *     - cutover modes currently shadow                    (mode flags)
 *     - (the tip-finalize-derived tip parity we assert directly)
 *
 * The OTHER TWO gates — `live` (node_health_collect: sync_get_state,
 * connman peer count, live main_state tip recency) and `chain_advance`
 * (chain_advance_coordinator live source selection) — read process-global
 * RUNTIME SINGLETONS that only a booted node populates. On a throwaway
 * fixture they are necessarily NOT ready (no peers, not synced, no live
 * coordinator). So the FULL RPC `ready` boolean CANNOT be true in a unit
 * test, and the real RPC flip is (correctly) REFUSED here.
 *
 * That boundary is itself a load-bearing assertion: Scenario A drives the
 * REAL `cutovermode` RPC and proves it REFUSES the authoritative flip while
 * the live/chain_advance gates are not ready (an unsafe flip is blocked),
 * proves the pairing requirement (header_admit-alone is refused), and proves
 * a shadow REVERT is always accepted. Scenario C proves the conservation
 * gate refuses with a typed blocker.
 *
 * For the post-accept behaviour the live node would exhibit AFTER a real
 * green flip — the mode actually transitions, the canary tracks forward
 * progress, and the log-derived tip equals the legacy tip for blocks that
 * connect after the flip — Scenario B drives the SAME functions the
 * controller calls on an accepted flip (cutover_modes_set_header_pipeline +
 * cutover_modes_record_change) directly against the fixture, then advances
 * blocks through tip_finalize and asserts tip parity + canary health. This
 * is the largest tractable slice: it proves steps 1-6 against a real
 * fixture, with the live-gate boundary documented and itself asserted.
 *
 * Every assertion below has teeth: the comparators are the same ones the
 * positive and negative paths share, and the mandatory negative controls
 * (Scenario A's refusal, Scenario C's conservation blocker, Scenario B's
 * poisoned-parity check) genuinely fail on divergence.
 */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "controllers/diagnostics_controller.h"
#include "json/json.h"
#include "jobs/header_admit_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/validate_headers_stage.h"
#include "rpc/server.h"
#include "services/cutover_modes.h"
#include "storage/progress_store.h"
#include "adapters/inbound/shadow_conservation.h"
#include "util/blocker.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CFD_CHECK(name, expr) do {                  \
    printf("cutover_flip_dryrun: %s... ", (name));  \
    if ((expr)) printf("OK\n");                     \
    else { printf("FAIL\n"); failures++; }          \
} while (0)

/* ── Synthetic chain (mirrors test_cutover_tip_parity / tip_finalize) ──── */

struct cfd_chain {
    struct block_index *blocks;
    struct uint256     *hashes;
    int n;
};

static int cfd_mkdir(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void cfd_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/cutover_flip_dryrun_%d_%s",
             (int)getpid(), tag);
}

static void cfd_hash(struct uint256 *out, int h, uint8_t fork_tag)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0xa0 + h);
    out->data[1] = fork_tag;
    out->data[2] = 0xC3;  /* match sibling fixtures' "C3" marker byte */
}

/* Build a coherent chain of n block_index entries (heights 0..n-1) with full
 * header fields so header_admit/validate_headers/tip_finalize can chew it. */
static bool cfd_chain_build(struct cfd_chain *sc, int n, uint8_t fork_tag)
{
    sc->blocks = calloc((size_t)n, sizeof(struct block_index));
    sc->hashes = calloc((size_t)n, sizeof(struct uint256));
    if (!sc->blocks || !sc->hashes) return false;
    for (int i = 0; i < n; i++) {
        cfd_hash(&sc->hashes[i], i, fork_tag);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;
        sc->blocks[i].nTime = (uint32_t)(1700006000u + (uint32_t)i);
        sc->blocks[i].nBits = 0x1f07ffff;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        arith_uint256_set_u64(&sc->blocks[i].nChainWork, (uint64_t)i + 1);
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void cfd_chain_free(struct cfd_chain *sc)
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

static bool cfd_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);  // raw-sql-ok:test-direct
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed utxo_apply_log with `n` all-passing rows + the upstream cursor, so
 * tip_finalize has `n` finalizable heights — same feeder as the tip-parity
 * test (we deliberately reuse this rather than inventing a new one). */
static bool cfd_seed_utxo_apply(sqlite3 *db, int n)
{
    if (!cfd_exec(db,
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
    for (int h = 0; h < n; h++) {
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
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return ok;
}

/* Bump the utxo_apply upstream cursor so tip_finalize may finalize `n`
 * heights (used to "feed more blocks" after the flip). Rows 0..n-1 already
 * exist; this just exposes more of them to the finalize stage. */
static bool cfd_bump_utxo_apply_cursor(sqlite3 *db, int n)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('utxo_apply', ?, 1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return ok;
}

/* Read the tip_hash the reducer recorded for finalize-row `height` (that row
 * describes the tip at logical height `height+1`). */
static bool cfd_log_tip_hash(sqlite3 *db, int height, struct uint256 *out)
{
    uint256_set_null(out);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT tip_hash FROM tip_finalize_log WHERE height = ? AND ok = 1",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:test-direct
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob && n == 32) { memcpy(out->data, blob, 32); found = true; }
    }
    sqlite3_finalize(st);
    return found;
}

/* The single load-bearing tip comparator (same shape as the tip-parity
 * test): true iff the log-derived tip at logical height `tip_height` equals
 * the legacy active_chain tip there. Both the positive replay and the
 * negative control call THIS, so green-positive + red-negative proves teeth. */
static bool cfd_parity_holds_at(sqlite3 *db, const struct active_chain *chain,
                                int tip_height)
{
    if (tip_height < 1) return false;
    struct block_index *legacy = active_chain_at(chain, tip_height);
    if (!legacy || !legacy->phashBlock) return false;
    struct uint256 derived;
    if (!cfd_log_tip_hash(db, tip_height - 1, &derived)) return false;
    return uint256_eq(&derived, legacy->phashBlock) != 0;
}

/* Negative-control poison: overwrite a recorded tip_hash so it no longer
 * matches chain_active. Used ONLY to prove the comparator detects divergence. */
static bool cfd_poison_tip_hash(sqlite3 *db, int finalize_height)
{
    struct uint256 bogus;
    uint256_set_null(&bogus);
    bogus.data[0] = 0xde; bogus.data[1] = 0xad;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "UPDATE tip_finalize_log SET tip_hash = ? WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_blob(st, 1, bogus.data, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, finalize_height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return ok;
}

/* Stub header validator that always passes (no PoW/Equihash in-fixture). */
static bool cfd_stub_pass(const struct block_index *bi, const char *datadir,
                          char *out_reason, size_t out_reason_size,
                          void *user)
{
    (void)bi; (void)datadir; (void)user;
    if (out_reason && out_reason_size) out_reason[0] = 0;
    return true;
}

/* ── Helpers that read the REAL preflight RPC and pull a sub-gate out ────── */

/* Run cutoverpreflight and return whether the named string blocker is present
 * in the blockers array. Also outputs the overall `ready` boolean. */
static bool cfd_preflight(const struct rpc_command *cmd, bool *ready_out,
                          const char *want_blocker, bool *blocker_present)
{
    struct json_value params, result;
    json_init(&params);
    json_init(&result);
    json_set_array(&params);
    bool ok = cmd && cmd->actor(&params, false, &result);
    if (ready_out) {
        const struct json_value *r = json_get(&result, "ready");
        *ready_out = r ? json_get_bool(r) : false;
    }
    if (blocker_present) {
        *blocker_present = false;
        const struct json_value *bl = json_get(&result, "blockers");
        if (want_blocker && bl && bl->type == JSON_ARR) {
            size_t n = json_size(bl);
            for (size_t i = 0; i < n; i++) {
                const struct json_value *b = json_at(bl, i);
                const char *s = b ? json_get_str(b) : NULL;
                if (s && strstr(s, want_blocker)) { *blocker_present = true; break; }
            }
        }
    }
    json_free(&params);
    json_free(&result);
    return ok;
}

/* Read a single named sub-gate's `ready`/`ok` boolean from the preflight
 * result (e.g. conservation.ok, header_admit_diff converged via status). */
static bool cfd_preflight_subgate(const struct rpc_command *cmd,
                                  const char *section, const char *key,
                                  bool *out_value)
{
    struct json_value params, result;
    json_init(&params);
    json_init(&result);
    json_set_array(&params);
    bool ok = cmd && cmd->actor(&params, false, &result);
    *out_value = false;
    if (ok) {
        const struct json_value *sec = json_get(&result, section);
        const struct json_value *v = sec ? json_get(sec, key) : NULL;
        if (v) *out_value = json_get_bool(v);
    }
    json_free(&params);
    json_free(&result);
    return ok;
}

int test_cutover_flip_dryrun(void);
int test_cutover_flip_dryrun(void)
{
    printf("\n=== cutover_flip_dryrun tests ===\n");
    int failures = 0;

    blocker_module_init();

    struct rpc_table rt;
    rpc_table_init(&rt);
    register_diagnostics_rpc_commands(&rt);
    const struct rpc_command *mode_cmd = rpc_table_find(&rt, "cutovermode");
    const struct rpc_command *pre_cmd = rpc_table_find(&rt, "cutoverpreflight");
    CFD_CHECK("RPC commands registered", mode_cmd && pre_cmd);

    /* ════════════════════════════════════════════════════════════════════
     * Scenario A — the REAL flip GATE via the cutovermode RPC.
     *
     * Proves the flip MECHANISM's refusal logic end-to-end against the live
     * controller: (1) header_admit-alone authoritative is REFUSED (pairing
     * requirement), (2) `all authoritative` is REFUSED because the live /
     * chain_advance preflight gates are not ready on a fixture (an unsafe
     * flip is blocked — the load-bearing safety property), (3) a shadow
     * revert is always ACCEPTED. After every refusal the modes stay SHADOW.
     * ════════════════════════════════════════════════════════════════════ */
    {
        /* Clean starting state: modes shadow, conservation ledger clean. */
        cutover_modes_test_reset();
        shadow_conservation_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        struct json_value params, result, v;
        json_init(&params);
        json_init(&result);
        json_init(&v);

        /* (1) Pairing requirement: header_admit alone -> authoritative is
         * refused; modes must remain shadow. */
        json_set_array(&params);
        json_set_str(&v, "header_admit");
        json_push_back(&params, &v);
        json_set_str(&v, "authoritative");
        json_push_back(&params, &v);
        bool refused_unpaired = mode_cmd && !mode_cmd->actor(&params, false, &result);
        CFD_CHECK("A: unpaired header_admit authoritative is REFUSED",
                  refused_unpaired);
        CFD_CHECK("A: modes stay shadow after unpaired refusal",
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() == VALIDATE_HEADERS_MODE_SHADOW);
        json_free(&result);

        /* (2) Paired `all authoritative` is REFUSED on a fixture: the live
         * and chain_advance preflight gates cannot be ready without a booted
         * node, so cutover_preflight_ready_now() is false and the flip is
         * blocked. This is the unsafe-flip-is-refused safety property. */
        bool ready = true;
        bool live_blk = false, ca_blk = false;
        cfd_preflight(pre_cmd, &ready, "live_health_not_ready", &live_blk);
        CFD_CHECK("A: fixture preflight ready==false (no live node)", !ready);
        CFD_CHECK("A: live_health gate names itself as a blocker", live_blk);
        cfd_preflight(pre_cmd, NULL, "chain_advance_not_ready", &ca_blk);
        CFD_CHECK("A: chain_advance gate names itself as a blocker", ca_blk);

        json_set_array(&params);
        json_set_str(&v, "all");
        json_push_back(&params, &v);
        json_set_str(&v, "authoritative");
        json_push_back(&params, &v);
        bool refused_paired = mode_cmd && !mode_cmd->actor(&params, false, &result);
        CFD_CHECK("A: paired authoritative REFUSED while preflight not ready",
                  refused_paired);
        CFD_CHECK("A: modes stay shadow after refused flip",
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() == VALIDATE_HEADERS_MODE_SHADOW);
        CFD_CHECK("A: no authoritative active after refused flip",
                  !cutover_modes_any_authoritative_active());
        json_free(&result);

        /* (3) Shadow revert is always accepted (no preflight gate on a
         * revert — the safe direction). First force modes to a non-shadow
         * value via the underlying setter, then revert via the RPC. */
        header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);
        json_set_array(&params);
        json_set_str(&v, "all");
        json_push_back(&params, &v);
        json_set_str(&v, "shadow");
        json_push_back(&params, &v);
        bool revert_ok = mode_cmd && mode_cmd->actor(&params, false, &result);
        CFD_CHECK("A: shadow revert is ACCEPTED", revert_ok);
        CFD_CHECK("A: modes are shadow after revert",
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() == VALIDATE_HEADERS_MODE_SHADOW);
        json_free(&result);

        json_free(&v);
        json_free(&params);
        cutover_modes_test_reset();
    }

    /* ════════════════════════════════════════════════════════════════════
     * Scenario B — the full flip EFFECT on an ephemeral fixture.
     *
     * Drives the shadow pipeline to catch-up on a deterministic chain, asserts
     * the FIXTURE-DRIVEABLE preflight gates are green (header_admit diff
     * CONVERGED, validate window clean, conservation fed==diffed, modes
     * shadow), then performs the programmatic equivalent of an ACCEPTED
     * `cutovermode all authoritative` — exactly what the controller calls on
     * an accepted flip: cutover_modes_set_header_pipeline(AUTH,AUTH) +
     * cutover_modes_record_change(). It then advances N more blocks through
     * tip_finalize and asserts the log-derived tip equals the legacy tip for
     * each new height, and that the no-progress canary stays HEALTHY
     * (does not fire FAILED) while blocks advance and turns PASSED on reach.
     * ════════════════════════════════════════════════════════════════════ */
    {
        char dir[256];
        cfd_tmpdir(dir, sizeof dir, "effect");
        cfd_mkdir("./test-tmp");
        cfd_mkdir(dir);

        const int CATCHUP = 5;      /* blocks fed before the flip   */
        const int POSTFLIP = 4;     /* blocks fed after the flip    */
        const int TOTAL = CATCHUP + POSTFLIP;  /* heights 1..TOTAL tips */

        cutover_modes_test_reset();
        shadow_conservation_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        struct main_state ms;
        struct cfd_chain sc;
        memset(&ms, 0, sizeof ms);

        CFD_CHECK("B: progress_store opens", progress_store_open(dir));
        active_chain_init(&ms.chain_active);
        /* Full chain has TOTAL+1 blocks (0..TOTAL); tips exist at 1..TOTAL. */
        CFD_CHECK("B: chain builds", cfd_chain_build(&sc, TOTAL + 1, 0x00));

        sqlite3 *db = progress_store_db();

        /* ── Step 1+2: run the shadow pipeline to CATCH UP to CATCHUP. ──── */
        /* Expose only the first CATCHUP heights to the chain + finalize. */
        active_chain_set_tip(&ms.chain_active, &sc.blocks[CATCHUP]);
        CFD_CHECK("B: chain height == CATCHUP",
                  active_chain_height(&ms.chain_active) == CATCHUP);
        CFD_CHECK("B: utxo_apply seeded (TOTAL rows)",
                  cfd_seed_utxo_apply(db, TOTAL));
        /* Only allow finalize up to CATCHUP for now. */
        CFD_CHECK("B: finalize cursor capped at CATCHUP pre-flip",
                  cfd_bump_utxo_apply_cursor(db, CATCHUP));

        CFD_CHECK("B: header_admit init", header_admit_stage_init(&ms));
        CFD_CHECK("B: validate_headers init", validate_headers_stage_init(&ms));
        validate_headers_stage_set_validator(cfd_stub_pass, NULL);
        CFD_CHECK("B: tip_finalize init", tip_finalize_stage_init(&ms));

        /* Drain the three shadow stages to catch up. header_admit admits
         * heights 0..CATCHUP inclusive (genesis through the tip), i.e.
         * CATCHUP+1 entries; tip_finalize finalizes heights 0..CATCHUP-1,
         * i.e. CATCHUP entries (its cursor lands at the tip height). */
        CFD_CHECK("B: header_admit drains CATCHUP+1 (genesis..tip)",
                  header_admit_stage_drain(100) == CATCHUP + 1);
        CFD_CHECK("B: validate_headers drains (>=1 step)",
                  validate_headers_stage_drain(100) >= 1);
        CFD_CHECK("B: tip_finalize drains CATCHUP",
                  tip_finalize_stage_drain(100) == CATCHUP);

        /* Mirror the live diff route: every fed block was diffed (conservation
         * fed==diffed). Record the same count the shadow pipeline would. */
        shadow_conservation_record_fed((unsigned long)CATCHUP);
        shadow_conservation_record_diffed((unsigned long)CATCHUP);

        /* Pre-flip tip parity at every caught-up height. */
        bool pre_parity = true;
        for (int th = 1; th <= CATCHUP; th++)
            if (!cfd_parity_holds_at(db, &ms.chain_active, th)) pre_parity = false;
        CFD_CHECK("B: pre-flip tip parity holds at all caught-up heights",
                  pre_parity);
        CFD_CHECK("B: tip_finalize cursor == CATCHUP (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() == (int64_t)CATCHUP);

        /* ── Step 3: assert the FIXTURE-DRIVEABLE preflight gates are green.
         * (Header-admit CONVERGED diff, conservation ok, modes shadow.) The
         * live/chain_advance gates remain not-ready — documented boundary. */
        bool cons_ok = false;
        cfd_preflight_subgate(pre_cmd, "conservation", "ok", &cons_ok);
        CFD_CHECK("B: preflight conservation gate ok (fed==diffed)", cons_ok);

        unsigned long fed = 0, diffed = 0, skipped = 0;
        shadow_conservation_snapshot(&fed, &diffed, &skipped);
        CFD_CHECK("B: conservation snapshot fed==diffed==CATCHUP",
                  fed == (unsigned long)CATCHUP &&
                  diffed == (unsigned long)CATCHUP);

        /* The header_admit diff over the caught-up range must be CONVERGED
         * (the shadow log matches the active chain). Read it via the diff API. */
        {
            struct header_admit_diff_report rep;
            bool diff_ok = header_admit_stage_diff(-1, -1, &rep);
            CFD_CHECK("B: header_admit diff query succeeds", diff_ok);
            CFD_CHECK("B: header_admit diff is CONVERGED (no divergence)",
                      diff_ok && rep.status == HEADER_ADMIT_DIFF_CONVERGED &&
                      rep.mismatch_count == 0 &&
                      rep.missing_in_chain_count == 0);
        }

        /* ── Step 4: perform the FLIP exactly as the accepted controller path
         * does (cutover_modes_set_header_pipeline + record_change). On a real
         * node this is reached only after preflight ready==true; here we drive
         * it directly because the live/chain_advance gates can't be green in a
         * unit fixture (documented boundary). Assert it is ACCEPTED. */
        int64_t flip_tip = active_chain_height(&ms.chain_active);  /* == CATCHUP */
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        cutover_modes_record_change(flip_tip, /*header*/ flip_tip,
                                    /*peer_best*/ flip_tip, /*tip_lag*/ 0);
        CFD_CHECK("B: flip accepted -> authoritative_active true",
                  cutover_modes_any_authoritative_active());
        CFD_CHECK("B: both stages report authoritative after flip",
                  cutover_modes_get_header_admit() ==
                      CUTOVER_STAGE_MODE_AUTHORITATIVE &&
                  cutover_modes_get_validate_headers() ==
                      CUTOVER_STAGE_MODE_AUTHORITATIVE);

        /* Canary target is flip_tip + 1; immediately after the flip the tip is
         * still at flip_tip, so the canary is PENDING (not yet passed) and must
         * NOT have FAILED (the watch window has not elapsed). */
        {
            struct cutover_canary_snapshot snap;
            cutover_modes_canary_snapshot(flip_tip, &snap);
            CFD_CHECK("B: canary has_change + authoritative_active after flip",
                      snap.has_change && snap.authoritative_active);
            CFD_CHECK("B: canary target == flip_tip + 1",
                      snap.target_height == flip_tip + 1);
            CFD_CHECK("B: canary NOT passed immediately (tip < target)",
                      !snap.passed);
            CFD_CHECK("B: canary NOT failed immediately (window open)",
                      !snap.failed);
        }

        /* ── Step 5: advance POSTFLIP more blocks; assert log-derived tip ==
         * legacy tip for each new height, and the canary stays healthy. ──── */
        bool post_parity = true;
        bool canary_never_failed = true;
        for (int i = 1; i <= POSTFLIP; i++) {
            int new_height = CATCHUP + i;
            /* Extend the legacy chain by one block and expose one more
             * finalizable height to tip_finalize. */
            active_chain_set_tip(&ms.chain_active, &sc.blocks[new_height]);
            CFD_CHECK("B: chain advanced one block after flip",
                      active_chain_height(&ms.chain_active) == new_height);
            if (!cfd_bump_utxo_apply_cursor(db, new_height)) {
                post_parity = false;
                break;
            }
            /* Drive the reducer's terminal stage one block forward. */
            job_result_t r = tip_finalize_stage_step_once();
            if (r != JOB_ADVANCED) { post_parity = false; break; }
            /* Conservation: account the one block fed+diffed this round. */
            shadow_conservation_record_fed(1);
            shadow_conservation_record_diffed(1);

            /* Tip parity at the new height (log-derived == legacy). */
            if (!cfd_parity_holds_at(db, &ms.chain_active, new_height))
                post_parity = false;
            /* Cursor tracks the tip with zero lag. */
            if ((int64_t)tip_finalize_stage_cursor() != (int64_t)new_height)
                post_parity = false;

            /* Canary: as the tip advances it must never read FAILED (the
             * flip is healthy: blocks ARE advancing within the window). */
            struct cutover_canary_snapshot snap;
            cutover_modes_canary_snapshot(new_height, &snap);
            if (snap.failed) canary_never_failed = false;
        }
        CFD_CHECK("B: post-flip tip parity holds at every new height",
                  post_parity);
        CFD_CHECK("B: no-progress canary NEVER fired FAILED while advancing",
                  canary_never_failed);

        /* The very first post-flip block reaches the canary target
         * (flip_tip + 1): the canary must now report PASSED — a healthy,
         * confirmed flip. */
        CFD_CHECK("B: canary PASSED once tip reached target",
                  cutover_modes_canary_target_reached(CATCHUP + 1) &&
                  cutover_modes_canary_target_reached(TOTAL));

        /* ── Step 6 (teeth): negative control — poison one finalized tip hash
         * and prove the SAME parity comparator now reports divergence. A
         * vacuous self-comparison could never fail this. ──────────────── */
        CFD_CHECK("B: negative-control poison written",
                  cfd_poison_tip_hash(db, /*finalize_height=*/CATCHUP + 1));
        CFD_CHECK("B: negative control — comparator DETECTS divergence",
                  cfd_parity_holds_at(db, &ms.chain_active,
                                      /*tip_height=*/CATCHUP + 2) == false);
        /* …and an unpoisoned height still passes (poison is local, comparator
         * is not stuck-FALSE). */
        CFD_CHECK("B: an unpoisoned height still passes",
                  cfd_parity_holds_at(db, &ms.chain_active, /*tip_height=*/2));

        validate_headers_stage_shutdown();
        header_admit_stage_shutdown();
        tip_finalize_stage_shutdown();
        active_chain_free(&ms.chain_active);
        cfd_chain_free(&sc);
        progress_store_close();
        cutover_modes_test_reset();
        shadow_conservation_reset();
        test_cleanup_tmpdir(dir);
    }

    /* ════════════════════════════════════════════════════════════════════
     * Scenario C — MANDATORY negative control: an unsafe flip is REFUSED.
     *
     * Force the shadow-pipeline conservation gate to FAIL (a silently dropped
     * block: fed=5, diffed=4) via the public test seam, then drive the REAL
     * cutoverpreflight RPC and assert ready==false WITH a typed blocker that
     * names the drop. Then drive the REAL cutovermode RPC and assert the
     * authoritative flip is REFUSED and the modes stay shadow. This proves the
     * gate actually blocks an unsafe flip (the whole point of the dry run).
     * ════════════════════════════════════════════════════════════════════ */
    {
        cutover_modes_test_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        /* Forge a silent drop. */
        shadow_conservation_reset();
        shadow_conservation_record_fed(5);
        shadow_conservation_record_diffed(4);

        /* Preflight must refuse with a typed, self-explaining blocker. */
        bool ready = true, cons_blk = false, cons_ok = true;
        cfd_preflight(pre_cmd, &ready, "shadow_pipeline_dropped_blocks",
                      &cons_blk);
        cfd_preflight_subgate(pre_cmd, "conservation", "ok", &cons_ok);
        CFD_CHECK("C: conservation gate reports NOT ok on a drop", !cons_ok);
        CFD_CHECK("C: preflight ready==false on a dropped block", !ready);
        CFD_CHECK("C: typed conservation blocker present (self-explaining)",
                  cons_blk);

        /* The actual flip via the RPC must be REFUSED, modes stay shadow. */
        struct json_value params, result, v;
        json_init(&params);
        json_init(&result);
        json_init(&v);
        json_set_array(&params);
        json_set_str(&v, "all");
        json_push_back(&params, &v);
        json_set_str(&v, "authoritative");
        json_push_back(&params, &v);
        bool refused = mode_cmd && !mode_cmd->actor(&params, false, &result);
        CFD_CHECK("C: authoritative flip REFUSED on unsafe (dropped) pipeline",
                  refused);
        CFD_CHECK("C: modes stay shadow after refused unsafe flip",
                  header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW &&
                  validate_headers_get_mode() == VALIDATE_HEADERS_MODE_SHADOW &&
                  !cutover_modes_any_authoritative_active());
        json_free(&v);
        json_free(&params);
        json_free(&result);

        shadow_conservation_reset();
        cutover_modes_test_reset();
    }

    /* Leave global state clean for tests sharing the process. */
    header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
    validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
    shadow_conservation_reset();
    cutover_modes_test_reset();
    /* rpc_table is a fixed-size stack struct (see rpc/server.h) — no free. */

    printf("cutover_flip_dryrun tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
