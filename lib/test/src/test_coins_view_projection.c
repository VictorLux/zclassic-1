/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_coins_view_projection — B4: the projection-backed coins_view
 * reconstructs `struct coins` correctly from EV_UTXO_* events.
 *
 * Proves the read primitive that closes the validation feedback loop:
 *   1. reconstruct — a multi-output txid (one output spent) yields a
 *      struct coins with the right num_vout, the spent vout nulled, the
 *      live vouts' value/script intact, version==1 (matching
 *      coins_view_sqlite), and height/is_coinbase preserved.
 *   2. have/absent — have_coins is true for a live txid, false once all
 *      its outputs are spent and false for an unknown txid. */

#include "test/test_helpers.h"

#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "storage/coins_view_projection.h"
#include "storage/coins_view_sqlite.h"
#include "storage/coins_view_stage_backing.h"
#include "storage/event_log.h"
#include "storage/utxo_projection.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CVP_CHECK(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
} while (0)

static void make_txid(uint8_t txid[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++) txid[i] = (uint8_t)(seed + i);
}

static bool emit_add(uint8_t seed, uint32_t vout, int64_t value,
                     uint32_t height, bool coinbase, uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t script[64];
    for (uint32_t k = 0; k < script_len && k < sizeof(script); k++)
        script[k] = (uint8_t)((seed * 11 + k) & 0xFF);
    return utxo_projection_emit_add(txid, vout, value, height, coinbase,
                                    script_len ? script : NULL, script_len);
}

static bool emit_spend(uint8_t seed, uint32_t vout)
{
    uint8_t txid[32]; make_txid(txid, seed);
    return utxo_projection_emit_spend(txid, vout);
}

/* Byte-for-byte compare two reconstructed coins (the comparison the
 * consensus read path cares about: is_coinbase, height, version, every
 * vout's value + script bytes, and null-ness for spent vouts). */
static bool coins_byte_equal(const struct coins *a, const struct coins *b)
{
    if (a->is_coinbase != b->is_coinbase) return false;
    if (a->height      != b->height)      return false;
    if (a->version     != b->version)     return false;
    if (a->num_vout    != b->num_vout)    return false;
    for (size_t i = 0; i < a->num_vout; i++) {
        const struct tx_out *x = &a->vout[i], *y = &b->vout[i];
        if (tx_out_is_null(x) != tx_out_is_null(y)) return false;
        if (tx_out_is_null(x)) continue;
        if (x->value != y->value) return false;
        if (x->script_pub_key.size != y->script_pub_key.size) return false;
        if (x->script_pub_key.size &&
            memcmp(x->script_pub_key.data, y->script_pub_key.data,
                   x->script_pub_key.size) != 0) return false;
    }
    return true;
}

/* B4-wiring parity: the authority-gated backing selection.
 *
 * Builds the SAME UTXO set in two backings — coins.db (coins_view_sqlite)
 * and the projection — then drives connect_block's exact pattern:
 * coins_view_cache over a backing chosen by coins_view_select_connect_backing.
 *
 *   1. author == LEGACY (default): the selector returns the legacy view
 *      verbatim; reads through the cache match coins.db. (path unchanged)
 *   2. author == STAGE: the selector returns the composite; reads through
 *      the cache resolve via the projection and are byte-identical to the
 *      coins.db-backed reads for the identical set; get_best_block still
 *      delegates to coins.db so the connect_block prevblock invariant holds.
 *   3. author flips back to LEGACY: selector is byte-identical again. */
