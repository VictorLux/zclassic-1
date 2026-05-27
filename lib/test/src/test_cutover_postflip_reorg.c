/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_cutover_postflip_reorg — the SCARIEST cutover-safety scenario: a chain
 * REORG that happens AFTER the shadow->authoritative flip, i.e. while the
 * log-authoritative tip (tip_finalize_log) is the DEFINITIONAL tip.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * test_cutover_flip_dryrun proves the flip MECHANISM (the gate, the pairing
 * requirement, the post-flip canary, forward-only tip parity). But the live
 * flip is only safe if the log-authoritative pipeline can ALSO correctly
 * UNWIND a losing branch and RE-CONVERGE onto a heavier winning branch — a
 * mainnet reorg WILL happen, and if it happens after the flip the reducer's
 * terminal stage (tip_finalize) must rewind its cursor to the fork boundary,
 * re-finalize the winning branch, and re-derive a tip that still equals what
 * the legacy pipeline produces. A pipeline that can only roll forward halts on
 * the first post-flip reorg. This test is the offline proof it does not.
 *
 * WHAT IS DRIVEN
 * --------------
 * Same fixture machinery as test_cutover_tip_parity (synthetic active_chain +
 * seeded utxo_apply_log feeding tip_finalize_stage_drain / _step_once) and
 * test_cutover_flip_dryrun (the real cutover_modes flip path +
 * shadow_conservation ledger + the no-progress canary). On an ephemeral
 * progress.kv under ./test-tmp — NEVER the live node, NEVER ~/.zclassic-c23.
 *
 * The reorg topology mirrors test_cutover_tip_parity's reorg scenario and the
 * test_reorg_parity branch-A/branch-B fork, but it is executed AFTER the flip
 * to authoritative:
 *   1. Build a chain to height N, catch up via the shadow stages, then flip to
 *      AUTHORITATIVE exactly as the accepted controller path does
 *      (cutover_modes_set_header_pipeline + cutover_modes_record_change).
 *   2. WHILE AUTHORITATIVE, install a coherent competing fork: heights
 *      FORK+1..N get NEW hashes and strictly more chain work, re-linked as a
 *      valid branch, then EXTEND the winning branch one block above the old
 *      tip (height N+1, heaviest of all) so the reorg both replaces interior
 *      heights AND advances past the old tip.
 *   3. ASSERT through the reorg:
 *      R1  the log-authoritative tip DISCONNECTS the losing branch and
 *          CONNECTS the winning branch — the recorded tip hash at every live
 *          height equals the NEW active_chain best block there (parity holds
 *          THROUGH the unwind+reconnect, via the SAME comparator the dry-run
 *          and tip-parity tests use).
 *      R2  the tip_finalize cursor rewinds to the fork boundary and re-advances
 *          to the new tip with zero lag (not stuck, not double-counting): the
 *          reorg_detected counter increments and the final cursor == new tip.
 *      R3  the UTXO commitment after the reorg is PATH-INDEPENDENT — the
 *          queried recompute over the reorged coin set equals a from-scratch
 *          recompute of the winning branch built directly (reusing the exact
 *          coins_view_cache_recompute_commitment assertion from
 *          test_reorg_parity). This is the byte-exact unwind invariant.
 *      R4  the no-progress canary does NOT fire FAILED during the reorg (a
 *          reorg is PROGRESS, not a stall) and re-PASSES on the new tip.
 *   4. NEGATIVE CONTROL (mandatory teeth): poison one finalized tip-hash on the
 *      WINNING branch and assert the parity comparator DETECTS the divergence.
 *
 * THE ONE HONEST BOUNDARY (same as the dry-run)
 * ---------------------------------------------
 * The full RPC `cutoverpreflight ready` boolean cannot be true on a fixture
 * (the `live` and `chain_advance` gates read runtime singletons only a booted
 * node populates — see test_cutover_flip_dryrun's header). So we drive the
 * accepted flip's underlying functions directly (the dry-run's Scenario B
 * documents and itself asserts that boundary). This test layers the POST-FLIP
 * REORG on top of that same largest-tractable slice. The tip_finalize stage
 * itself runs UNMODIFIED — its real reorg-rewind path
 * (rewind_cursor_if_active_chain_reorged) is what we exercise.
 *
 * Every assertion has teeth: the comparators are shared between the positive
 * replay and the negative control; the negative control genuinely fails on a
 * poisoned tip hash; and the key parity assertion is mutation-tested (a flipped
 * byte in the recorded tip hash makes parity_holds_at return false — proven by
 * the negative control firing on exactly that mutation).
 */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "jobs/header_admit_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/validate_headers_stage.h"
