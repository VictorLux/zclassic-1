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
#include "core/uint256.h"
#include "models/block.h"
#include "models/database.h"
#include "primitives/block.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "storage/block_index_db.h"
#include "storage/progress_store.h"
#include "storage/txdb.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_logic.h"
#include "validation/main_state.h"

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

extern struct block_tree_db *g_active_block_tree;

/* Item 2 seam: inject the node.db handle used by the default validator's
 * SQLite solution fallback (declared in validate_headers_internal.h, a
 * sibling-private header). */
void validate_headers_validator_set_node_db(struct node_db *ndb);

/* Insert one connected (status>=3) block row at `height` carrying
 * `sol`/`sol_len` as its Equihash solution. A NULL/0 solution exercises
 * the empty-column residual path. Returns true on save. */
static bool vh_db_put_block(struct node_db *ndb, int height,
                            const unsigned char *sol, size_t sol_len)
{
    struct db_block blk;
    memset(&blk, 0, sizeof(blk));
    memset(blk.hash, (uint8_t)(0x40 + (height & 0x3F)), 32);
    blk.hash[31] = (uint8_t)height;          /* keep hashes distinct */
    memset(blk.prev_hash, 0xBB, 32);
    memset(blk.merkle_root, 0xCC, 32);
    blk.height = height;
    blk.time = 1700000000;
    blk.bits = 0x1d00ffff;
    blk.status = 3;                          /* connected floor */
    blk.solution = (uint8_t *)sol;           /* may be NULL → empty col */
    blk.solution_len = sol_len;
    return db_block_save(ndb, &blk);
}