static int cvp_test_connect_backing_parity(utxo_projection_t *p)
{
    int failures = 0;

    /* --- coins.db backing holding the SAME set the projection has --- */
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db,
        "CREATE TABLE node_state (key TEXT PRIMARY KEY, value BLOB);"
        "CREATE TABLE utxos ("
        "  txid BLOB, vout INT, value INT, script BLOB,"
        "  script_type INT, address_hash BLOB, height INT, is_coinbase INT);",
        NULL, NULL, NULL);

    struct coins_view_sqlite cvs;
    CVP_CHECK("sqlite view open", coins_view_sqlite_open(&cvs, db));

    /* Populate coins.db by copying each txid's coins out of the projection
     * (T seed 0x70 and U seed 0x90 from the caller) and flushing through a
     * cache — exactly the write path connect_tip uses. */
    uint8_t tT[32]; make_txid(tT, 0x70);
    uint8_t tU[32]; make_txid(tU, 0x90);
    struct uint256 T, U;
    memcpy(T.data, tT, 32); memcpy(U.data, tU, 32);

    {
        struct coins_view backing = cvs.view;  /* impl set by _open */
        struct coins_view_cache wc;
        coins_view_cache_init(&wc, &backing);

        /* T is live (vout 1 spent) → copy from projection into the cache. */
        struct coins cT;
        if (utxo_projection_get_coins(p, T.data, &cT)) {
            struct coins_cache_entry *e = coins_view_cache_modify_new(&wc, &T);
            CVP_CHECK("db seed T entry", e != NULL);
            if (e) { coins_free(&e->coins); coins_copy(&e->coins, &cT); }
            coins_free(&cT);
        }
        /* U is fully spent → not present in either backing; nothing to seed. */
        CVP_CHECK("db flush", coins_view_cache_flush_for_testing(&wc));
        coins_view_cache_free(&wc);
    }

    struct coins_view legacy_view = cvs.view;  /* impl set by _open */

    struct coins_view_stage_backing sb;
    struct coins_view chosen;

    /* 1. LEGACY: selector returns the legacy view verbatim. */
    utxo_projection_test_set_author(UTXO_AUTHOR_LEGACY);
    CVP_CHECK("select LEGACY ok",
              coins_view_select_connect_backing(&chosen, &sb, &legacy_view,
                                                p));
    CVP_CHECK("LEGACY backing identical vtable",
              chosen.vtable == legacy_view.vtable);
    CVP_CHECK("LEGACY backing identical impl",
              chosen.impl == legacy_view.impl);

    /* 2. STAGE: composite. Reads must be byte-identical to coins.db. */
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
    CVP_CHECK("select STAGE ok",
              coins_view_select_connect_backing(&chosen, &sb, &legacy_view,
                                                p));
    CVP_CHECK("STAGE backing is composite (not legacy vtable)",
              chosen.vtable != legacy_view.vtable);

    /* Drive the same RAM cache layer connect_block uses, over each backing. */
    {
        struct coins_view_cache stage_cache, legacy_cache;
        coins_view_cache_init(&stage_cache, &chosen);
        coins_view_cache_init(&legacy_cache, &legacy_view);

        struct coins cs, cl;
        bool gs = coins_view_cache_get_coins(&stage_cache, &T, &cs);
        bool gl = coins_view_cache_get_coins(&legacy_cache, &T, &cl);
        CVP_CHECK("STAGE/legacy both have T", gs && gl);
        if (gs && gl) {
            CVP_CHECK("STAGE T byte-identical to coins.db",
                      coins_byte_equal(&cs, &cl));
            coins_free(&cs); coins_free(&cl);
        } else { if (gs) coins_free(&cs); if (gl) coins_free(&cl); }

        /* Fully-spent U: absent in both. */
        CVP_CHECK("STAGE U absent == legacy U absent",
                  coins_view_cache_have_coins(&stage_cache, &U) ==
                  coins_view_cache_have_coins(&legacy_cache, &U));
        CVP_CHECK("STAGE U absent",
                  !coins_view_cache_have_coins(&stage_cache, &U));

        /* best_block delegates to coins.db under STAGE (prevblock invariant). */
        struct uint256 sb_best, lg_best;
        coins_view_cache_get_best_block(&stage_cache, &sb_best);
        coins_view_cache_get_best_block(&legacy_cache, &lg_best);
        CVP_CHECK("STAGE best_block == legacy best_block",
                  uint256_cmp(&sb_best, &lg_best) == 0);

        coins_view_cache_free(&stage_cache);
        coins_view_cache_free(&legacy_cache);
    }

    /* 3. Flip back to LEGACY: byte-identical to the legacy view again. */
    utxo_projection_test_set_author(UTXO_AUTHOR_LEGACY);
    CVP_CHECK("re-select LEGACY ok",
              coins_view_select_connect_backing(&chosen, &sb, &legacy_view,
                                                p));
    CVP_CHECK("re-LEGACY identical vtable",
              chosen.vtable == legacy_view.vtable);
    CVP_CHECK("re-LEGACY identical impl",
              chosen.impl == legacy_view.impl);

    /* Misconfiguration: STAGE author with a NULL projection must fall back
     * to the legacy backing (return false, *out = legacy) — never crash. */
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
    CVP_CHECK("STAGE+NULL proj falls back (returns false)",
              !coins_view_select_connect_backing(&chosen, &sb, &legacy_view,
                                                 NULL));
    CVP_CHECK("STAGE+NULL proj backing == legacy vtable",
              chosen.vtable == legacy_view.vtable);
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);  /* restore default */

    coins_view_sqlite_close(&cvs);
    sqlite3_close(db);
    return failures;
}

