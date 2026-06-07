/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_coins_applied_frontier — the invariant proof for self-heal P2:
 * coins_applied_height (the canonical contiguous applied-frontier counter for
 * the coins_kv UTXO set) ALWAYS equals the durable utxo_apply stage cursor.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * MAX(coins.height) is the most-recent SURVIVING coin's creation height, NOT a
 * contiguous applied frontier — a missing-fsync interior drop is invisible to
 * it. P2 co-commits coins_applied_height inside the SAME transaction as every
 * coin mutation so the frontier cannot hide an interior hole and gives the
 * self-heal a single non-divergent coins-frontier input. This test pins the
 * load-bearing invariant — coins_applied_height == stage_cursor('utxo_apply') —
 * on all four advancing/rewinding/seeding paths:
 *
 *   (1) a forward apply advance   → frontier == cursor (== tip+1)
 *   (2) an upstream_failed advance → frontier == cursor (no coin mutation, the
 *       frontier still advances in lockstep so it never holes a skip)
 *   (3) a reorg rewind            → frontier PULLED BACK to fork+1 == cursor
 *       (a PLAIN set: the decrease must NOT be blocked by a monotonic floor)
 *   (4) a virgin progress.kv      → get returns found=false (ABSENT, never
 *       0-as-applied); after the boot backfill seeds it from the cursor →
 *       found=true and == cursor.
 *
 * Built on the same synthetic-branch harness as test_stage_reorg_unwind_parity
 * (the closest existing reducer fixture). */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "jobs/stage_helpers.h"
#include "jobs/utxo_apply_stage.h"
#include "storage/coins_kv.h"
#include "storage/event_log.h"
#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CAF_CHECK(name, expr) do {                       \
    printf("coins_applied_frontier: %s... ", (name));    \
    if ((expr)) printf("OK\n");                          \
    else { printf("FAIL\n"); failures++; }               \
} while (0)

/* ── External base coins (the pre-fork UTXO set the spends consume) ──── */

struct caf_ext_coin {
    struct uint256 txid;
    uint32_t vout;
    int64_t value;
    uint32_t height;
    bool is_coinbase;
    uint8_t script[8];
    uint32_t script_len;
};

/* Chain bodies for one branch (index by height; 0 = genesis). */
struct caf_branch {
    struct block       *bodies;
    struct uint256     *hashes;
    struct block_index *blocks;
    int n;            /* number of heights (genesis at 0 .. n-1) */
};

static int caf_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void caf_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/coins_applied_frontier_%d_%s", (int)getpid(), tag);
}

static void cb_txid(struct uint256 *out, uint8_t branch_tag, int h)
{
    uint256_set_null(out);
    out->data[0] = 0xC0;
    out->data[1] = branch_tag;
    out->data[2] = (uint8_t)h;
}

static void spend_txid(struct uint256 *out, uint8_t branch_tag, int h)
{
    uint256_set_null(out);
    out->data[0] = 0x5E;
    out->data[1] = branch_tag;
    out->data[2] = (uint8_t)h;
}

static void make_coinbase(struct transaction *tx, uint8_t branch_tag, int h)
{
    transaction_init(tx);
    (void)transaction_alloc(tx, 1, 1);
    outpoint_set_null(&tx->vin[0].prevout);
    tx->vout[0].value = 1000000000LL + h;
    uint8_t pk[3] = { 0x76, 0xa9, (uint8_t)(0x10 + h) };
    script_set(&tx->vout[0].script_pub_key, pk, 3);
    cb_txid(&tx->hash, branch_tag, h);
}

static void make_spend(struct transaction *tx, uint8_t branch_tag, int h,
                       const struct caf_ext_coin *ext)
{
    transaction_init(tx);
    (void)transaction_alloc(tx, 1, 1);
    tx->vin[0].prevout.hash = ext->txid;
    tx->vin[0].prevout.n = ext->vout;
    tx->vout[0].value = ext->value - 1000; /* fee */
    uint8_t pk[4] = { 0x76, 0xa9, 0xBB, branch_tag };
    script_set(&tx->vout[0].script_pub_key, pk, 4);
    spend_txid(&tx->hash, branch_tag, h);
}

