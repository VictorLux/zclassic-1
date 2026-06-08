/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Unit tests for Wave S S-8 utxo_apply stage. */

#include "test/test_helpers.h"

#include "bloom/merkle.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "jobs/utxo_apply_stage.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UV_CHECK(name, expr) do { \
    printf("utxo_apply: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

enum uv_fail_kind {
    UV_FAIL_NONE = 0,
    UV_FAIL_UNKNOWN,
    UV_FAIL_COLLISION,
    UV_FAIL_OVERFLOW,
};

struct external_utxo {
    struct uint256 txid;
    uint32_t vout;
    int64_t value;
};

struct synth_chain_uv {
    struct block_index *blocks;
    struct uint256     *hashes;
    struct block       *bodies;
    struct external_utxo *ext;
    int                 n;
    int                 upstream_fail_height;
    enum uv_fail_kind   fail_kind;
};

static int mkdir_p_uv(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void synthetic_txid(struct uint256 *out, int h, int salt)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0x80 + h);
    out->data[1] = (uint8_t)salt;
}

static bool make_tx(struct transaction *tx, int h, bool coinbase,
                    const struct uint256 *prev, int64_t in_value,
                    int64_t out_value)
{
    transaction_init(tx);
    if (!transaction_alloc(tx, 1, 1)) return false;
    if (coinbase) {
        outpoint_set_null(&tx->vin[0].prevout);
    } else {
        tx->vin[0].prevout.hash = *prev;
        tx->vin[0].prevout.n = 0;
        (void)in_value;
    }
    tx->vout[0].value = out_value;
    tx->vout[0].script_pub_key.size = 0;
    synthetic_txid(&tx->hash, h, coinbase ? 1 : 2);
    return true;
}

static bool make_body(struct synth_chain_uv *sc, int h)
{
    struct block *b = &sc->bodies[h];
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700002000u + (uint32_t)h);
    b->header.nBits = 0x1f07ffff;
    b->num_vtx = 2;
    b->vtx = zcl_calloc(2, sizeof(struct transaction), "uv_tx");
    if (!b->vtx) return false;

    struct uint256 prev;
    synthetic_txid(&prev, h, 9);
    sc->ext[h].txid = prev;
    sc->ext[h].vout = 0;
    sc->ext[h].value = 1000 + h;

    if (!make_tx(&b->vtx[0], h, true, NULL, 0, 50 + h)) return false;
    int64_t out_value = (sc->fail_kind == UV_FAIL_OVERFLOW && h == 1)
        ? 5000 : 900 + h;
    if (!make_tx(&b->vtx[1], h, false, &prev, sc->ext[h].value, out_value))
        return false;
    struct uint256 txids[2] = { b->vtx[0].hash, b->vtx[1].hash };
    b->header.hashMerkleRoot = compute_merkle_root(txids, 2);
    return true;
}