static int mkdir_p_vh(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
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
    if (sc->blocks) {
        for (int i = 0; i < sc->n; i++) {
            free(sc->blocks[i].nSolution);
            sc->blocks[i].nSolution = NULL;
            sc->blocks[i].nSolutionSize = 0;
        }
    }
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

/* Build {progress_store + ms + synth_chain + S-2 init + S-3 init w/
 * injected validator}. The caller does the work and then runs the
 * teardown. Returns 0 on success, nonzero on partial failure (still
 * tear down whatever was set up). */
static int vh_setup(const char *tag, int n, vh_validator_fn fn, void *user,
                     char *dir_out, size_t dir_out_size,
                     struct main_state *ms,
                     struct synth_chain_vh *sc)
{
    test_fmt_tmpdir(dir_out, dir_out_size, "validate_headers", tag);
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
        job_result_t r = validate_headers_stage_step_once();
        VH_CHECK("happy: next step IDLE", r == JOB_IDLE);

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
                 validate_headers_stage_step_once() == JOB_ADVANCED);
        VH_CHECK("batched: cursor at 8 after step 1",
                 validate_headers_stage_cursor() == VH_BATCH_SIZE);
        VH_CHECK("batched: step 2 ADVANCED",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
        VH_CHECK("batched: cursor at 16 after step 2",
                 validate_headers_stage_cursor() == 2 * VH_BATCH_SIZE);
        VH_CHECK("batched: step 3 ADVANCED (final partial)",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
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
            job_result_t r = header_admit_stage_step_once();
            VH_CHECK("floor: admit step advances", r == JOB_ADVANCED);
        }
        VH_CHECK("floor: admit cursor at 3",
                 header_admit_stage_cursor() == 3);

        VH_CHECK("floor: validate step ADVANCED (partial batch of 3)",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
        VH_CHECK("floor: validate cursor at 3",
                 validate_headers_stage_cursor() == 3);

        /* Next validate step has nothing to do → IDLE. */
        VH_CHECK("floor: next validate step IDLE",
                 validate_headers_stage_step_once() == JOB_IDLE);

        /* Admit 3 more, validate advances 3 more. */
        for (int i = 0; i < 3; i++)
            header_admit_stage_step_once();
        VH_CHECK("floor: admit cursor now at 6",
                 header_admit_stage_cursor() == 6);
        VH_CHECK("floor: validate ADVANCED again",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
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

    /* ── default validator must not require a body file ────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        VH_CHECK("index-header: setup default validator",
                 vh_setup("index_header", 1, NULL, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        sc.blocks[0].nSolutionSize = 36;
        sc.blocks[0].nSolution = zcl_malloc(sc.blocks[0].nSolutionSize,
                                            "vh_test_solution");
        VH_CHECK("index-header: alloc solution",
                 sc.blocks[0].nSolution != NULL);
        if (sc.blocks[0].nSolution)
            memset(sc.blocks[0].nSolution, 0, sc.blocks[0].nSolutionSize);

        header_admit_stage_drain(10);
        VH_CHECK("index-header: validate one row",
                 validate_headers_stage_drain(10) == 1);
        int ok = -1;
        char reason[64];
        VH_CHECK("index-header: row exists",
                 log_row_at(progress_store_db(), 0, &ok,
                            reason, sizeof(reason)));
        VH_CHECK("index-header: failure is header-derived",
                 ok == 0 && strcmp(reason, "disk-read-failed") != 0);

        vh_teardown(dir, &ms, &sc);
    }

    /* ── default validator loads persisted solution, not block body ── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        struct block_tree_db btdb;
        bool btdb_open = false;
        struct block_tree_db *old_tree = g_active_block_tree;

        VH_CHECK("persisted-solution: setup default validator",
                 vh_setup("persisted_solution", 1, NULL, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        dbi.nHeight = 0;
        dbi.nStatus = BLOCK_VALID_HEADER;
        dbi.nVersion = sc.blocks[0].nVersion;
        dbi.hashMerkleRoot = sc.blocks[0].hashMerkleRoot;
        dbi.hashFinalSaplingRoot = sc.blocks[0].hashFinalSaplingRoot;
        dbi.nTime = sc.blocks[0].nTime;
        dbi.nBits = sc.blocks[0].nBits;
        dbi.nNonce = sc.blocks[0].nNonce;
        dbi.nSolutionSize = 36;
        memset(dbi.nSolution, 0, dbi.nSolutionSize);
        disk_block_index_get_hash(&dbi, &sc.hashes[0]);

        char btdb_path[512];
        snprintf(btdb_path, sizeof(btdb_path), "%s/blocktree", dir);
        btdb_open = block_tree_db_open(&btdb, btdb_path,
                                       1 << 20, false, true);
        VH_CHECK("persisted-solution: blocktree opens", btdb_open);
        if (btdb_open) {
            VH_CHECK("persisted-solution: writes disk index",
                     block_tree_db_write_block_index(&btdb, &dbi));
            g_active_block_tree = &btdb;
        }

        header_admit_stage_drain(10);
        VH_CHECK("persisted-solution: validate one row",
                 validate_headers_stage_drain(10) == 1);
        int ok = -1;
        char reason[64];
        VH_CHECK("persisted-solution: row exists",
                 log_row_at(progress_store_db(), 0, &ok,
                            reason, sizeof(reason)));
        VH_CHECK("persisted-solution: failure is header-derived",
                 ok == 0 &&
                 strcmp(reason, "disk-read-failed") != 0 &&
                 strcmp(reason, "no-header-solution") != 0);

        g_active_block_tree = old_tree;
        if (btdb_open)
            block_tree_db_close(&btdb);
        vh_teardown(dir, &ms, &sc);
    }

    /* ── Item 2: node.db solution loader bounds (case c) ───────────── */
    {
        struct node_db ndb;
        VH_CHECK("loader: db opens",
                 node_db_open(&ndb, ":memory:"));

        unsigned char sol[64];
        memset(sol, 0xA5, sizeof(sol));
        VH_CHECK("loader: insert connected block w/ 64B solution",
                 vh_db_put_block(&ndb, 100, sol, sizeof(sol)));

        /* Present + fits → loads exactly, sets out_len. */
        unsigned char out[MAX_SOLUTION_SIZE];
        size_t out_len = 999;
        VH_CHECK("loader: loads present solution",
                 db_block_load_solution_by_height(&ndb, 100, out,
                                                  &out_len, sizeof(out)));
        VH_CHECK("loader: out_len == 64 + bytes match",
                 out_len == sizeof(sol) &&
                 memcmp(out, sol, sizeof(sol)) == 0);

        /* Oversize: max smaller than the stored blob → false, out_len 0. */
        out_len = 999;
        VH_CHECK("loader: oversize (max<len) → false",
                 !db_block_load_solution_by_height(&ndb, 100, out,
                                                   &out_len, 32) &&
                 out_len == 0);

        /* Missing height → false. */
        out_len = 999;
        VH_CHECK("loader: missing row → false",
                 !db_block_load_solution_by_height(&ndb, 4242, out,
                                                   &out_len, sizeof(out)) &&
                 out_len == 0);

        /* Wrong status (below the >=3 connected floor) → invisible. */
        {
            sqlite3_stmt *st = NULL;
            unsigned char z32[32]; memset(z32, 0x11, 32);
            int rc = sqlite3_prepare_v2(ndb.db,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "num_tx) VALUES(?,?,?,4,?,1,1,?,?,?,1,0)",
                -1, &st, NULL);
            VH_CHECK("loader: prepare status-2 insert", rc == SQLITE_OK);
            if (rc == SQLITE_OK) {
                unsigned char h[32]; memset(h, 0x77, 32);
                sqlite3_bind_blob(st, 1, h, 32, SQLITE_STATIC);
                sqlite3_bind_int(st, 2, 101);
                sqlite3_bind_blob(st, 3, z32, 32, SQLITE_STATIC);
                sqlite3_bind_blob(st, 4, z32, 32, SQLITE_STATIC);
                sqlite3_bind_blob(st, 5, z32, 32, SQLITE_STATIC);
                sqlite3_bind_blob(st, 6, sol, (int)sizeof(sol), SQLITE_STATIC);
                sqlite3_bind_blob(st, 7, z32, 32, SQLITE_STATIC);
                (void)sqlite3_step(st);  // raw-sql-ok:test-direct
                sqlite3_finalize(st);
            }
            out_len = 999;
            VH_CHECK("loader: status<3 row not returned",
                     !db_block_load_solution_by_height(&ndb, 101, out,
                                                       &out_len, sizeof(out)));
        }

        /* Empty solution column at status>=3 → false (backfill residual). */
        {
            sqlite3_stmt *st = NULL;
            unsigned char z32[32]; memset(z32, 0x22, 32);
            int rc = sqlite3_prepare_v2(ndb.db,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "num_tx) VALUES(?,?,?,4,?,1,1,?,?,?,3,0)",
                -1, &st, NULL);
            VH_CHECK("loader: prepare empty-solution insert", rc == SQLITE_OK);
            if (rc == SQLITE_OK) {
                unsigned char h[32]; memset(h, 0x88, 32);
                sqlite3_bind_blob(st, 1, h, 32, SQLITE_STATIC);
                sqlite3_bind_int(st, 2, 102);
                sqlite3_bind_blob(st, 3, z32, 32, SQLITE_STATIC);
                sqlite3_bind_blob(st, 4, z32, 32, SQLITE_STATIC);
                /* zero-length, non-NULL blob → satisfies NOT NULL but empty */
                sqlite3_bind_blob(st, 5, z32, 32, SQLITE_STATIC);
                sqlite3_bind_blob(st, 6, "", 0, SQLITE_STATIC);
                sqlite3_bind_blob(st, 7, z32, 32, SQLITE_STATIC);
                (void)sqlite3_step(st);  // raw-sql-ok:test-direct
                sqlite3_finalize(st);
            }
            out_len = 999;
            VH_CHECK("loader: empty solution col → false (no false data)",
                     !db_block_load_solution_by_height(&ndb, 102, out,
                                                       &out_len, sizeof(out)) &&
                     out_len == 0);
        }

        /* NULL-arg / out_len-only guards. */
        VH_CHECK("loader: NULL ndb → false",
                 !db_block_load_solution_by_height(NULL, 100, out,
                                                   &out_len, sizeof(out)));
        VH_CHECK("loader: max==0 → false",
                 !db_block_load_solution_by_height(&ndb, 100, out,
                                                   &out_len, 0));

        node_db_close(&ndb);
    }

    /* ── Item 2: validator fallback loads from node.db (case a) ────── *
     * block_index solution empty, persisted index absent, BUT node.db
     * carries a solution at this height. The fallback must LOAD it and
     * push it through the IDENTICAL validate_header_fields path — so the
     * verdict is a real PoW/Equihash result, never the backfill reason
     * and never "no-header-solution". (A synthetic 1344B zero blob will
     * fail Equihash, which is exactly correct: no false pass.) */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        struct node_db ndb;
        VH_CHECK("fallback: node.db opens",
                 node_db_open(&ndb, ":memory:"));

        unsigned char sol[MAX_SOLUTION_SIZE];
        memset(sol, 0x00, sizeof(sol));
        VH_CHECK("fallback: seed node.db solution at h=0",
                 vh_db_put_block(&ndb, 0, sol, sizeof(sol)));

        VH_CHECK("fallback: setup default validator",
                 vh_setup("fallback_loads", 1, NULL, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        /* Leave sc.blocks[0].nSolution NULL (index empty) and inject the
         * fixture node.db so the fallback resolves it. */
        validate_headers_validator_set_node_db(&ndb);

        header_admit_stage_drain(10);
        VH_CHECK("fallback: validate one row",
                 validate_headers_stage_drain(10) == 1);
        int ok = -1;
        char reason[64] = {0};
        VH_CHECK("fallback: row exists",
                 log_row_at(progress_store_db(), 0, &ok,
                            reason, sizeof(reason)));
        /* Loader was reached: verdict is a real validation failure, NOT
         * the residual backfill reason and NOT the pre-loader empty
         * reason. This proves the node.db bytes flowed into identical
         * Equihash verification. */
        VH_CHECK("fallback: node.db solution reached identical validation",
                 ok == 0 &&
                 strcmp(reason, "no-header-solution") != 0 &&
                 strcmp(reason,
                        "no-header-solution-backfill-required") != 0);

        validate_headers_validator_set_node_db(NULL);
        vh_teardown(dir, &ms, &sc);
        node_db_close(&ndb);
    }

    /* ── Item 2: node.db ALSO empty → fail with backfill reason (b) ── *
     * The 675K residual rows. block_index empty, persisted index empty,
     * node.db has no solution at this height → the validator MUST fail
     * with the distinct backfill reason. Proves there is NO false-pass on
     * a header lacking a real, verified solution. */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        struct node_db ndb;
        VH_CHECK("residual: node.db opens",
                 node_db_open(&ndb, ":memory:"));
        /* node.db is empty — no row at h=0 at all. */

        VH_CHECK("residual: setup default validator",
                 vh_setup("residual_backfill", 1, NULL, NULL,
                          dir, sizeof(dir), &ms, &sc) == 0);
        validate_headers_validator_set_node_db(&ndb);

        header_admit_stage_drain(10);
        VH_CHECK("residual: validate one row",
                 validate_headers_stage_drain(10) == 1);
        int ok = -1;
        char reason[64] = {0};
        VH_CHECK("residual: row exists",
                 log_row_at(progress_store_db(), 0, &ok,
                            reason, sizeof(reason)));
        VH_CHECK("residual: fails with distinct backfill reason (no false pass)",
                 ok == 0 &&
                 strcmp(reason,
                        "no-header-solution-backfill-required") == 0);

        validate_headers_validator_set_node_db(NULL);
        vh_teardown(dir, &ms, &sc);
        node_db_close(&ndb);
    }

    /* ── persisted failures are rechecked after restart ────────────── */
    {
        char dir[256]; struct main_state ms; struct synth_chain_vh sc;
        struct fail_at_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.fail_height = 1;
        VH_CHECK("recheck: setup with failing validator",
                 vh_setup("recheck", 3, stub_fail_at, &ctx,
                          dir, sizeof(dir), &ms, &sc) == 0);
        header_admit_stage_drain(100);
        VH_CHECK("recheck: initial validate advances",
                 validate_headers_stage_drain(10) == 1);
        int ok = -1;
        char reason[64] = {0};
        VH_CHECK("recheck: failed row exists",
                 log_row_at(progress_store_db(), 1, &ok,
                            reason, sizeof(reason)));
        VH_CHECK("recheck: initial row failed",
                 ok == 0 && strcmp(reason, "stub-injected-failure") == 0);

        validate_headers_stage_shutdown();
        header_admit_stage_shutdown();
        progress_store_close();

        VH_CHECK("recheck: reopen store", progress_store_open(dir));
        VH_CHECK("recheck: re-init admit", header_admit_stage_init(&ms));
        VH_CHECK("recheck: re-init validate",
                 validate_headers_stage_init(&ms));
        validate_headers_stage_set_validator(stub_pass, NULL);

        VH_CHECK("recheck: failed row is retried",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
        ok = -1;
        reason[0] = 0;
        VH_CHECK("recheck: retried row exists",
                 log_row_at(progress_store_db(), 1, &ok,
                            reason, sizeof(reason)));
        VH_CHECK("recheck: retried row now passes", ok == 1);
        VH_CHECK("recheck: cursor remains advanced",
                 validate_headers_stage_cursor() == 3);
        VH_CHECK("recheck: next step idle",
                 validate_headers_stage_step_once() == JOB_IDLE);

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

        job_result_t r = validate_headers_stage_step_once();
        VH_CHECK("replay: first step after reopen is IDLE (cursor=7)",
                 r == JOB_IDLE);
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

        VH_CHECK("auth: validate advances",
                 validate_headers_stage_step_once() == JOB_ADVANCED);
        VH_CHECK("auth: h=0 marked valid header",
                 (sc.blocks[0].nStatus & BLOCK_VALID_MASK) >=
                     BLOCK_VALID_HEADER);
        VH_CHECK("auth: pass record lookup succeeds",
                 validate_headers_stage_has_pass_record(0, &sc.hashes[0]));

        vh_teardown(dir, &ms, &sc);
    }

    /* ── pre-init guards ───────────────────────────────────────────── */
    {
        VH_CHECK("guard: step_once with no init returns IDLE",
                 validate_headers_stage_step_once() == JOB_IDLE);
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
        VH_CHECK("dump: recheck cursor present",
                 strstr(buf, "\"failure_recheck_cursor\":") != NULL);
        json_free(&v);

        vh_teardown(dir, &ms, &sc);
    }
    printf("validate_headers_stage: %d failures\n", failures);
    return failures;
}