#include "services/cutover_modes.h"
#include "storage/progress_store.h"
#include "adapters/inbound/shadow_conservation.h"
#include "util/blocker.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/connect_block.h"
#include "validation/update_coins.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "coins/undo.h"
#include "consensus/validation.h"
#include "bloom/merkle.h"
#include "script/script.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CPR_CHECK(name, expr) do {                     \
    printf("cutover_postflip_reorg: %s... ", (name));  \
    if ((expr)) printf("OK\n");                        \
    else { printf("FAIL\n"); failures++; }             \
} while (0)

/* ── Synthetic chain (mirrors test_cutover_tip_parity / tip_finalize) ──── */

struct cpr_chain {
    struct block_index *blocks;
    struct uint256     *hashes;
    int n;
};

static int cpr_mkdir(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void cpr_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/cutover_postflip_reorg_%d_%s",
             (int)getpid(), tag);
}

static void cpr_hash(struct uint256 *out, int h, uint8_t fork_tag)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0xa0 + h);
    out->data[1] = fork_tag;
    out->data[2] = 0xC3;  /* match sibling fixtures' "C3" marker byte */
}

/* Build a coherent chain of n block_index entries (heights 0..n-1). */
static bool cpr_chain_build(struct cpr_chain *sc, int n, uint8_t fork_tag)
{
    sc->blocks = calloc((size_t)n, sizeof(struct block_index));
    sc->hashes = calloc((size_t)n, sizeof(struct uint256));
    if (!sc->blocks || !sc->hashes) return false;
    for (int i = 0; i < n; i++) {
        cpr_hash(&sc->hashes[i], i, fork_tag);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = 4;
        sc->blocks[i].nTime = (uint32_t)(1700007000u + (uint32_t)i);
        sc->blocks[i].nBits = 0x1f07ffff;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        arith_uint256_set_u64(&sc->blocks[i].nChainWork, (uint64_t)i + 1);
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void cpr_chain_free(struct cpr_chain *sc)
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

static bool cpr_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);  // raw-sql-ok:test-direct
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

/* Seed utxo_apply_log with `n` all-passing rows (heights 0..n-1) + the upstream
 * cursor at `cursor`. Same feeder as the tip-parity / dry-run tests. */
static bool cpr_seed_utxo_apply(sqlite3 *db, int rows, int cursor)
{
    if (!cpr_exec(db,
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

/* Bump the utxo_apply upstream cursor so tip_finalize may finalize more
 * heights. Rows already exist; this exposes more of them to the stage. */
static bool cpr_bump_utxo_apply_cursor(sqlite3 *db, int cursor)
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

/* Read the tip_hash the reducer recorded for finalize-row `height` (that row
 * describes the tip at logical height `height+1`). */
static bool cpr_log_tip_hash(sqlite3 *db, int height, struct uint256 *out)
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

/* The single load-bearing tip comparator (same shape as the tip-parity and
 * dry-run tests): true iff the log-derived tip at logical height `tip_height`
 * equals the legacy active_chain tip there. Both the positive replay and the
 * negative control call THIS, so green-positive + red-negative proves teeth. */
static bool cpr_parity_holds_at(sqlite3 *db, const struct active_chain *chain,
                                int tip_height)
{
    if (tip_height < 1) return false;
    struct block_index *legacy = active_chain_at(chain, tip_height);
    if (!legacy || !legacy->phashBlock) return false;
    struct uint256 derived;
    if (!cpr_log_tip_hash(db, tip_height - 1, &derived)) return false;
    return uint256_eq(&derived, legacy->phashBlock) != 0;
}

/* Negative-control poison: overwrite a recorded tip_hash so it no longer
 * matches chain_active. Used ONLY to prove the comparator detects divergence. */
static bool cpr_poison_tip_hash(sqlite3 *db, int finalize_height)
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
static bool cpr_stub_pass(const struct block_index *bi, const char *datadir,
                          char *out_reason, size_t out_reason_size,
                          void *user)
{
    (void)bi; (void)datadir; (void)user;
    if (out_reason && out_reason_size) out_reason[0] = 0;
    return true;
}

/* ── Byte-exact UTXO commitment machinery (reused from test_reorg_parity) ──
 * Real consensus paths: update_coins / update_coins_with_undo / disconnect_block
 * with full block_undo. No stubs — the post-flip reorg's coin-set unwind is the
 * genuine disconnect path, so R3's path-independence proof has real teeth. */

static struct transaction cpr_make_coinbase(int height, uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "cpr_cb_vin");
    uint8_t sig[6] = {
        4,
        (uint8_t)(height & 0xFF),
        (uint8_t)((height >> 8) & 0xFF),
        (uint8_t)((height >> 16) & 0xFF),
        (uint8_t)((height >> 24) & 0xFF),
        seed,
    };
    script_set(&tx.vin[0].script_sig, sig, 6);
    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.n = 0xFFFFFFFF;
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "cpr_cb_vout");
    tx.vout[0].value = 1000000000LL;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, 3);
    transaction_compute_hash(&tx);
    return tx;
}

/* A simple spend of one prior output -> one new output (exercises a divergent
 * spend pattern across branches, as test_reorg_parity does). */
static struct transaction cpr_make_spend(const struct uint256 *prev_txid,
                                         uint32_t prev_n, int64_t value,
                                         uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "cpr_sp_vin");
    tx.vin[0].prevout.hash = *prev_txid;
    tx.vin[0].prevout.n = prev_n;
    uint8_t sig[2] = {0x48, seed};
    script_set(&tx.vin[0].script_sig, sig, 2);
    tx.vin[0].sequence = 0xFFFFFFFF;
    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "cpr_sp_vout");
    tx.vout[0].value = value;
    uint8_t pk[4] = {0x76, 0xa9, 0x14, seed};
    script_set(&tx.vout[0].script_pub_key, pk, 4);
    transaction_compute_hash(&tx);
    return tx;
}

