/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Wave S S-3 validate_headers stage
 * (app/services/src/validate_headers_stage.c).
 *
 * Coverage:
 *   - init / shutdown round-trip; pool spins up + joins clean
 *   - happy: synthetic chain, header_admit drains fully, validate
 *     drains, log has N pass rows
 *   - batched: > VH_BATCH_SIZE; multi-step advance
 *   - header_admit floor: validate cursor never overruns admit cursor
 *   - injected failure: stub returns false for one height → ok=0 + reason
 *   - replay across progress_store reopen: cursor + log persist
 *   - pre-init guards */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "event/event.h"
#include "primitives/block.h"
#include "services/header_admit_stage.h"
#include "services/validate_headers_stage.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_logic.h"
#include "validation/main_state.h"
#include "validation/process_block.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VH_CHECK(name, expr) do { \
    printf("validate_headers: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int mkdir_p_vh(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static bool vh_stamp_cursor(sqlite3 *db, const char *name, int64_t cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at)"
            " VALUES(?,?,0)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cursor);
    int rc = sqlite3_step(st);  // raw-sql-ok:test-direct
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static void vh_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/validate_headers_%d_%s",
             (int)getpid(), tag);
}

struct synth_chain_vh {
    struct block_index *blocks;
    struct uint256     *hashes;
    int                 n;
};

static bool synth_chain_vh_build(struct synth_chain_vh *sc, int n)
{
    memset(sc, 0, sizeof(*sc));
    sc->blocks = zcl_malloc(
        (size_t)n * sizeof(struct block_index), "vh_blocks");
    if (!sc->blocks) return false;
    sc->hashes = zcl_malloc(
        (size_t)n * sizeof(struct uint256), "vh_hashes");
    if (!sc->hashes) { free(sc->blocks); return false; }
    for (int i = 0; i < n; i++) {
        block_index_init(&sc->blocks[i]);
        memset(&sc->hashes[i], 0, sizeof(struct uint256));
        sc->hashes[i].data[0] = (uint8_t)(i & 0xFF);
        sc->hashes[i].data[1] = (uint8_t)((i >> 8) & 0xFF);
        sc->hashes[i].data[2] = 0xC3;
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;          /* MIN_BLOCK_VERSION */
        sc->blocks[i].nBits = 0x1f07ffff;    /* unused under stub */
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void synth_chain_vh_free(struct synth_chain_vh *sc)
{
    free(sc->blocks);
    free(sc->hashes);
    memset(sc, 0, sizeof(*sc));
}

/* Stub validator that always passes. */
static bool stub_pass(const struct block_index *bi, const char *datadir,
                      char *out_reason, size_t out_reason_size,
                      void *user)
{
    (void)bi; (void)datadir; (void)user;
    if (out_reason && out_reason_size) out_reason[0] = 0;
    return true;
}

/* Stub validator that fails at a configurable height. */
struct fail_at_ctx {
    int fail_height;
    _Atomic int call_count;
};

static bool stub_fail_at(const struct block_index *bi, const char *datadir,
                          char *out_reason, size_t out_reason_size,
                          void *user)
{
    (void)datadir;
    struct fail_at_ctx *c = (struct fail_at_ctx *)user;
    atomic_fetch_add(&c->call_count, 1);
    if (bi && bi->nHeight == c->fail_height) {
        snprintf(out_reason, out_reason_size, "stub-injected-failure");
        return false;
    }
    if (out_reason && out_reason_size) out_reason[0] = 0;
    return true;
}

static int log_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM validate_headers_log",
        -1, &st, NULL) != SQLITE_OK) return -1;
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool log_row_at(sqlite3 *db, int height,
                       int *out_ok, char *out_reason, size_t reason_size)
{
    *out_ok = -1;
    if (out_reason && reason_size) out_reason[0] = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, fail_reason FROM validate_headers_log WHERE height=?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_reason && reason_size)
            snprintf(out_reason, reason_size, "%s", (const char *)txt);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

static void cutover_guard_observer(enum event_type type,
                                   uint32_t peer_id,
                                   const void *payload,
                                   uint32_t payload_len,
                                   void *ctx)
{
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    int *calls = ctx;
    if (type == EV_CUTOVER_GUARD_DIVERGED && calls)
        (*calls)++;
}