static void finalize_block(struct block *b, int h)
{
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700000000u + (uint32_t)h);
    b->header.nBits = 0x1f07ffff;
    struct uint256 *leaves =
        zcl_calloc(b->num_vtx, sizeof(struct uint256), "caf_leaves");
    for (size_t i = 0; i < b->num_vtx; i++) leaves[i] = b->vtx[i].hash;
    b->header.hashMerkleRoot = compute_merkle_root(leaves, b->num_vtx);
    free(leaves);
}

static bool branch_build(struct caf_branch *br, uint8_t tag, int n,
                         int spend_at, const struct caf_ext_coin *ext)
{
    memset(br, 0, sizeof(*br));
    br->n = n;
    br->bodies = zcl_calloc((size_t)n, sizeof(struct block), "caf_bodies");
    br->hashes = zcl_calloc((size_t)n, sizeof(struct uint256), "caf_hashes");
    br->blocks = zcl_calloc((size_t)n, sizeof(struct block_index), "caf_blocks");
    if (!br->bodies || !br->hashes || !br->blocks) return false;

    for (int h = 0; h < n; h++) {
        struct block *b = &br->bodies[h];
        block_init(b);
        bool has_spend = (h == spend_at);
        b->num_vtx = has_spend ? 2u : 1u;
        b->vtx = zcl_calloc(b->num_vtx, sizeof(struct transaction), "caf_vtx");
        if (!b->vtx) return false;
        uint8_t cbtag = (h == 0) ? 0x00 : tag;
        make_coinbase(&b->vtx[0], cbtag, h);
        if (has_spend) make_spend(&b->vtx[1], tag, h, ext);
        finalize_block(b, h);

        block_header_get_hash(&b->header, &br->hashes[h]);
        block_index_init(&br->blocks[h]);
        br->blocks[h].phashBlock = &br->hashes[h];
        br->blocks[h].nHeight = h;
        br->blocks[h].nStatus = BLOCK_HAVE_DATA;
        if (h > 0) br->blocks[h].pprev = &br->blocks[h - 1];
    }
    return true;
}

static void branch_free(struct caf_branch *br)
{
    if (br->bodies) {
        for (int h = 0; h < br->n; h++) block_free(&br->bodies[h]);
    }
    free(br->bodies);
    free(br->hashes);
    free(br->blocks);
    memset(br, 0, sizeof(*br));
}

struct caf_ctx {
    struct caf_branch *active;
    const struct caf_ext_coin *ext;
    int n_ext;
};

static bool block_copy(struct block *dst, const struct block *src)
{
    block_init(dst);
    dst->header = src->header;
    dst->num_vtx = src->num_vtx;
    if (src->num_vtx == 0) return true;
    dst->vtx = zcl_calloc(src->num_vtx, sizeof(struct transaction), "caf_copy");
    if (!dst->vtx) return false;
    for (size_t i = 0; i < src->num_vtx; i++) {
        transaction_init(&dst->vtx[i]);
        if (!transaction_copy(&dst->vtx[i], &src->vtx[i])) return false;
    }
    return true;
}

static bool caf_reader(struct block *out, const struct block_index *bi,
                       const char *datadir, void *user)
{
    (void)datadir;
    struct caf_ctx *c = user;
    if (!out || !bi || !c || bi->nHeight < 0 || bi->nHeight >= c->active->n)
        return false;
    return block_copy(out, &c->active->bodies[bi->nHeight]);
}

static bool caf_lookup(const struct uint256 *txid, uint32_t vout,
                       struct utxo_apply_lookup *out, void *user)
{
    struct caf_ctx *c = user;
    memset(out, 0, sizeof(*out));
    if (!c) return true;
    for (int i = 0; i < c->n_ext; i++) {
        const struct caf_ext_coin *e = &c->ext[i];
        if (e->vout == vout && uint256_eq(&e->txid, txid)) {
            out->found = true;
            out->value = e->value;
            out->height = e->height;
            out->is_coinbase = e->is_coinbase;
            out->script_len = e->script_len;
            memcpy(out->script, e->script, e->script_len);
            return true;
        }
    }
    return true;
}

static bool caf_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed proof_validate_log + its cursor. `ok_through` heights are ok=1; if
 * `fail_at >= 0` that single height is recorded ok=0 (drives the
 * upstream_failed advance path). */
