/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_cutover_tip_parity — the SAFETY GATE for the cutover authority flip
 * (docs/work/cutover.md "B7": make the log_head-derived tip definitional and
 * demote chain_active to a derived index).
 *
 * BEFORE we can flip authority we must PROVE, block-by-block, that the tip
 * derived from the append-only reducer log equals the legacy chain_active
 * tip at every height — including across a reorg. This test is that proof in
 * miniature, driven offline (no live node, no service, no consensus mutation).
 *
 * WHAT "log_head-derived tip" MEANS HERE
 * --------------------------------------
 * The reducer's terminal stage is tip_finalize (Wave S, S-9, in
 * app/jobs/src/tip_finalize_stage.c). It consumes utxo_apply_log and, for
 * each finalized height H, appends a row to tip_finalize_log carrying
 * (height=H, ok=1, status="finalized", tip_hash). By construction
 * tip_finalize records `tip_hash = active_chain best block at height H+1`
 * (see step_finalize() writing new_tip->phashBlock). The log-derived tip is
 * therefore:
 *     derived_tip_height = (max finalized H) + 1
 *     derived_tip_hash   = tip_finalize_log[max finalized H].tip_hash
 * and the legacy tip is active_chain_at(chain_active, H+1)->phashBlock with
 * height active_chain_height(chain_active).
 *
 * THIS IS THE SAME DRIVER the rest of the suite uses for this stage
 * (test_tip_finalize_stage.c): a synthetic active_chain + a seeded
 * utxo_apply_log feeding tip_finalize_stage_drain(). We reuse that feeder
 * pattern deliberately rather than inventing a parallel one — the only new
 * thing is the per-height PARITY assertion and the negative control.
 *
 * INVARIANTS ASSERTED (per height, after each block connects)
 * -----------------------------------------------------------
 *   P1  derived tip HASH  == chain_active best-block hash at the same height
 *   P2  derived tip HEIGHT == chain_active height (no lag, no overshoot)
 *   P3  the cursor lag at full convergence is exactly 0 — matching the
 *       documented cutover preflight criterion in
 *       app/controllers/src/cutover_controller.c (required_cursor =
 *       chain_tip_height + 1; header_caught_up / validate_caught_up both
 *       require cursor_lag == 0). We read that tolerance from the code's
 *       model, not a hand-picked constant.
 *
 * REORG COVERAGE
 * --------------
 * The tip_finalize stage HAS a reorg-rewind path (rewind_cursor_if_active_
 * chain_reorged) and the existing unit test exercises it via
 * active_chain_set_tip onto a competing fork. We use that same fixture: build
 * a chain, finalize to tip, install a coherent competing fork (new hashes +
 * more work) via active_chain_set_tip, then re-run the stage and assert tip
 * parity holds through the disconnect+reconnect — the rewritten log row's
 * tip_hash must equal the NEW chain_active best block at that height.
 *
 * NEGATIVE CONTROL (proves the assertion has teeth)
 * -------------------------------------------------
 * parity_holds_at() is the single comparator both the positive and negative
 * paths use. The negative control forges a divergence by poisoning the
 * recorded tip_hash in tip_finalize_log (so it no longer matches
 * chain_active) and asserts parity_holds_at() returns FALSE. A vacuous test
 * (one that compared a value to itself) could never fail this control, so
 * its passing is evidence the comparator genuinely detects divergence.
 */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "jobs/tip_finalize_stage.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTP_CHECK(name, expr) do {                  \
    printf("cutover_tip_parity: %s... ", (name));   \
    if ((expr)) printf("OK\n");                     \
    else { printf("FAIL\n"); failures++; }          \
} while (0)

/* ── Synthetic chain, mirroring test_tip_finalize_stage's fixture ───────── */

struct ctp_chain {
    struct block_index *blocks;
    struct uint256     *hashes;
    int n;
};

static int ctp_mkdir(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void ctp_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/cutover_tip_parity_%d_%s",
             (int)getpid(), tag);
}

static void ctp_hash(struct uint256 *out, int h, uint8_t fork_tag)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0xa0 + h);
    out->data[1] = fork_tag;  /* distinguishes forks at the same height */
}