int test_coins_view_projection(void);
int test_coins_view_projection(void)
{
    int failures = 0;
    printf("test_coins_view_projection: B4 projection-backed coins_view\n");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/zcl_cvp_%d", (int)getpid());
    mkdir(dir, 0700);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",         dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    CVP_CHECK("open log+projection", log && p);
    if (!log || !p) goto done;

    utxo_projection_set_event_log(log);

    /* txid T (seed 0x70): coinbase tx, 3 outputs; vout 1 later spent. */
    CVP_CHECK("emit vout0", emit_add(0x70, 0, 5000000000LL, 250, true, 25));
    CVP_CHECK("emit vout1", emit_add(0x70, 1, 1500, 250, true, 10));
    CVP_CHECK("emit vout2", emit_add(0x70, 2, 2500, 250, true, 33));
    /* An unrelated txid U (seed 0x90): single output, will be fully spent. */
    CVP_CHECK("emit U", emit_add(0x90, 0, 777, 251, false, 5));
    CVP_CHECK("spend T:1", emit_spend(0x70, 1));
    CVP_CHECK("spend U:0", emit_spend(0x90, 0));
    CVP_CHECK("catch_up", utxo_projection_catch_up(p) != UINT64_MAX);

    struct coins_view_projection cvp;
    CVP_CHECK("init adapter", coins_view_projection_init(&cvp, p));

    /* 1. reconstruct T */
    uint8_t tT[32]; make_txid(tT, 0x70);
    struct uint256 T; memcpy(T.data, tT, 32);
    struct coins c;
    bool got = coins_view_get_coins(&cvp.view, &T, &c);
    CVP_CHECK("T get_coins true", got);
    if (got) {
        CVP_CHECK("T num_vout==3", c.num_vout == 3);
        CVP_CHECK("T version==1", c.version == 1);
        CVP_CHECK("T height==250", c.height == 250);
        CVP_CHECK("T is_coinbase", c.is_coinbase == true);
        if (c.num_vout == 3) {
            CVP_CHECK("T vout0 value", c.vout[0].value == 5000000000LL);
            CVP_CHECK("T vout0 script_len", c.vout[0].script_pub_key.size == 25);
            CVP_CHECK("T vout1 spent (null)", tx_out_is_null(&c.vout[1]));
            CVP_CHECK("T vout2 value", c.vout[2].value == 2500);
            CVP_CHECK("T vout2 script_len", c.vout[2].script_pub_key.size == 33);
            bool s0ok = true;
            for (uint32_t k = 0; k < 25; k++)
                if (c.vout[0].script_pub_key.data[k] !=
                    (uint8_t)((0x70 * 11 + k) & 0xFF)) { s0ok = false; break; }
            CVP_CHECK("T vout0 script bytes", s0ok);
        }
        coins_free(&c);
    }

    /* 2. have / absent */
    CVP_CHECK("have T (live)", coins_view_have_coins(&cvp.view, &T));

    uint8_t tU[32]; make_txid(tU, 0x90);
    struct uint256 U; memcpy(U.data, tU, 32);
    CVP_CHECK("U absent (all spent)", !coins_view_have_coins(&cvp.view, &U));
    struct coins cu;
    CVP_CHECK("U get_coins false", !coins_view_get_coins(&cvp.view, &U, &cu));
    CVP_CHECK("U coins_init'd (num_vout==0)", cu.num_vout == 0);

    uint8_t tX[32]; make_txid(tX, 0x12);
    struct uint256 X; memcpy(X.data, tX, 32);
    CVP_CHECK("unknown txid absent", !coins_view_have_coins(&cvp.view, &X));

    /* 3. B4-wiring: authority-gated connect-time backing selection. */
    failures += cvp_test_connect_backing_parity(p);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    if (failures == 0)
        printf("  all coins_view_projection checks passed\n");
    return failures;
}