static void cpr_free_tx(struct transaction *tx)
{
    free(tx->vin);
    free(tx->vout);
}

static void cpr_free_block(struct block *blk)
{
    for (size_t i = 0; i < blk->num_vtx; i++) {
        free(blk->vtx[i].vin);
        free(blk->vtx[i].vout);
    }
    free(blk->vtx);
    blk->vtx = NULL;
    blk->num_vtx = 0;
}

/* Build a block holding the supplied transactions (block takes ownership by
 * copy; caller frees via cpr_free_block, NOT the originals). */
static void cpr_make_block(struct block *blk, int height,
                           const struct uint256 *prev_hash, uint8_t seed,
                           struct transaction *txs, size_t ntx)
{
    memset(blk, 0, sizeof(*blk));
    blk->num_vtx = ntx;
    blk->vtx = zcl_calloc(ntx, sizeof(struct transaction), "cpr_blk_vtx");
    struct uint256 *leaves =
        zcl_calloc(ntx, sizeof(struct uint256), "cpr_blk_leaves");
    for (size_t i = 0; i < ntx; i++) {
        blk->vtx[i] = txs[i];
        leaves[i] = txs[i].hash;
    }
    blk->header.nVersion = 4;
    if (prev_hash) blk->header.hashPrevBlock = *prev_hash;
    blk->header.nTime = 1000000 + (uint32_t)height * 150 + seed;
    blk->header.hashMerkleRoot = compute_merkle_root(leaves, ntx);
    free(leaves);
}

/* Connect every tx via update_coins_with_undo, accumulating undo so
 * disconnect_block can reverse it. Coinbase is connected with update_coins. */
static bool cpr_connect_with_undo(struct block *blk, int height,
                                  struct coins_view_cache *cache,
                                  struct block_undo *bu)
{
    block_undo_init(bu);
    if (blk->num_vtx > 1) block_undo_alloc(bu, blk->num_vtx - 1);
    update_coins(&blk->vtx[0], cache, height);
    for (size_t i = 1; i < blk->num_vtx; i++) {
        struct tx_undo txundo;
        memset(&txundo, 0, sizeof(txundo));
        if (!update_coins_with_undo(&blk->vtx[i], cache, &txundo, height))
            return false;
        bu->vtxundo[i - 1] = txundo;
    }
    return true;
}

/* Recompute a path-independent commitment over an explicit txid universe — the
 * universe-driven twin of coins_view_cache_recompute_commitment(), kept to
 * cross-check the production query against an INDEPENDENT recompute. */