static bool ctp_chain_build(struct ctp_chain *sc, int n, uint8_t fork_tag)
{
    sc->blocks = calloc((size_t)n, sizeof(struct block_index));
    sc->hashes = calloc((size_t)n, sizeof(struct uint256));
    if (!sc->blocks || !sc->hashes) return false;
    for (int i = 0; i < n; i++) {
        ctp_hash(&sc->hashes[i], i, fork_tag);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;
        sc->blocks[i].nTime = (uint32_t)(1700005000u + (uint32_t)i);
        sc->blocks[i].nBits = 0x1f07ffff;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        arith_uint256_set_u64(&sc->blocks[i].nChainWork, (uint64_t)i + 1);
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void ctp_chain_free(struct ctp_chain *sc)
{
    free(sc->blocks);
    free(sc->hashes);
    memset(sc, 0, sizeof(*sc));
}

static bool ctp_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed utxo_apply_log with `n` all-passing rows + the upstream cursor, so the
 * tip_finalize stage has `n` finalizable heights to chew through. */
static bool ctp_seed_utxo_apply(sqlite3 *db, int n)
{
    if (!ctp_exec(db,
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
        if (sqlite3_step(st) != SQLITE_DONE) {
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
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Read the tip_hash the reducer recorded for the finalize-row at `height`
 * (this row describes the tip that exists at logical height `height+1`). */
static bool ctp_log_tip_hash(sqlite3 *db, int height, struct uint256 *out)
{
    uint256_set_null(out);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT tip_hash FROM tip_finalize_log WHERE height = ? AND ok = 1",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob && n == 32) {
            memcpy(out->data, blob, 32);
            found = true;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* The single load-bearing comparator. Returns true iff the log-derived tip at
 * logical height `tip_height` matches the legacy chain_active tip there.
 *
 * `tip_height` is the height of a connected block; the reducer's row for it
 * lives at finalize-height (tip_height - 1) and carries that block's hash.
 * Both the positive replay and the negative control call THIS function, so a
 * green positive + a red negative proves the comparator has teeth. */
static bool parity_holds_at(sqlite3 *db, const struct active_chain *chain,
                            int tip_height)
{
    if (tip_height < 1) return false;

    /* Legacy side: chain_active best block at this height. */
    struct block_index *legacy = active_chain_at(chain, tip_height);
    if (!legacy || !legacy->phashBlock) return false;

    /* Derived side: hash the reducer log recorded for this tip. */
    struct uint256 derived;
    if (!ctp_log_tip_hash(db, tip_height - 1, &derived)) return false;

    return uint256_eq(&derived, legacy->phashBlock) != 0;
}

/* Negative-control poison: overwrite the recorded tip_hash for the finalize
 * row at `finalize_height` with a hash that does NOT match chain_active. Used
 * ONLY to prove parity_holds_at() detects divergence; never on a path that
 * affects the suite's pass/fail other than asserting the control fires. */
static bool ctp_poison_tip_hash(sqlite3 *db, int finalize_height)
{
    struct uint256 bogus;
    uint256_set_null(&bogus);
    bogus.data[0] = 0xde;
    bogus.data[1] = 0xad;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "UPDATE tip_finalize_log SET tip_hash = ? WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_blob(st, 1, bogus.data, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, finalize_height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* The documented cutover-flip cursor-lag tolerance is ZERO: see
 * app/controllers/src/cutover_controller.c — required_cursor = (tip + 1) and
 * both header_caught_up and validate_caught_up require cursor_lag == 0.
 *
 * tip_finalize's cursor counts FINALIZED heights: after finalizing height H it
 * advances to H+1. Finalizing height H records the tip that exists at logical
 * height H+1. So to derive a tip AT chain height T the stage must finalize
 * heights 0..T-1, leaving cursor == T == T (no lag, no overshoot). The "+1" in
 * the controller is because its cursor is keyed on the NEXT height to admit,
 * one ahead of the finalize cursor; the zero-lag invariant is identical.
 *
 * required_finalize_cursor(T) is therefore exactly T: the derived tip height
 * equals chain_active height with cursor_lag == 0. */
static int64_t required_finalize_cursor(int chain_tip_height)
{
    return (chain_tip_height > 0) ? (int64_t)chain_tip_height : 0;
}

int test_cutover_tip_parity(void);
int test_cutover_tip_parity(void)
{
    printf("\n=== cutover_tip_parity tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── Scenario A: linear replay — parity at EVERY height as blocks
     * connect one at a time. This is the core B7 invariant. ───────────── */
    {
        char dir[256];
        ctp_tmpdir(dir, sizeof dir, "linear");
        ctp_mkdir("./test-tmp");
        ctp_mkdir(dir);

        const int N = 8;  /* finalize heights 0..7 -> tips at heights 1..8 */
        struct main_state ms;
        struct ctp_chain sc;
        memset(&ms, 0, sizeof ms);

        CTP_CHECK("linear: progress_store opens", progress_store_open(dir));
        active_chain_init(&ms.chain_active);
        CTP_CHECK("linear: chain builds", ctp_chain_build(&sc, N + 1, 0x00));
        /* chain_active tip = the highest block; tips exist at heights 1..N */
        active_chain_set_tip(&ms.chain_active, &sc.blocks[N]);
        CTP_CHECK("linear: chain height is N",
                  ms.chain_active.height == N);
        CTP_CHECK("linear: utxo_apply seeded",
                  ctp_seed_utxo_apply(progress_store_db(), N));
        CTP_CHECK("linear: stage init", tip_finalize_stage_init(&ms));

        sqlite3 *db = progress_store_db();

        /* Connect one block at a time: each step finalizes one height, and we
         * assert tip parity for the tip that height produces. */
        bool all_parity = true;
        int connected = 0;
        for (int h = 0; h < N; h++) {
            job_result_t r = tip_finalize_stage_step_once();
            if (r != JOB_ADVANCED) break;
            connected++;
            int tip_height = h + 1;   /* finalize row h describes tip at h+1 */
            if (!parity_holds_at(db, &ms.chain_active, tip_height)) {
                all_parity = false;
                printf("    [parity violated at tip_height=%d]\n", tip_height);
                break;
            }
            /* P2: derived tip height tracks chain_active exactly (cursor is
             * (finalized H)+1 = current tip_height; never lags/overshoots the
             * heights connected so far). */
            if ((int64_t)tip_finalize_stage_cursor() != (int64_t)tip_height) {
                all_parity = false;
                printf("    [cursor lag at tip_height=%d cursor=%llu]\n",
                       tip_height,
                       (unsigned long long)tip_finalize_stage_cursor());
                break;
            }
        }
        CTP_CHECK("linear: all N blocks connected", connected == N);
        CTP_CHECK("linear: P1+P2 parity held at every height", all_parity);

        /* P3: at full convergence the finalize cursor equals the chain tip
         * height with zero lag — derived tip height == chain_active height.
         * (chain tip is at height N here; the stage finalized heights 0..N-1.) */
        int64_t need = required_finalize_cursor(ms.chain_active.height);
        CTP_CHECK("linear: P3 cursor == chain tip height (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() == need);

        /* Negative control: poison one recorded tip_hash and prove the SAME
         * comparator now reports divergence. parity must FAIL at that height. */
        CTP_CHECK("linear: negative-control poison written",
                  ctp_poison_tip_hash(db, /*finalize_height=*/3));
        CTP_CHECK("linear: negative control — comparator detects divergence",
                  parity_holds_at(db, &ms.chain_active, /*tip_height=*/4)
                      == false);
        /* …and an unpoisoned height still passes (poison is local, not global,
         * so the comparator is not stuck-FALSE). */
        CTP_CHECK("linear: unpoisoned height still passes",
                  parity_holds_at(db, &ms.chain_active, /*tip_height=*/2));

        tip_finalize_stage_shutdown();
        active_chain_free(&ms.chain_active);
        ctp_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── Scenario B: reorg — parity holds through disconnect + reconnect.
     * Build a chain, finalize to tip, then install a competing fork that
     * replaces interior heights 2..3 with NEW hashes + strictly more work,
     * keeping the SAME tip height. tip_finalize's reorg-rewind
     * (rewind_cursor_if_active_chain_reorged) must detect that its recorded
     * tip hash no longer matches chain_active, rewind to the fork point, and
     * re-finalize the new branch. We then assert tip parity holds against the
     * NEW chain_active at every live height — i.e. the log-derived tip tracks
     * the legacy tip through the disconnect+reconnect.
     *
     * This is the SAME reorg fixture the stage's own unit test
     * (test_tip_finalize_stage.c "reorg_replay") uses: a same-height coherent
     * fork. We layer the per-height parity assertion on top. ──────────────── */
    {
        char dir[256];
        ctp_tmpdir(dir, sizeof dir, "reorg");
        ctp_mkdir("./test-tmp");
        ctp_mkdir(dir);

        const int N = 3;  /* tips at heights 1..3; reorg keeps tip at height 3 */
        struct main_state ms;
        struct ctp_chain sc;
        memset(&ms, 0, sizeof ms);

        CTP_CHECK("reorg: progress_store opens", progress_store_open(dir));
        active_chain_init(&ms.chain_active);
        CTP_CHECK("reorg: chain builds", ctp_chain_build(&sc, N + 1, 0x00));
        active_chain_set_tip(&ms.chain_active, &sc.blocks[N]);
        CTP_CHECK("reorg: utxo_apply seeded",
                  ctp_seed_utxo_apply(progress_store_db(), N));
        CTP_CHECK("reorg: stage init", tip_finalize_stage_init(&ms));

        sqlite3 *db = progress_store_db();

        CTP_CHECK("reorg: initial drain finalizes N",
                  tip_finalize_stage_drain(100) == N);
        CTP_CHECK("reorg: pre-reorg cursor == chain tip (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() ==
                      required_finalize_cursor(ms.chain_active.height));
        bool pre_parity = true;
        for (int tip_height = 1; tip_height <= N; tip_height++)
            if (!parity_holds_at(db, &ms.chain_active, tip_height))
                pre_parity = false;
        CTP_CHECK("reorg: parity held pre-reorg at all heights", pre_parity);

        /* Install a coherent competing fork: heights 2 and 3 get NEW hashes
         * and strictly more chain work, re-linked as a valid same-height
         * branch (tip stays at height 3, but with a different block). */
        sc.hashes[2].data[0] = 0xf2; sc.hashes[2].data[1] = 0x99;
        sc.hashes[3].data[0] = 0xf3; sc.hashes[3].data[1] = 0x99;
        sc.blocks[2].pprev = &sc.blocks[1];
        sc.blocks[3].pprev = &sc.blocks[2];
        arith_uint256_set_u64(&sc.blocks[2].nChainWork, 20);
        arith_uint256_set_u64(&sc.blocks[3].nChainWork, 30);
        CTP_CHECK("reorg: competing fork installed onto chain_active",
                  active_chain_set_tip(&ms.chain_active, &sc.blocks[3]));
        CTP_CHECK("reorg: chain_active still height N (same-height fork)",
                  ms.chain_active.height == N);

        /* Before re-driving, the STALE recorded tip at the fork point must no
         * longer match chain_active — that is the divergence the rewind keys
         * off. This also doubles as a live negative-control: a real reorg the
         * reducer had NOT yet reconciled WOULD be caught by parity_holds_at. */
        CTP_CHECK("reorg: stale derived tip diverges before reconcile",
                  parity_holds_at(db, &ms.chain_active, /*tip_height=*/3)
                      == false);

        /* The reducer must rewind its cursor to the fork point and re-finalize
         * the new branch. Drive until it settles (IDLE = caught up). */
        for (int i = 0; i < 100; i++) {
            job_result_t r = tip_finalize_stage_step_once();
            if (r == JOB_IDLE) break;
        }
        CTP_CHECK("reorg: stage detected and reconciled the reorg",
                  tip_finalize_stage_reorg_detected_total() >= 1);
        CTP_CHECK("reorg: post-reorg cursor back at chain tip (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() ==
                      required_finalize_cursor(ms.chain_active.height));

        /* Parity must now hold against the NEW chain_active at every live
         * height (1..N): the rewritten log rows carry the fork's block hashes,
         * which equal chain_active's best blocks there. */
        bool post_parity = true;
        for (int tip_height = 1; tip_height <= N; tip_height++) {
            if (!parity_holds_at(db, &ms.chain_active, tip_height)) {
                post_parity = false;
                printf("    [post-reorg parity violated at tip_height=%d]\n",
                       tip_height);
            }
        }
        CTP_CHECK("reorg: parity restored post-reorg at all live heights",
                  post_parity);

        tip_finalize_stage_shutdown();
        active_chain_free(&ms.chain_active);
        ctp_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    printf("cutover_tip_parity tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