static bool synth_chain_uv_build(struct synth_chain_uv *sc, int n)
{
    sc->blocks = zcl_calloc((size_t)n, sizeof(struct block_index),
                            "uv_blocks");
    sc->hashes = zcl_calloc((size_t)n, sizeof(struct uint256),
                            "uv_hashes");
    sc->bodies = zcl_calloc((size_t)n, sizeof(struct block),
                            "uv_bodies");
    sc->ext = zcl_calloc((size_t)n, sizeof(struct external_utxo),
                         "uv_ext");
    if (!sc->blocks || !sc->hashes || !sc->bodies || !sc->ext)
        return false;
    for (int i = 0; i < n; i++) {
        if (!make_body(sc, i)) return false;
        block_header_get_hash(&sc->bodies[i].header, &sc->hashes[i]);
        block_index_init(&sc->blocks[i]);
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].hashMerkleRoot = sc->bodies[i].header.hashMerkleRoot;
        sc->blocks[i].nHeight = i;
        sc->blocks[i].nVersion = sc->bodies[i].header.nVersion;
        sc->blocks[i].nTime = sc->bodies[i].header.nTime;
        sc->blocks[i].nBits = sc->bodies[i].header.nBits;
        sc->blocks[i].nStatus = BLOCK_HAVE_DATA;
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void synth_chain_uv_free(struct synth_chain_uv *sc)
{
    if (sc->bodies) {
        for (int i = 0; i < sc->n; i++)
            block_free(&sc->bodies[i]);
    }
    free(sc->blocks);
    free(sc->hashes);
    free(sc->bodies);
    free(sc->ext);
    memset(sc, 0, sizeof(*sc));
}

static bool fake_reader(struct block *out, const struct block_index *bi,
                        const char *datadir, void *user)
{
    (void)datadir;
    struct synth_chain_uv *sc = user;
    if (!out || !bi || !sc || bi->nHeight < 0 || bi->nHeight >= sc->n)
        return false;
    return test_block_copy(out, &sc->bodies[bi->nHeight], "uv_tx_copy");
}

static bool fake_lookup(const struct uint256 *txid, uint32_t vout,
                        struct utxo_apply_lookup *out, void *user)
{
    struct synth_chain_uv *sc = user;
    memset(out, 0, sizeof(*out));
    if (!sc) return true;
    if (sc->fail_kind == UV_FAIL_COLLISION &&
        uint256_eq(txid, &sc->bodies[1].vtx[1].hash) && vout == 0) {
        out->found = true;
        out->value = 1;
        return true;
    }
    if (sc->fail_kind == UV_FAIL_UNKNOWN &&
        uint256_eq(txid, &sc->ext[1].txid) && vout == sc->ext[1].vout)
        return true;
    for (int i = 0; i < sc->n; i++) {
        if (sc->ext[i].vout == vout && uint256_eq(&sc->ext[i].txid, txid)) {
            out->found = true;
            out->value = sc->ext[i].value;
            return true;
        }
    }
    return true;
}

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool seed_proof_validate(sqlite3 *db, int n, int upstream_fail_height)
{
    if (!exec_sql(db,
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height                  INTEGER PRIMARY KEY,"
        "  status                  TEXT    NOT NULL,"
        "  ok                      INTEGER NOT NULL,"
        "  sapling_spends_total    INTEGER NOT NULL,"
        "  sapling_outputs_total   INTEGER NOT NULL,"
        "  sprout_joinsplits_total INTEGER NOT NULL,"
        "  first_failure_txid      BLOB,"
        "  first_failure_proof_type TEXT,"
        "  validated_at            INTEGER NOT NULL"
        ")"))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO proof_validate_log "
        "(height, status, ok, sapling_spends_total, sapling_outputs_total, "
        " sprout_joinsplits_total, validated_at) "
        "VALUES (?, ?, ?, 0, 0, 0, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    for (int h = 0; h < n; h++) {
        int ok = (h == upstream_fail_height) ? 0 : 1;
        sqlite3_bind_int(st, 1, h);
        sqlite3_bind_text(st, 2, ok ? "verified" : "proof_invalid",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, ok);
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
        "VALUES('proof_validate', ?, 1)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, n);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool log_row_at(sqlite3 *db, int height, int *out_ok,
                       char *out_status, size_t status_size,
                       char *out_kind, size_t kind_size)
{
    *out_ok = -1;
    if (out_status && status_size) out_status[0] = 0;
    if (out_kind && kind_size) out_kind[0] = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT ok, status, first_failure_kind "
        "FROM utxo_apply_log WHERE height = ?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out_ok = sqlite3_column_int(st, 0);
        const unsigned char *txt = sqlite3_column_text(st, 1);
        if (txt && out_status && status_size)
            snprintf(out_status, status_size, "%s", (const char *)txt);
        const unsigned char *kind = sqlite3_column_text(st, 2);
        if (kind && out_kind && kind_size)
            snprintf(out_kind, kind_size, "%s", (const char *)kind);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

static int uv_setup(const char *tag, int n, enum uv_fail_kind fail_kind,
                    int upstream_fail_height, char *dir_out,
                    size_t dir_out_size, struct main_state *ms,
                    struct synth_chain_uv *sc)
{
    test_fmt_tmpdir(dir_out, dir_out_size, "utxo_apply", tag);
    mkdir_p_uv("./test-tmp");
    mkdir_p_uv(dir_out);
    if (!progress_store_open(dir_out)) return 1;

    memset(sc, 0, sizeof(*sc));
    sc->fail_kind = fail_kind;
    sc->upstream_fail_height = upstream_fail_height;
    memset(ms, 0, sizeof(*ms));
    active_chain_init(&ms->chain_active);
    if (!synth_chain_uv_build(sc, n)) return 2;
    active_chain_move_window_tip(&ms->chain_active, &sc->blocks[n - 1]);

    if (!seed_proof_validate(progress_store_db(), n, upstream_fail_height))
        return 3;
    if (!utxo_apply_stage_init(ms)) return 4;
    utxo_apply_stage_set_reader(fake_reader, sc);
    utxo_apply_stage_set_lookup(fake_lookup, sc);
    return 0;
}

static void uv_teardown(const char *dir, struct main_state *ms,
                        struct synth_chain_uv *sc)
{
    utxo_apply_stage_shutdown();
    active_chain_free(&ms->chain_active);
    synth_chain_uv_free(sc);
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

static void run_fail_case(int *failures_out, enum uv_fail_kind kind,
                          const char *expected_status,
                          uint64_t (*counter)(void))
{
    int failures = 0;
    char dir[256]; struct main_state ms; struct synth_chain_uv sc;
    blocker_clear("utxo_apply.apply_failed");
    UV_CHECK("failure: setup",
             uv_setup(expected_status, 3, kind, -1, dir, sizeof(dir),
                      &ms, &sc) == 0);
    UV_CHECK("failure: drains until failed height", utxo_apply_stage_drain(100) == 1);
    UV_CHECK("failure: counter == 1", counter() == 1);
    int ok = -1; char status[32]; char kindbuf[32];
    bool found = log_row_at(progress_store_db(), 1, &ok, status, sizeof(status),
                            kindbuf, sizeof(kindbuf));
    UV_CHECK("failure: h=1 row rolled back", !found && ok == -1);
    UV_CHECK("failure: cursor held at h=1", utxo_apply_stage_cursor() == 1);
    UV_CHECK("failure: typed blocker recorded",
             blocker_exists("utxo_apply.apply_failed"));
    /* A block that FAILED utxo_apply must never report success: this is what
     * keeps reducer_pending_body_is_accepted from accepting a stateful-invalid
     * block. The failed verdict blocks and rolls back its scratch row, so no
     * ok=0 row can masquerade as an applied height. */
    UV_CHECK("failure: succeeded_at(1) false (no committed row)",
             !utxo_apply_stage_succeeded_at(1));
    blocker_clear("utxo_apply.apply_failed");
    uv_teardown(dir, &ms, &sc);
    *failures_out += failures;
}

int test_utxo_apply_stage(void);
int test_utxo_apply_stage(void)
{
    printf("\n=== utxo_apply_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    {
        char dir[256]; struct main_state ms; struct synth_chain_uv sc;
        UV_CHECK("happy: setup",
                 uv_setup("happy", 3, UV_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        UV_CHECK("happy: drains 3", utxo_apply_stage_drain(100) == 3);
        UV_CHECK("happy: cursor at 3", utxo_apply_stage_cursor() == 3);
        UV_CHECK("happy: verified_total == 3",
                 utxo_apply_stage_verified_total() == 3);
        UV_CHECK("happy: added_total == 6",
                 utxo_apply_stage_outputs_added_total() == 6);
        UV_CHECK("happy: spent_total == 3",
                 utxo_apply_stage_outputs_spent_total() == 3);
        for (int h = 0; h < 3; h++) {
            int ok = -1; char status[32]; char kind[32];
            log_row_at(progress_store_db(), h, &ok, status, sizeof(status),
                       kind, sizeof(kind));
            UV_CHECK("happy: row ok=1", ok == 1);
            UV_CHECK("happy: row status verified",
                     strcmp(status, "verified") == 0);
            UV_CHECK("happy: failure kind null", kind[0] == 0);
        }
        /* The reducer front door (reducer_pending_body_is_accepted) gates
         * acceptance of an un-finalizable tip on this accessor: an applied
         * (ok=1) height reports success; an un-applied height and a negative
         * height report failure. This is the consensus gate added to close the
         * accept-on-HAVE_DATA hole. */
        UV_CHECK("happy: succeeded_at(2) true",
                 utxo_apply_stage_succeeded_at(2));
        UV_CHECK("happy: succeeded_at(99) false (no row)",
                 !utxo_apply_stage_succeeded_at(99));
        UV_CHECK("happy: succeeded_at(-1) false",
                 !utxo_apply_stage_succeeded_at(-1));
        UV_CHECK("happy: next step IDLE",
                 utxo_apply_stage_step_once() == JOB_IDLE);
        uv_teardown(dir, &ms, &sc);
    }

    run_fail_case(&failures, UV_FAIL_UNKNOWN, "spend_unknown_utxo",
                  utxo_apply_stage_spend_unknown_total);
    run_fail_case(&failures, UV_FAIL_COLLISION, "utxo_collision",
                  utxo_apply_stage_utxo_collision_total);
    run_fail_case(&failures, UV_FAIL_OVERFLOW, "value_overflow",
                  utxo_apply_stage_value_overflow_total);

    {
        char dir[256]; struct main_state ms; struct synth_chain_uv sc;
        UV_CHECK("upstream_failed: setup",
                 uv_setup("upstream", 3, UV_FAIL_NONE, 2, dir, sizeof(dir),
                          &ms, &sc) == 0);
        blocker_clear("utxo_apply.apply_failed");
        UV_CHECK("upstream_failed: drains until failed height",
                 utxo_apply_stage_drain(100) == 2);
        UV_CHECK("upstream_failed: counter == 1",
                 utxo_apply_stage_upstream_failed_total() == 1);
        int ok = -1; char status[32]; char kind[32];
        bool found = log_row_at(progress_store_db(), 2, &ok, status,
                                sizeof(status), kind, sizeof(kind));
        UV_CHECK("upstream_failed: h=2 row rolled back", !found && ok == -1);
        UV_CHECK("upstream_failed: cursor held at h=2",
                 utxo_apply_stage_cursor() == 2);
        UV_CHECK("upstream_failed: typed blocker recorded",
                 blocker_exists("utxo_apply.apply_failed"));
        blocker_clear("utxo_apply.apply_failed");
        uv_teardown(dir, &ms, &sc);
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_uv sc;
        UV_CHECK("idle: setup",
                 uv_setup("idle", 3, UV_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        sqlite3_exec(progress_store_db(),
            "UPDATE stage_cursor SET cursor=1 WHERE name='proof_validate'",
            NULL, NULL, NULL);
        UV_CHECK("idle: advances one", utxo_apply_stage_drain(100) == 1);
        UV_CHECK("idle: next step IDLE",
                 utxo_apply_stage_step_once() == JOB_IDLE);
        UV_CHECK("idle: cursor stays 1", utxo_apply_stage_cursor() == 1);
        uv_teardown(dir, &ms, &sc);
    }

    {
        UV_CHECK("guard: step_once with no init returns IDLE",
                 utxo_apply_stage_step_once() == JOB_IDLE);
        UV_CHECK("guard: init(NULL) rejected",
                 !utxo_apply_stage_init(NULL));
    }

    {
        char dir[256]; struct main_state ms; struct synth_chain_uv sc;
        UV_CHECK("dump: setup",
                 uv_setup("dump", 2, UV_FAIL_NONE, -1, dir, sizeof(dir),
                          &ms, &sc) == 0);
        utxo_apply_stage_drain(100);
        struct json_value v;
        json_init(&v);
        UV_CHECK("dump: returns true", utxo_apply_dump_state_json(&v, NULL));
        char buf[1024];
        size_t n = json_write(&v, buf, sizeof(buf));
        UV_CHECK("dump: serializes", n > 0 && n < sizeof(buf));
        UV_CHECK("dump: stage_name",
                 strstr(buf, "\"stage_name\":\"utxo_apply\"") != NULL);
        UV_CHECK("dump: cursor=2", strstr(buf, "\"cursor\":2") != NULL);
        UV_CHECK("dump: verified_total=2",
                 strstr(buf, "\"verified_total\":2") != NULL);
        json_free(&v);
        uv_teardown(dir, &ms, &sc);
    }

    printf("utxo_apply_stage tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