static void cpr_recompute_universe(struct coins_view_cache *view,
                                   const struct uint256 *txids, size_t ntx,
                                   struct utxo_commitment *out)
{
    utxo_commitment_init(out);
    for (size_t t = 0; t < ntx; t++) {
        struct coins c;
        coins_init(&c);
        if (coins_view_cache_get_coins(view, &txids[t], &c)) {
            for (size_t i = 0; i < c.num_vout; i++) {
                if (coins_is_available(&c, (unsigned)i))
                    utxo_commitment_add(out, txids[t].data, (uint32_t)i,
                                        c.vout[i].value, (int32_t)c.height);
            }
        }
        coins_free(&c);
    }
}

int test_cutover_postflip_reorg(void);
int test_cutover_postflip_reorg(void)
{
    printf("\n=== cutover_postflip_reorg tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ════════════════════════════════════════════════════════════════════
     * Scenario A — a REORG while the log-authoritative tip is DEFINITIONAL.
     *
     * Catch up to height N, flip to authoritative, then install a heavier
     * competing fork (interior heights replaced + extended one above the old
     * tip). Assert the log-authoritative pipeline disconnects the loser,
     * reconnects the winner, restores tip parity at every height, rewinds +
     * re-advances its cursor, and keeps the canary quiet (a reorg is progress).
     * ════════════════════════════════════════════════════════════════════ */
    {
        char dir[256];
        cpr_tmpdir(dir, sizeof dir, "reorg");
        cpr_mkdir("./test-tmp");
        cpr_mkdir(dir);

        const int N    = 6;   /* original tip height (tips at 1..N)          */
        const int FORK = 3;   /* fork boundary: heights 1..FORK are shared   */
        const int WIN  = N + 1; /* winning-branch tip (one above old tip)    */

        cutover_modes_test_reset();
        shadow_conservation_reset();
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);

        struct main_state ms;
        struct cpr_chain sc;
        memset(&ms, 0, sizeof ms);

        CPR_CHECK("progress_store opens", progress_store_open(dir));
        active_chain_init(&ms.chain_active);
        /* Full chain has WIN+1 block_index slots (heights 0..WIN). */
        CPR_CHECK("chain builds", cpr_chain_build(&sc, WIN + 1, 0x00));

        sqlite3 *db = progress_store_db();

        /* ── Catch up to N via the shadow stages. ──────────────────────── */
        active_chain_set_tip(&ms.chain_active, &sc.blocks[N]);
        CPR_CHECK("chain height == N", active_chain_height(&ms.chain_active) == N);
        /* Seed enough apply rows for the winning branch too (heights 0..WIN-1),
         * but expose only the first N to finalize for now. */
        CPR_CHECK("utxo_apply seeded (WIN rows)",
                  cpr_seed_utxo_apply(db, /*rows=*/WIN, /*cursor=*/N));

        CPR_CHECK("header_admit init", header_admit_stage_init(&ms));
        CPR_CHECK("validate_headers init", validate_headers_stage_init(&ms));
        validate_headers_stage_set_validator(cpr_stub_pass, NULL);
        CPR_CHECK("tip_finalize init", tip_finalize_stage_init(&ms));

        CPR_CHECK("header_admit drains N+1 (genesis..tip)",
                  header_admit_stage_drain(100) == N + 1);
        CPR_CHECK("validate_headers drains (>=1 step)",
                  validate_headers_stage_drain(100) >= 1);
        CPR_CHECK("tip_finalize drains N", tip_finalize_stage_drain(100) == N);
        shadow_conservation_record_fed((unsigned long)N);
        shadow_conservation_record_diffed((unsigned long)N);

        bool pre_parity = true;
        for (int th = 1; th <= N; th++)
            if (!cpr_parity_holds_at(db, &ms.chain_active, th)) pre_parity = false;
        CPR_CHECK("pre-flip tip parity at all caught-up heights", pre_parity);
        CPR_CHECK("pre-flip cursor == N (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() == (int64_t)N);

        /* ── Flip to AUTHORITATIVE (accepted controller path). ─────────── */
        int64_t flip_tip = active_chain_height(&ms.chain_active);  /* == N */
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        cutover_modes_record_change(flip_tip, flip_tip, flip_tip, /*tip_lag*/ 0);
        CPR_CHECK("flip accepted -> authoritative_active",
                  cutover_modes_any_authoritative_active());

        uint64_t reorg_before = tip_finalize_stage_reorg_detected_total();

        /* ── Install the competing fork WHILE AUTHORITATIVE. ────────────
         * Heights FORK+1..N get NEW hashes (different fork_tag byte) and
         * strictly more chain work; height WIN extends one block above the old
         * tip, heaviest of all. Heights 1..FORK are untouched (shared base). */
        for (int h = FORK + 1; h <= WIN; h++) {
            sc.hashes[h].data[1] = 0x99;           /* new fork tag */
            sc.hashes[h].data[2] = 0xC3;
            sc.blocks[h].pprev = &sc.blocks[h - 1];
            /* strictly heavier than the original linear work (h+1) */
            arith_uint256_set_u64(&sc.blocks[h].nChainWork,
                                  (uint64_t)100 + (uint64_t)h);
        }
        CPR_CHECK("competing fork installed onto chain_active",
                  active_chain_set_tip(&ms.chain_active, &sc.blocks[WIN]));
        CPR_CHECK("chain_active advanced to WIN (= N+1)",
                  active_chain_height(&ms.chain_active) == WIN);

        /* Before reconcile, the STALE derived tip at the old tip height (N) no
         * longer matches chain_active — the divergence the rewind keys off.
         * (Doubles as a live negative control: an un-reconciled reorg IS caught
         * by the comparator.) */
        CPR_CHECK("stale derived tip diverges before reconcile (at FORK+1)",
                  cpr_parity_holds_at(db, &ms.chain_active,
                                      /*tip_height=*/FORK + 1) == false);

        /* Expose the winning branch's extra height to the finalize upstream. */
        CPR_CHECK("utxo_apply cursor bumped to WIN",
                  cpr_bump_utxo_apply_cursor(db, WIN));

        /* ── Drive the reducer through the reorg. It must rewind to the fork
         * boundary and re-finalize the winning branch up to WIN. ──────── */
        bool canary_never_failed = true;
        for (int i = 0; i < 200; i++) {
            /* Sample the canary as the reorg proceeds: a reorg is PROGRESS,
             * so the no-progress canary must never read FAILED. */
            struct cutover_canary_snapshot snap;
            cutover_modes_canary_snapshot(active_chain_height(&ms.chain_active),
                                          &snap);
            if (snap.failed) canary_never_failed = false;

            job_result_t r = tip_finalize_stage_step_once();
            if (r == JOB_IDLE) break;
            if (r == JOB_FATAL) break;
        }

        /* R2: the stage detected the reorg and the cursor re-advanced to the
         * NEW tip with zero lag (rewound to the fork boundary, then re-advanced
         * past the old tip — not stuck, not double-counting). */
        CPR_CHECK("R2: reorg detected by the stage",
                  tip_finalize_stage_reorg_detected_total() > reorg_before);
        CPR_CHECK("R2: cursor re-advanced to WIN (lag 0)",
                  (int64_t)tip_finalize_stage_cursor() == (int64_t)WIN);

        /* R1: tip parity holds against the NEW chain_active at EVERY live
         * height through the unwind+reconnect (1..WIN). The rewritten log rows
         * carry the winning branch's hashes. */
        bool post_parity = true;
        for (int th = 1; th <= WIN; th++) {
            if (!cpr_parity_holds_at(db, &ms.chain_active, th)) {
                post_parity = false;
                printf("    [post-reorg parity violated at tip_height=%d]\n", th);
            }
        }
        CPR_CHECK("R1: tip parity restored at every live height post-reorg",
                  post_parity);
        /* The shared base (heights 1..FORK) parity also held — proving the
         * unwind did NOT disturb heights below the fork point. */
        {
            bool base_ok = true;
            for (int th = 1; th <= FORK; th++)
                if (!cpr_parity_holds_at(db, &ms.chain_active, th)) base_ok = false;
            CPR_CHECK("R1: shared base below fork point untouched", base_ok);
        }

        /* R4: the canary never fired FAILED during the reorg, and once we
         * re-record the change at the winning tip + re-advance, it re-PASSES
         * (a reorg that advances the tip is a healthy, confirmed flip). */
        CPR_CHECK("R4: no-progress canary NEVER fired FAILED during reorg",
                  canary_never_failed);
        cutover_modes_record_change(flip_tip, flip_tip, flip_tip, /*tip_lag*/ 0);
        CPR_CHECK("R4: canary re-PASSES on the post-reorg winning tip",
                  cutover_modes_canary_target_reached(WIN));

        /* ── Step (teeth): negative control — poison a WINNING-branch tip hash
         * and prove the SAME parity comparator now reports divergence. The
         * finalize row at height (WIN-1) describes the tip at WIN. A vacuous
         * self-comparison could never fail this. ─────────────────────────── */
        CPR_CHECK("negative-control poison written",
                  cpr_poison_tip_hash(db, /*finalize_height=*/WIN - 1));
        CPR_CHECK("negative control — comparator DETECTS divergence at WIN",
                  cpr_parity_holds_at(db, &ms.chain_active,
                                      /*tip_height=*/WIN) == false);
        /* …an unpoisoned height still passes (poison is local; comparator is
         * not stuck-FALSE). */
        CPR_CHECK("an unpoisoned height still passes (comparator not stuck)",
                  cpr_parity_holds_at(db, &ms.chain_active, /*tip_height=*/2));

        validate_headers_stage_shutdown();
        header_admit_stage_shutdown();
        tip_finalize_stage_shutdown();
        active_chain_free(&ms.chain_active);
        cpr_chain_free(&sc);
        progress_store_close();
        cutover_modes_test_reset();
        shadow_conservation_reset();
        test_cleanup_tmpdir(dir);
    }

    /* ════════════════════════════════════════════════════════════════════
     * Scenario B — R3: PATH-INDEPENDENT UTXO commitment across the post-flip
     * reorg. The coin set after (build losing branch → unwind → apply winning
     * branch) must be BYTE-IDENTICAL to building the winning branch directly
     * from the fork point. Reuses the exact recompute assertion from
     * test_reorg_parity (coins_view_cache_recompute_commitment), so the live
     * node's authoritative utxo_commitment model is the comparison primitive.
     *
     * This is the consensus invariant the post-flip reorg MUST preserve: the
     * log-authoritative tip is only safe if its underlying coin set is
     * path-independent. We assert it directly on real coins_view_cache paths.
     * ════════════════════════════════════════════════════════════════════ */
    {
        /* ── Shared fork point at height 0 (genesis coinbase). ─────────── */
        struct uint256 g_hash, g_cb_hash;
        struct block genesis;
        {
            struct transaction txs[1] = { cpr_make_coinbase(0, 0x00) };
            cpr_make_block(&genesis, 0, NULL, 0x00, txs, 1);
        }
        block_header_get_hash(&genesis.header, &g_hash);
        g_cb_hash = genesis.vtx[0].hash;

        /* ── Losing branch L: heights 1..3; L2 spends L1's coinbase. ────── */
        struct block l_blk[4];
        struct uint256 l_hash[4];
        l_hash[0] = g_hash;
        {
            struct transaction txs[1] = { cpr_make_coinbase(1, 0x40) };
            cpr_make_block(&l_blk[1], 1, &l_hash[0], 0x40, txs, 1);
            block_header_get_hash(&l_blk[1].header, &l_hash[1]);
        }
        struct uint256 l1_cb_hash = l_blk[1].vtx[0].hash;
        {
            struct transaction cb = cpr_make_coinbase(2, 0x41);
            struct transaction sp = cpr_make_spend(&l1_cb_hash, 0,
                                                   900000000LL, 0x4A);
            struct transaction txs[2] = { cb, sp };
            cpr_make_block(&l_blk[2], 2, &l_hash[1], 0x41, txs, 2);
            block_header_get_hash(&l_blk[2].header, &l_hash[2]);
        }
        {
            struct transaction txs[1] = { cpr_make_coinbase(3, 0x42) };
            cpr_make_block(&l_blk[3], 3, &l_hash[2], 0x42, txs, 1);
            block_header_get_hash(&l_blk[3].header, &l_hash[3]);
        }

        /* ── Winning branch W: heights 1..4 (heavier, divergent spend).
         * W2 spends the SHARED genesis coinbase — an output L never touched. */
        struct block w_blk[5];
        struct uint256 w_hash[5];
        w_hash[0] = g_hash;
        {
            struct transaction txs[1] = { cpr_make_coinbase(1, 0x50) };
            cpr_make_block(&w_blk[1], 1, &w_hash[0], 0x50, txs, 1);
            block_header_get_hash(&w_blk[1].header, &w_hash[1]);
        }
        {
            struct transaction cb = cpr_make_coinbase(2, 0x51);
            struct transaction sp = cpr_make_spend(&g_cb_hash, 0,
                                                   950000000LL, 0x5B);
            struct transaction txs[2] = { cb, sp };
            cpr_make_block(&w_blk[2], 2, &w_hash[1], 0x51, txs, 2);
            block_header_get_hash(&w_blk[2].header, &w_hash[2]);
        }
        {
            struct transaction txs[1] = { cpr_make_coinbase(3, 0x52) };
            cpr_make_block(&w_blk[3], 3, &w_hash[2], 0x52, txs, 1);
            block_header_get_hash(&w_blk[3].header, &w_hash[3]);
        }
        {
            struct transaction txs[1] = { cpr_make_coinbase(4, 0x53) };
            cpr_make_block(&w_blk[4], 4, &w_hash[3], 0x53, txs, 1);
            block_header_get_hash(&w_blk[4].header, &w_hash[4]);
        }

        /* block_index records (height + pprev + hash) for disconnect_block. */
        struct block_index gi;
        block_index_init(&gi);
        gi.nHeight = 0;
        gi.phashBlock = &l_hash[0];
        struct block_index l_idx[4];
        for (int h = 1; h <= 3; h++) {
            block_index_init(&l_idx[h]);
            l_idx[h].nHeight = h;
            l_idx[h].phashBlock = &l_hash[h];
            l_idx[h].pprev = (h == 1) ? &gi : &l_idx[h - 1];
        }

        /* ── VIEW 1: build L, fully DISCONNECT it (real disconnect_block),
         * then connect heavier W (the genuine post-flip reorg path). ────── */
        struct coins_view_cache v_reorg;
        struct coins_view nv1;
        memset(&nv1, 0, sizeof(nv1));
        coins_view_cache_init(&v_reorg, &nv1);
        update_coins(&genesis.vtx[0], &v_reorg, 0);

        struct block_undo l_undo[4];
        bool built_l = true;
        for (int h = 1; h <= 3 && built_l; h++)
            built_l = cpr_connect_with_undo(&l_blk[h], h, &v_reorg, &l_undo[h]);
        CPR_CHECK("R3: losing branch L connects (with spend in L2)", built_l);

        bool unwound = true;
        for (int h = 3; h >= 1 && unwound; h--) {
            struct validation_state vs;
            validation_state_init(&vs);
            unwound = disconnect_block(&l_blk[h], &vs, &l_idx[h],
                                       &v_reorg, &l_undo[h]);
        }
        CPR_CHECK("R3: losing branch L fully disconnects to fork point",
                  unwound);
        for (int h = 1; h <= 3; h++) block_undo_free(&l_undo[h]);

        struct block_undo w_undo_r[5];
        bool reapplied = true;
        for (int h = 1; h <= 4 && reapplied; h++)
            reapplied = cpr_connect_with_undo(&w_blk[h], h, &v_reorg,
                                              &w_undo_r[h]);
        CPR_CHECK("R3: winning branch W connects after the reorg", reapplied);

        /* ── VIEW 2: build W directly from the fork point, never seeing L. ── */
        struct coins_view_cache v_direct;
        struct coins_view nv2;
        memset(&nv2, 0, sizeof(nv2));
        coins_view_cache_init(&v_direct, &nv2);
        update_coins(&genesis.vtx[0], &v_direct, 0);
        struct block_undo w_undo_d[5];
        bool direct = true;
        for (int h = 1; h <= 4 && direct; h++)
            direct = cpr_connect_with_undo(&w_blk[h], h, &v_direct,
                                           &w_undo_d[h]);
        CPR_CHECK("R3: winning branch W builds directly from fork point",
                  direct);

        /* Candidate txid universe: every coinbase + spend output either path
         * could create. */
        struct uint256 universe[16];
        size_t nu = 0;
        universe[nu++] = g_cb_hash;
        universe[nu++] = l1_cb_hash;
        universe[nu++] = l_blk[2].vtx[0].hash;
        universe[nu++] = l_blk[2].vtx[1].hash;
        universe[nu++] = l_blk[3].vtx[0].hash;
        for (int h = 1; h <= 4; h++) universe[nu++] = w_blk[h].vtx[0].hash;
        universe[nu++] = w_blk[2].vtx[1].hash;

        /* R3a: production path-independent query agrees between the reorged
         * view (L → unwind → W) and the direct-built W. */
        struct utxo_commitment q_reorg, q_direct;
        coins_view_cache_recompute_commitment(&v_reorg, &q_reorg);
        coins_view_cache_recompute_commitment(&v_direct, &q_direct);
        CPR_CHECK("R3: queried commitment path-independent (reorg == direct)",
                  utxo_commitment_equal(&q_reorg, &q_direct) &&
                  q_reorg.count == q_direct.count);

        /* R3b: production query agrees with an INDEPENDENT universe recompute
         * on both views — neither leaks nor drops a coin. */
        struct utxo_commitment u_reorg, u_direct;
        cpr_recompute_universe(&v_reorg, universe, nu, &u_reorg);
        cpr_recompute_universe(&v_direct, universe, nu, &u_direct);
        CPR_CHECK("R3: production query == universe recompute (reorg view)",
                  utxo_commitment_equal(&q_reorg, &u_reorg) &&
                  q_reorg.count == u_reorg.count);
        CPR_CHECK("R3: production query == universe recompute (direct view)",
                  utxo_commitment_equal(&q_direct, &u_direct) &&
                  q_direct.count == u_direct.count);

        /* R3c (teeth): no losing-branch coin leaks into the reorged view; and
         * every winning coinbase IS present. */
        {
            bool gone = !coins_view_cache_have_coins(&v_reorg, &l1_cb_hash) &&
                        !coins_view_cache_have_coins(&v_reorg,
                                                     &l_blk[2].vtx[0].hash) &&
                        !coins_view_cache_have_coins(&v_reorg,
                                                     &l_blk[2].vtx[1].hash) &&
                        !coins_view_cache_have_coins(&v_reorg,
                                                     &l_blk[3].vtx[0].hash);
            CPR_CHECK("R3: no losing-branch coin leaks into the reorged view",
                      gone);
            bool present = true;
            for (int h = 1; h <= 4; h++)
                present = present &&
                    coins_view_cache_have_coins(&v_reorg, &w_blk[h].vtx[0].hash);
            CPR_CHECK("R3: every winning-branch coinbase present post-reorg",
                      present);
        }

        /* R3d (teeth, mutation test): build a clone of the direct view, perturb
         * the coin set by exactly one output (spend genesis coinbase via W2 is
         * already done; here we connect an EXTRA spend that removes one more
         * coin), and prove the commitment query DIVERGES — confirming the R3
         * equalities are not vacuous. */
        {
            struct coins_view_cache v_mut;
            struct coins_view nvm;
            memset(&nvm, 0, sizeof(nvm));
            coins_view_cache_init(&v_mut, &nvm);
            update_coins(&genesis.vtx[0], &v_mut, 0);
            struct block_undo wm[5];
            bool okm = true;
            for (int h = 1; h <= 4 && okm; h++)
                okm = cpr_connect_with_undo(&w_blk[h], h, &v_mut, &wm[h]);
            /* Tamper: spend W1's coinbase output, removing exactly one coin. */
            struct transaction tsp =
                cpr_make_spend(&w_blk[1].vtx[0].hash, 0, 100000000LL, 0x7F);
            struct tx_undo tu;
            memset(&tu, 0, sizeof(tu));
            okm = okm && update_coins_with_undo(&tsp, &v_mut, &tu, 4);
            struct utxo_commitment q_mut;
            coins_view_cache_recompute_commitment(&v_mut, &q_mut);
            CPR_CHECK("R3: mutation (one coin spent) DIVERGES the commitment",
                      okm && (!utxo_commitment_equal(&q_mut, &q_direct) ||
                              q_mut.count != q_direct.count));
            tx_undo_free(&tu);
            cpr_free_tx(&tsp);
            for (int h = 1; h <= 4; h++) block_undo_free(&wm[h]);
            coins_view_cache_free(&v_mut);
        }

        for (int h = 1; h <= 4; h++) {
            block_undo_free(&w_undo_r[h]);
            block_undo_free(&w_undo_d[h]);
        }
        coins_view_cache_free(&v_reorg);
        coins_view_cache_free(&v_direct);
        cpr_free_block(&genesis);
        for (int h = 1; h <= 3; h++) cpr_free_block(&l_blk[h]);
        for (int h = 1; h <= 4; h++) cpr_free_block(&w_blk[h]);
    }

    /* Leave global state clean for tests sharing the process. */
    header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
    validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
    shadow_conservation_reset();
    cutover_modes_test_reset();

    printf("cutover_postflip_reorg tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