static bool seed_proof_validate(sqlite3 *db, int through_height, int fail_at)
{
    if (!caf_exec(db,
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL,"
        "  sapling_spends_total INTEGER NOT NULL,"
        "  sapling_outputs_total INTEGER NOT NULL,"
        "  sprout_joinsplits_total INTEGER NOT NULL,"
        "  first_failure_txid BLOB, first_failure_proof_type TEXT,"
        "  validated_at INTEGER NOT NULL)"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO proof_validate_log "
        "(height, status, ok, sapling_spends_total, sapling_outputs_total,"
        " sprout_joinsplits_total, validated_at) VALUES (?, ?, ?, 0,0,0,1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h <= through_height; h++) {
        int ok = (h == fail_at) ? 0 : 1;
        sqlite3_bind_int(st, 1, h);
        sqlite3_bind_text(st, 2, ok ? "verified" : "proof_failed", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, ok);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return false; }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
        "VALUES('proof_validate', ?, 1)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, through_height + 1);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static void seed_base_coins(sqlite3 *pdb, const struct caf_ext_coin *ext, int n)
{
    (void)coins_kv_ensure_schema(pdb);
    for (int i = 0; i < n; i++) {
        const struct caf_ext_coin *e = &ext[i];
        (void)coins_kv_add(pdb, e->txid.data, e->vout, e->value,
                           (int32_t)e->height, e->is_coinbase,
                           e->script_len ? e->script : NULL, e->script_len);
    }
}

/* The single invariant: coins_applied_height present AND == the durable
 * utxo_apply stage cursor. Returns true iff it holds. */
static bool frontier_eq_cursor(sqlite3 *db)
{
    int32_t frontier = -777;
    bool found = false;
    if (!coins_kv_get_applied_height(db, &frontier, &found))
        return false;
    if (!found)
        return false;
    uint64_t cursor = stage_cursor_persisted(db, "utxo_apply", "caf_test");
    return (uint64_t)frontier == cursor;
}

int test_coins_applied_frontier(void);
int test_coins_applied_frontier(void)
{
    printf("\n=== coins_applied_height contiguous-frontier invariant test ===\n");
    int failures = 0;

    blocker_module_init();
    caf_mkdir_p("./test-tmp");

    struct caf_ext_coin ext[2];
    memset(ext, 0, sizeof(ext));
    ext[0].txid.data[0] = 0xE7; ext[0].txid.data[1] = 0x0A; /* EXT_L */
    ext[0].vout = 0; ext[0].value = 500000000LL; ext[0].height = 0;
    ext[0].script[0] = 0x76; ext[0].script[1] = 0xa9; ext[0].script[2] = 0xAA;
    ext[0].script_len = 3;
    ext[1].txid.data[0] = 0xE7; ext[1].txid.data[1] = 0x0B; /* EXT_W */
    ext[1].vout = 0; ext[1].value = 600000000LL; ext[1].height = 0;
    ext[1].script[0] = 0x76; ext[1].script[1] = 0xa9; ext[1].script[2] = 0xBC;
    ext[1].script_len = 3;

    /* ── PART A: virgin datadir → ABSENT; forward apply; upstream_failed;
     *           reorg rewind — frontier == cursor on every path. ──────── */
    struct caf_branch L, W;
    bool built = branch_build(&L, 0x11, 4, 2, &ext[0]) &&
                 branch_build(&W, 0x22, 5, 2, &ext[1]);
    CAF_CHECK("branches build", built);

    if (built) {
        char dir[256]; caf_tmpdir(dir, sizeof(dir), "main"); caf_mkdir_p(dir);
        char log_path[512], proj_path[512];
        snprintf(log_path, sizeof(log_path), "%s/events.log", dir);
        snprintf(proj_path, sizeof(proj_path), "%s/utxo.db", dir);

        CAF_CHECK("progress_store opens", progress_store_open(dir));
        event_log_t *lg = event_log_open(log_path);
        utxo_projection_t *p = lg ? utxo_projection_open(proj_path, lg) : NULL;
        CAF_CHECK("projection opens", p != NULL);

        if (lg && p) {
            utxo_projection_set_event_log(lg);
            utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
            sqlite3 *pdb = progress_store_db();

            /* (4a) VIRGIN: before any stage activity coins_applied_height is
             * ABSENT — a fresh datadir is NOT 0-as-applied. */
            int32_t f0 = -1; bool found0 = true;
            CAF_CHECK("virgin: get succeeds",
                      coins_kv_get_applied_height(pdb, &f0, &found0));
            CAF_CHECK("virgin: found == false (ABSENT, never 0-as-applied)",
                      found0 == false);

            seed_base_coins(pdb, ext, 2);

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            active_chain_init(&ms.chain_active);
            active_chain_move_window_tip(&ms.chain_active, &L.blocks[L.n - 1]);

            struct caf_ctx ctx = { .active = &L, .ext = ext, .n_ext = 2 };
            CAF_CHECK("stage init (also runs boot backfill)",
                      utxo_apply_stage_init(&ms));
            utxo_apply_stage_set_reader(caf_reader, &ctx);
            utxo_apply_stage_set_lookup(caf_lookup, &ctx);

            /* (4b) BOOT BACKFILL on a virgin datadir: no cursor row exists yet,
             * so backfill leaves the key ABSENT (never seeds from MAX(coins)).
             * The first forward apply writes it in lockstep with the cursor. */
            int32_t fb = -1; bool foundb = true;
            CAF_CHECK("virgin backfill: still ABSENT (no cursor row to seed)",
                      coins_kv_get_applied_height(pdb, &fb, &foundb) &&
                      foundb == false);

            /* (1) FORWARD APPLY: drive L's genesis + 3 blocks. The losing
             * branch L spends EXT_L at h2. frontier must == cursor == L.n. */
            CAF_CHECK("L seed proof_validate (all ok)",
                      seed_proof_validate(pdb, L.n - 1, -1));
            int adv_l = utxo_apply_stage_drain(100);
            CAF_CHECK("L drains all", adv_l == L.n);
            CAF_CHECK("forward apply: frontier == cursor", frontier_eq_cursor(pdb));
            {
                int32_t fr = -1; bool fnd = false;
                (void)coins_kv_get_applied_height(pdb, &fr, &fnd);
                CAF_CHECK("forward apply: frontier present && == L tip+1",
                          fnd && fr == L.n);
            }

            /* (3) REORG REWIND: install heavier W on active_chain, extend
             * proof_validate, step. The unwind pulls cursor + frontier BACK to
             * fork+1, then re-advances over W. frontier must == cursor (the
             * PLAIN set allowed the decrease). */
            ctx.active = &W;
            active_chain_move_window_tip(&ms.chain_active, &W.blocks[W.n - 1]);
            CAF_CHECK("W seed proof_validate (all ok)",
                      seed_proof_validate(pdb, W.n - 1, -1));
            int adv_w = utxo_apply_stage_drain(100);
            CAF_CHECK("reorg fired exactly once",
                      utxo_apply_stage_reorg_unwound_total() == 1);
            CAF_CHECK("re-advanced over W", adv_w >= W.n - 1);
            CAF_CHECK("after reorg rewind: frontier == cursor",
                      frontier_eq_cursor(pdb));
            {
                int32_t fr = -1; bool fnd = false;
                (void)coins_kv_get_applied_height(pdb, &fr, &fnd);
                CAF_CHECK("after reorg: frontier present && == W tip+1",
                          fnd && fr == W.n);
            }

            utxo_apply_stage_shutdown();
            active_chain_free(&ms.chain_active);
        }
        utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
        utxo_projection_set_event_log(NULL);
        if (p) utxo_projection_close(p);
        if (lg) event_log_close(lg);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── PART B: upstream_failed advance path (no coin mutation). ─────── */
    struct caf_branch F;
    bool fbuilt = branch_build(&F, 0x33, 4, -1, NULL);  /* no spends */
    CAF_CHECK("fail-branch builds", fbuilt);

    if (fbuilt) {
        char dir[256]; caf_tmpdir(dir, sizeof(dir), "upfail"); caf_mkdir_p(dir);
        char log_path[512], proj_path[512];
        snprintf(log_path, sizeof(log_path), "%s/events.log", dir);
        snprintf(proj_path, sizeof(proj_path), "%s/utxo.db", dir);

        CAF_CHECK("upfail: progress_store opens", progress_store_open(dir));
        event_log_t *lg = event_log_open(log_path);
        utxo_projection_t *p = lg ? utxo_projection_open(proj_path, lg) : NULL;

        if (lg && p) {
            utxo_projection_set_event_log(lg);
            utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
            sqlite3 *pdb = progress_store_db();

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            active_chain_init(&ms.chain_active);
            active_chain_move_window_tip(&ms.chain_active, &F.blocks[F.n - 1]);

            struct caf_ctx ctx = { .active = &F, .ext = ext, .n_ext = 0 };
            CAF_CHECK("upfail: stage init", utxo_apply_stage_init(&ms));
            utxo_apply_stage_set_reader(caf_reader, &ctx);
            utxo_apply_stage_set_lookup(caf_lookup, &ctx);

            /* Mark height 2 as upstream ok=0 → step_apply takes the
             * upstream_failed advance path (no coin mutation, frontier still
             * advances in lockstep with the cursor). */
            CAF_CHECK("upfail: seed proof_validate with ok=0 at h2",
                      seed_proof_validate(pdb, F.n - 1, 2));
            int adv = utxo_apply_stage_drain(100);
            CAF_CHECK("upfail: drains all heights", adv == F.n);
            CAF_CHECK("upfail: recorded an upstream_failed advance",
                      utxo_apply_stage_upstream_failed_total() >= 1);
            CAF_CHECK("upstream_failed advance: frontier == cursor",
                      frontier_eq_cursor(pdb));
            {
                int32_t fr = -1; bool fnd = false;
                (void)coins_kv_get_applied_height(pdb, &fr, &fnd);
                CAF_CHECK("upstream_failed: frontier present && == tip+1 (no hole)",
                          fnd && fr == F.n);
            }

            utxo_apply_stage_shutdown();
            active_chain_free(&ms.chain_active);
        }
        utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
        utxo_projection_set_event_log(NULL);
        if (p) utxo_projection_close(p);
        if (lg) event_log_close(lg);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── PART C: boot backfill SEEDS from an existing cursor (no key yet). ─
     * Simulate an existing datadir that has a durable utxo_apply cursor but no
     * coins_applied_height key (predates P2): the backfill must seed the
     * frontier == cursor, NOT from MAX(coins.height). ──────────────────── */
    {
        char dir[256]; caf_tmpdir(dir, sizeof(dir), "backfill"); caf_mkdir_p(dir);
        CAF_CHECK("backfill: progress_store opens", progress_store_open(dir));
        sqlite3 *pdb = progress_store_db();
        (void)coins_kv_ensure_schema(pdb);

        /* Stamp a durable utxo_apply cursor at 123 with NO applied-height key
         * and NO coins at all (so MAX(coins.height) would be wrong/absent —
         * the backfill must use the cursor). */
        CAF_CHECK("backfill: stamp utxo_apply cursor=123",
                  caf_exec(pdb,
                    "INSERT OR REPLACE INTO stage_cursor(name, cursor, updated_at) "
                    "VALUES('utxo_apply', 123, 1)"));

        int32_t before = -1; bool found_before = true;
        CAF_CHECK("backfill: key ABSENT before backfill",
                  coins_kv_get_applied_height(pdb, &before, &found_before) &&
                  found_before == false);

        CAF_CHECK("backfill: runs", coins_kv_backfill_applied_height_if_absent(pdb));

        int32_t after = -999; bool found_after = false;
        CAF_CHECK("backfill: key present after backfill",
                  coins_kv_get_applied_height(pdb, &after, &found_after) &&
                  found_after == true);
        CAF_CHECK("backfill: seeded value == cursor (123), not MAX(coins)",
                  after == 123);
        CAF_CHECK("backfill: frontier == cursor after seed", frontier_eq_cursor(pdb));

        /* Idempotent: a second call is a no-op and never re-seeds (would block
         * a later legitimate rewind). Lower the cursor and confirm the key is
         * unchanged. */
        CAF_CHECK("backfill: lower cursor to 50",
                  caf_exec(pdb,
                    "UPDATE stage_cursor SET cursor=50 WHERE name='utxo_apply'"));
        CAF_CHECK("backfill: second call no-ops",
                  coins_kv_backfill_applied_height_if_absent(pdb));
        int32_t after2 = -1; bool found2 = false;
        (void)coins_kv_get_applied_height(pdb, &after2, &found2);
        CAF_CHECK("backfill: idempotent — frontier unchanged (still 123)",
                  found2 && after2 == 123);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    branch_free(&L);
    branch_free(&W);
    branch_free(&F);

    printf("=== coins_applied_height frontier invariant: %d failures ===\n", failures);
    return failures;
}