/* Build {progress_store + ms + synth_chain + S-2 init + S-3 init w/
 * injected validator}. The caller does the work and then runs the
 * teardown. Returns 0 on success, nonzero on partial failure (still
 * tear down whatever was set up). */
static int vh_setup(const char *tag, int n, vh_validator_fn fn, void *user,
                     char *dir_out, size_t dir_out_size,
                     struct main_state *ms,
                     struct synth_chain_vh *sc)
{
    vh_tmpdir(dir_out, dir_out_size, tag);
    mkdir_p_vh(dir_out);
    if (!progress_store_open(dir_out)) return 1;

    memset(ms, 0, sizeof(*ms));
    active_chain_init(&ms->chain_active);
    if (!synth_chain_vh_build(sc, n)) return 2;
    active_chain_set_tip(&ms->chain_active, &sc->blocks[n - 1]);

    if (!header_admit_stage_init(ms))  return 3;
    if (!validate_headers_stage_init(ms)) return 4;
    if (fn) validate_headers_stage_set_validator(fn, user);
    return 0;
}

static void vh_teardown(const char *dir, struct main_state *ms,
                         struct synth_chain_vh *sc)
{
    validate_headers_stage_shutdown();
    header_admit_stage_shutdown();
    active_chain_free(&ms->chain_active);
    synth_chain_vh_free(sc);
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

int test_validate_headers_stage(void);
int test_validate_headers_stage(void)
{
    printf("\n=== validate_headers_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── cutover mode defaults and input validation ───────────────── */
    {
        VH_CHECK("mode: default is shadow",
                 validate_headers_get_mode() ==
                     VALIDATE_HEADERS_MODE_SHADOW);
        validate_headers_set_mode((validate_headers_mode_t)999);
        VH_CHECK("mode: invalid value maps to shadow",
                 validate_headers_get_mode() ==
                     VALIDATE_HEADERS_MODE_SHADOW);
    }

    /* ── happy: 5 blocks, all pass under stub ──────────────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("happy: setup",
                 vh_setup("happy", 5, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);

        /* Drain S-2 fully so validate can advance. */
        VH_CHECK("happy: header_admit drains 5",
                 header_admit_stage_drain(100) == 5);

        int adv = validate_headers_stage_drain(100);
        VH_CHECK("happy: validate drains 5 (single batched step)",
                 adv == 1);  /* batch=8 covers 5 in one step */
        VH_CHECK("happy: cursor reaches 5",
                 validate_headers_stage_cursor() == 5);
        VH_CHECK("happy: passed_total == 5",
                 validate_headers_stage_passed_total() == 5);
        VH_CHECK("happy: failed_total == 0",
                 validate_headers_stage_failed_total() == 0);

        sqlite3 *db = progress_store_db();
        VH_CHECK("happy: log has 5 rows", log_row_count(db) == 5);
        for (int h = 0; h < 5; h++) {
            int ok = -1;
            log_row_at(db, h, &ok, NULL, 0);
            VH_CHECK("happy: row marks ok=1", ok == 1);
        }
        struct validate_headers_window_report rep;
        VH_CHECK("happy: window report available",
                 validate_headers_stage_window_report(0, 4, &rep));
        VH_CHECK("happy: window report complete",
                 rep.available && rep.complete &&
                 rep.expected_count == 5 && rep.checked_count == 5);
        VH_CHECK("happy: window report has no failures",
                 rep.failed_count == 0 && rep.first_failed_height == -1);

        /* Next step is IDLE — nothing to validate. */
        stage_result_t r = validate_headers_stage_step_once();
        VH_CHECK("happy: next step IDLE", r == STAGE_IDLE);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── batched: 20 blocks → 3 steps (8 + 8 + 4) ──────────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("batched: setup",
                 vh_setup("batched", 20, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        VH_CHECK("batched: header_admit drains 20",
                 header_admit_stage_drain(100) == 20);

        VH_CHECK("batched: step 1 ADVANCED",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("batched: cursor at 8 after step 1",
                 validate_headers_stage_cursor() == VH_BATCH_SIZE);
        VH_CHECK("batched: step 2 ADVANCED",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("batched: cursor at 16 after step 2",
                 validate_headers_stage_cursor() == 2 * VH_BATCH_SIZE);
        VH_CHECK("batched: step 3 ADVANCED (final partial)",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("batched: cursor at 20 after step 3",
                 validate_headers_stage_cursor() == 20);
        VH_CHECK("batched: passed_total == 20",
                 validate_headers_stage_passed_total() == 20);

        sqlite3 *db = progress_store_db();
        VH_CHECK("batched: log has 20 rows", log_row_count(db) == 20);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── header_admit floor: validate stays at or below S-2 cursor ── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("floor: setup",
                 vh_setup("floor", 10, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);

        /* Admit only 3 of 10 → validate can do 3. */
        for (int i = 0; i < 3; i++) {
            stage_result_t r = header_admit_stage_step_once();
            VH_CHECK("floor: admit step advances", r == STAGE_ADVANCED);
        }
        VH_CHECK("floor: admit cursor at 3",
                 header_admit_stage_cursor() == 3);

        VH_CHECK("floor: validate step ADVANCED (partial batch of 3)",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("floor: validate cursor at 3",
                 validate_headers_stage_cursor() == 3);

        /* Next validate step has nothing to do → IDLE. */
        VH_CHECK("floor: next validate step IDLE",
                 validate_headers_stage_step_once() == STAGE_IDLE);

        /* Admit 3 more, validate advances 3 more. */
        for (int i = 0; i < 3; i++)
            header_admit_stage_step_once();
        VH_CHECK("floor: admit cursor now at 6",
                 header_admit_stage_cursor() == 6);
        VH_CHECK("floor: validate ADVANCED again",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("floor: validate cursor at 6",
                 validate_headers_stage_cursor() == 6);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── injected failure: stub returns false at height=3 ──────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        struct fail_at_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.fail_height = 3;
        VH_CHECK("fail: setup",
                 vh_setup("fail", 5, stub_fail_at, &ctx,
                          dir, sizeof(dir), &ms, &sc) == 0);
        header_admit_stage_drain(100);

        VH_CHECK("fail: validate drains 1 (batched step)",
                 validate_headers_stage_drain(10) == 1);
        VH_CHECK("fail: cursor at 5 (failure does not block advance)",
                 validate_headers_stage_cursor() == 5);
        VH_CHECK("fail: passed_total == 4",
                 validate_headers_stage_passed_total() == 4);
        VH_CHECK("fail: failed_total == 1",
                 validate_headers_stage_failed_total() == 1);
        VH_CHECK("fail: every height was checked",
                 atomic_load(&ctx.call_count) == 5);

        sqlite3 *db = progress_store_db();
        int ok = -1;
        char reason[64] = {0};
        VH_CHECK("fail: row at h=3 exists",
                 log_row_at(db, 3, &ok, reason, sizeof(reason)));
        VH_CHECK("fail: row at h=3 has ok=0", ok == 0);
        VH_CHECK("fail: row at h=3 reason matches",
                 strcmp(reason, "stub-injected-failure") == 0);

        int ok0 = -1;
        log_row_at(db, 0, &ok0, NULL, 0);
        VH_CHECK("fail: row at h=0 has ok=1", ok0 == 1);
        struct validate_headers_window_report rep;
        VH_CHECK("fail: window report available",
                 validate_headers_stage_window_report(0, 4, &rep));
        VH_CHECK("fail: window report counts failure",
                 rep.complete && rep.failed_count == 1 &&
                 rep.first_failed_height == 3);
        VH_CHECK("fail: window report carries reason",
                 strcmp(rep.first_fail_reason,
                        "stub-injected-failure") == 0);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── replay across reopen: cursor + log persist ────────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("replay: setup",
                 vh_setup("replay", 7, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        header_admit_stage_drain(100);
        VH_CHECK("replay: validate cursor at 7",
                 validate_headers_stage_drain(10) >= 1 &&
                 validate_headers_stage_cursor() == 7);

        /* Tear down both stages, close store. */
        validate_headers_stage_shutdown();
        header_admit_stage_shutdown();
        progress_store_close();

        /* Reopen everything. */
        VH_CHECK("replay: reopen store", progress_store_open(dir));
        VH_CHECK("replay: re-init admit", header_admit_stage_init(&ms));
        VH_CHECK("replay: re-init validate",
                 validate_headers_stage_init(&ms));
        validate_headers_stage_set_validator(stub_pass, NULL);

        stage_result_t r = validate_headers_stage_step_once();
        VH_CHECK("replay: first step after reopen is IDLE (cursor=7)",
                 r == STAGE_IDLE);
        VH_CHECK("replay: cursor restored to 7",
                 validate_headers_stage_cursor() == 7);

        sqlite3 *db = progress_store_db();
        VH_CHECK("replay: log still has 7 rows",
                 log_row_count(db) == 7);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── authoritative path marks successful headers valid ─────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("auth: setup",
                 vh_setup("auth", 2, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        header_admit_stage_drain(100);
        sc.blocks[0].nStatus = BLOCK_VALID_UNKNOWN;
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);

        VH_CHECK("auth: validate advances",
                 validate_headers_stage_step_once() == STAGE_ADVANCED);
        VH_CHECK("auth: h=0 marked valid header",
                 (sc.blocks[0].nStatus & BLOCK_VALID_MASK) >=
                     BLOCK_VALID_HEADER);
        VH_CHECK("auth: pass record lookup succeeds",
                 validate_headers_stage_has_pass_record(0, &sc.hashes[0]));

        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
        vh_teardown(dir, &ms, &sc);
    }

    /* ── authoritative legacy ingress guard detects missing pass row ─ */
    {
        char dir[256];
        vh_tmpdir(dir, sizeof(dir), "cutover_guard");
        mkdir_p_vh(dir);
        VH_CHECK("guard: store opens", progress_store_open(dir));

        const struct chain_params *params = chain_params_get();
        if (!params) {
            printf("validate_headers: guard skipped (no chain params)\n");
        } else {
            struct main_state ms;
            main_state_init(&ms);

            struct uint256 parent_hash;
            memset(&parent_hash, 0, sizeof(parent_hash));
            parent_hash.data[0] = 0x55;
            struct block_index *parent = chainstate_insert_block_index(
                (struct chainstate *)&ms, &parent_hash);
            VH_CHECK("guard: parent inserted", parent != NULL);
            if (parent) {
                parent->nHeight = 0;
                parent->nStatus = BLOCK_VALID_TREE;
                active_chain_set_tip(&ms.chain_active, parent);
                ms.pindex_best_header = parent;
            }

            struct block_header hdr;
            block_header_init(&hdr);
            hdr.hashPrevBlock = parent_hash;
            hdr.nTime = 1;
            hdr.nBits = 1;
            hdr.nSolutionSize = 0;

            struct uint256 hash;
            block_header_get_hash(&hdr, &hash);
            struct block_index *bi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hash);
            VH_CHECK("guard: block index inserted", bi != NULL);
            if (bi) {
                bi->nHeight = 1;
                bi->nStatus = BLOCK_VALID_UNKNOWN;
                bi->pprev = parent;
                active_chain_set_tip(&ms.chain_active, bi);
                ms.pindex_best_header = bi;
            }

            VH_CHECK("guard: stage init creates schema",
                     validate_headers_stage_init(&ms));

            int guard_events = 0;
            event_log_init();
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            VH_CHECK("guard: observer registers",
                     event_observe(EV_CUTOVER_GUARD_DIVERGED,
                                   cutover_guard_observer,
                                   &guard_events));

            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *out = NULL;
            validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);
            bool ok = accept_block_header(&hdr, &vs, &ms, params, &out);
            VH_CHECK("guard: legacy path rejects missing pass row", !ok);
            VH_CHECK("guard: reject reason names validate_headers",
                     strcmp(vs.reject_reason,
                            "validate-headers-cutover-diverged") == 0);
            VH_CHECK("guard: divergence event emitted",
                     guard_events == 1);

            validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            validate_headers_stage_shutdown();
            main_state_free(&ms);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── fast-forwarded cursor permits the first post-import header ── */
    {
        char dir[256];
        vh_tmpdir(dir, sizeof(dir), "fast_forward_guard");
        mkdir_p_vh(dir);
        VH_CHECK("ff: store opens", progress_store_open(dir));

        const struct chain_params *params = chain_params_get();
        if (!params) {
            printf("validate_headers: fast-forward guard skipped (no chain params)\n");
        } else {
            sqlite3 *db = progress_store_db();
            VH_CHECK("ff: stamp validate cursor",
                     vh_stamp_cursor(db, "validate_headers", 1));
            int32_t legacy_tip = 0;
            VH_CHECK("ff: stamp legacy attach tip meta",
                     progress_meta_set(db, "legacy_attach_tip_height",
                                       &legacy_tip, sizeof(legacy_tip)));

            struct main_state ms;
            main_state_init(&ms);

            struct uint256 parent_hash;
            memset(&parent_hash, 0, sizeof(parent_hash));
            parent_hash.data[0] = 0x77;
            struct block_index *parent = chainstate_insert_block_index(
                (struct chainstate *)&ms, &parent_hash);
            VH_CHECK("ff: parent inserted", parent != NULL);
            if (parent) {
                parent->nHeight = 0;
                parent->nStatus = BLOCK_VALID_TREE;
                active_chain_set_tip(&ms.chain_active, parent);
                ms.pindex_best_header = parent;
            }

            struct block_header hdr;
            block_header_init(&hdr);
            hdr.hashPrevBlock = parent_hash;
            hdr.nTime = 1;
            hdr.nBits = 1;
            hdr.nSolutionSize = 0;

            struct uint256 hash;
            block_header_get_hash(&hdr, &hash);
            struct block_index *bi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hash);
            VH_CHECK("ff: block index inserted", bi != NULL);
            if (bi) {
                bi->nHeight = 1;
                bi->nStatus = BLOCK_VALID_UNKNOWN;
                bi->pprev = parent;
                active_chain_set_tip(&ms.chain_active, bi);
                ms.pindex_best_header = bi;
            }

            VH_CHECK("ff: stage init creates schema",
                     validate_headers_stage_init(&ms));
            VH_CHECK("ff: cursor observes persisted fast-forward",
                     validate_headers_stage_cursor() == 1);

            int guard_events = 0;
            event_log_init();
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            VH_CHECK("ff: observer registers",
                     event_observe(EV_CUTOVER_GUARD_DIVERGED,
                                   cutover_guard_observer,
                                   &guard_events));

            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *out = NULL;
            validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);
            bool ok = accept_block_header(&hdr, &vs, &ms, params, &out);
            VH_CHECK("ff: existing header at fast-forward cursor accepted",
                     ok && out == bi);
            VH_CHECK("ff: no divergence event emitted", guard_events == 0);

            validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            validate_headers_stage_shutdown();
            main_state_free(&ms);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── pre-init guards ───────────────────────────────────────────── */
    {
        VH_CHECK("guard: step_once with no init returns IDLE",
                 validate_headers_stage_step_once() == STAGE_IDLE);
        VH_CHECK("guard: init(NULL) rejected",
                 !validate_headers_stage_init(NULL));
    }

    /* ── dump_state_json shape ─────────────────────────────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("dump: setup",
                 vh_setup("dump", 2, stub_pass, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        header_admit_stage_drain(100);
        validate_headers_stage_drain(10);

        struct json_value v;
        json_init(&v);
        VH_CHECK("dump: returns true",
                 validate_headers_stage_dump_state_json(&v, NULL));
        char buf[2048];
        size_t n = json_write(&v, buf, sizeof(buf));
        VH_CHECK("dump: serializes", n > 0 && n < sizeof(buf));
        VH_CHECK("dump: initialised=true",
                 strstr(buf, "\"initialised\":true") != NULL);
        VH_CHECK("dump: cursor=2",
                 strstr(buf, "\"cursor\":2") != NULL);
        VH_CHECK("dump: passed_total=2",
                 strstr(buf, "\"passed_total\":2") != NULL);
        VH_CHECK("dump: pool_size present",
                 strstr(buf, "\"pool_size\":4") != NULL);
        VH_CHECK("dump: failure_log_count present",
                 strstr(buf, "\"failure_log_count\":0") != NULL);
        VH_CHECK("dump: first_failed_height present",
                 strstr(buf, "\"first_failed_height\":-1") != NULL);
        VH_CHECK("dump: last_failed_height present",
                 strstr(buf, "\"last_failed_height\":-1") != NULL);
        json_free(&v);

        vh_teardown(dir, &ms, &sc);
    }

    printf("validate_headers_stage: %d failures\n", failures);
    return failures;
}
