/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * B8 live-preflight verification harness — the UTXO-commitment parity gate.
 *
 * The cutover (workstream B) flips chain authority from the legacy coins.db
 * onto the append-only event log + its pure-fold projections. Before that
 * flip the operator runs `cutoverpreflight`, a READ-ONLY go/no-go that ANDs
 * a conservative set of gates. The newest gate — added with this test — is
 * the UTXO-set commitment parity gate:
 *
 *     SHA3-256 over the entire legacy coins.db `utxos` table
 *         == SHA3-256 over the log-folded utxo_projection
 *
 * byte-for-byte. Both sides use the identical canonical record encoding
 * (txid|vout_le|value_le|script_len_le|script|height_le|is_coinbase, ordered
 * by (txid,vout) — see lib/coins/src/utxo_commitment.c and
 * storage/utxo_projection.h). A match PROVES the projection reproduces the
 * live UTXO set exactly, so moving UTXO authority onto the projection is
 * safe; a mismatch must REFUSE the flip.
 *
 * This test does NOT touch the live node. It builds an in-process fixture —
 * an event log + utxo_projection AND a legacy `utxos` table holding the SAME
 * UTXO set — and asserts the exact comparison the gate makes:
 *
 *   1. PARITY: same set ⇒ commitments equal + counts equal (gate = ok).
 *   2. TEETH (negative control): perturb ONE coin's value on the legacy side
 *      ⇒ commitments DIVERGE (gate = not ok). A predicate that always
 *      matched would be vacuous; this proves it has teeth.
 *   3. COUNT teeth: drop one coin from the legacy side ⇒ counts diverge.
 *
 * Scratch under ./test-tmp/ (no-/tmp convention).
 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/utxo_projection.h"
#include "coins/utxo_commitment.h"

#include <sqlite3.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PF_CHECK(name, expr) do {                       \
    printf("cutover_preflight: %s... ", (name));        \
    if ((expr)) printf("OK\n");                         \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

/* One UTXO in the shared fixture set. */
struct pf_utxo {
    uint32_t key;       /* drives the txid bytes */
    uint32_t vout;
    int64_t  value;
    uint32_t height;
    uint8_t  is_coinbase;
    uint8_t  script[16];
    uint32_t script_len;
};

static void pf_make_txid(uint8_t txid[32], uint32_t key)
{
    for (int i = 0; i < 32; i++)
        txid[i] = (uint8_t)((key >> ((i % 4) * 8)) & 0xFF);
}

/* Append one UTXO into the event log so the projection folds it. */
static bool pf_log_add(event_log_t *log, const struct pf_utxo *u)
{
    struct ev_utxo_add_hdr hdr = {0};
    pf_make_txid(hdr.txid, u->key);
    hdr.vout = u->vout;
    hdr.value = u->value;
    hdr.height = u->height;
    hdr.is_coinbase = u->is_coinbase ? 1 : 0;
    hdr.script_len = u->script_len;

    uint8_t buf[EV_UTXO_ADD_HDR_WIRE_LEN + 16];
    size_t out_len = 0;
    if (!ev_utxo_add_serialize(&hdr, u->script_len ? u->script : NULL,
                               buf, sizeof(buf), &out_len))
        return false;
    return event_log_append(log, EV_UTXO_ADD, buf, out_len) != UINT64_MAX;
}

/* Insert one UTXO into the legacy coins.db `utxos` table. */
static bool pf_legacy_insert(sqlite3 *db, const struct pf_utxo *u)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO utxos(txid,vout,value,script,height,is_coinbase)"
        " VALUES(?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    uint8_t txid[32];
    pf_make_txid(txid, u->key);
    sqlite3_bind_blob(st, 1, txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, (int)u->vout);
    sqlite3_bind_int64(st, 3, u->value);
    if (u->script_len)
        sqlite3_bind_blob(st, 4, u->script, (int)u->script_len,
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_int(st, 5, (int)u->height);
    sqlite3_bind_int(st, 6, u->is_coinbase ? 1 : 0);
    int rc = sqlite3_step(st);  // raw-sql-ok:test-fixture
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* Mirror of the gate's match predicate: commitments equal AND counts equal.
 * (push_cutover_utxo_commitment_gate_json computes exactly this.) */
static bool pf_gate_ok(sqlite3 *legacy_db, utxo_projection_t *proj,
                       bool *commit_match_out, bool *count_match_out)
{
    uint8_t legacy_hash[32] = {0};
    uint64_t legacy_count = 0;
    utxo_commitment_sha3_compute(legacy_db, legacy_hash, &legacy_count);

    uint8_t proj_hash[32] = {0};
    if (utxo_projection_commitment(proj, proj_hash) != 0) {
        if (commit_match_out) *commit_match_out = false;
        if (count_match_out) *count_match_out = false;
        return false;
    }
    uint64_t proj_count = utxo_projection_count(proj);

    bool cm = memcmp(legacy_hash, proj_hash, 32) == 0;
    bool nm = legacy_count == proj_count;
    if (commit_match_out) *commit_match_out = cm;
    if (count_match_out) *count_match_out = nm;
    return cm && nm;
}

static void pf_tmpdir(char *buf, size_t n)
{
    snprintf(buf, n, "./test-tmp/preflight_%d", (int)getpid());
}

int test_cutover_preflight(void);
int test_cutover_preflight(void)
{
    int failures = 0;
    printf("\n== test_cutover_preflight (utxo commitment parity gate) ==\n");

    mkdir("./test-tmp", 0700);
    char dir[256];
    pf_tmpdir(dir, sizeof(dir));
    mkdir(dir, 0700);
    char log_path[400], proj_path[400];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log", dir);
    snprintf(proj_path, sizeof(proj_path), "%s/proj.db",    dir);

    /* ── shared fixture UTXO set ──────────────────────────────────── */
    enum { NU = 40 };
    struct pf_utxo set[NU];
    for (int i = 0; i < NU; i++) {
        set[i].key = 0xA000u + (uint32_t)i;
        set[i].vout = (uint32_t)(i % 3);
        set[i].value = 100000LL + (int64_t)i * 777;
        set[i].height = (uint32_t)(500000 + i);
        set[i].is_coinbase = (i % 7 == 0) ? 1 : 0;
        set[i].script_len = (uint32_t)(4 + (i % 12));
        for (uint32_t k = 0; k < set[i].script_len; k++)
            set[i].script[k] = (uint8_t)((set[i].key * 5 + k) & 0xFF);
    }

    /* ── build the log-folded projection ──────────────────────────── */
    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *proj = utxo_projection_open(proj_path, log);
    PF_CHECK("open event log + utxo projection", log && proj);
    if (!log || !proj) {
        if (proj) utxo_projection_close(proj);
        if (log) event_log_close(log);
        goto done;
    }

    bool appended_all = true;
    for (int i = 0; i < NU; i++)
        appended_all = appended_all && pf_log_add(log, &set[i]);
    PF_CHECK("append all UTXOs to event log", appended_all);
    PF_CHECK("projection catch_up folds the log",
             utxo_projection_catch_up(proj) != UINT64_MAX);
    PF_CHECK("projection count == fixture size",
             utxo_projection_count(proj) == (uint64_t)NU);

    /* ── build the legacy coins.db with the SAME set ──────────────── */
    sqlite3 *legacy = NULL;
    int orc = sqlite3_open(":memory:", &legacy);
    PF_CHECK("open in-memory legacy coins.db", orc == SQLITE_OK && legacy);
    if (orc == SQLITE_OK && legacy) {
        int crc = sqlite3_exec(legacy,
            "CREATE TABLE node_state (key TEXT PRIMARY KEY, value BLOB);"
            "CREATE TABLE utxos ("
            "  txid BLOB, vout INT, value INT, script BLOB,"
            "  script_type INT, address_hash BLOB,"
            "  height INT, is_coinbase INT);",
            NULL, NULL, NULL);
        PF_CHECK("create legacy utxos table", crc == SQLITE_OK);

        bool inserted_all = true;
        for (int i = 0; i < NU; i++)
            inserted_all = inserted_all && pf_legacy_insert(legacy, &set[i]);
        PF_CHECK("insert all UTXOs into legacy table", inserted_all);

        /* (1) PARITY — same set ⇒ gate reads ok. */
        {
            bool cm = false, nm = false;
            bool ok = pf_gate_ok(legacy, proj, &cm, &nm);
            PF_CHECK("PARITY: legacy SHA3 == projection SHA3", cm);
            PF_CHECK("PARITY: legacy count == projection count", nm);
            PF_CHECK("PARITY: gate go/no-go == GO", ok);
        }

        /* (1b) SEED — anchor-seed a FRESH empty projection directly from
         * the legacy `utxos` table (no event log involved) and assert
         * byte-exact parity + the one-time guard. This is the live
         * utxo_commitment fix: the projection holds only tail deltas
         * (154 of 1.34M live); seeding copies the full set from coins.db. */
        {
            char seed_log_path[400], seed_proj_path[400];
            snprintf(seed_log_path,  sizeof(seed_log_path),
                     "%s/seed_events.log", dir);
            snprintf(seed_proj_path, sizeof(seed_proj_path),
                     "%s/seed_proj.db", dir);
            event_log_t *seed_log = event_log_open(seed_log_path);
            utxo_projection_t *seed_proj =
                utxo_projection_open(seed_proj_path, seed_log);
            PF_CHECK("SEED: open fresh projection", seed_log && seed_proj);
            if (seed_log && seed_proj) {
                PF_CHECK("SEED: fresh projection starts empty",
                         utxo_projection_count(seed_proj) == 0);
                int64_t seeded =
                    utxo_projection_seed_from_legacy(seed_proj, legacy);
                PF_CHECK("SEED: seeded row count == fixture size",
                         seeded == (int64_t)NU);
                bool cm = false, nm = false;
                bool ok = pf_gate_ok(legacy, seed_proj, &cm, &nm);
                PF_CHECK("SEED: seeded SHA3 == legacy SHA3", cm);
                PF_CHECK("SEED: seeded count == legacy count", nm);
                PF_CHECK("SEED: gate go/no-go == GO after seed", ok);
                /* one-time guard: a second seed must REFUSE. */
                int64_t again =
                    utxo_projection_seed_from_legacy(seed_proj, legacy);
                PF_CHECK("SEED: second seed refuses (one-time guard)",
                         again < 0);
                bool cm2 = false;
                (void)pf_gate_ok(legacy, seed_proj, &cm2, NULL);
                PF_CHECK("SEED: parity intact after refused re-seed", cm2);
            }
            if (seed_proj) utxo_projection_close(seed_proj);
            if (seed_log) event_log_close(seed_log);
        }

        /* (2) TEETH — perturb ONE coin's value on the legacy side; the
         * commitment MUST diverge (proves the gate is not vacuous). */
        {
            sqlite3_stmt *st = NULL;
            uint8_t txid0[32];
            pf_make_txid(txid0, set[0].key);
            int prc = sqlite3_prepare_v2(legacy,
                "UPDATE utxos SET value = value + 1 WHERE txid = ? AND vout = ?",
                -1, &st, NULL);
            if (prc == SQLITE_OK) {
                sqlite3_bind_blob(st, 1, txid0, 32, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 2, (int)set[0].vout);
                (void)sqlite3_step(st);  // raw-sql-ok:test-fixture
                sqlite3_finalize(st);
            }
            bool cm = true, nm = false;
            bool ok = pf_gate_ok(legacy, proj, &cm, &nm);
            PF_CHECK("TEETH: a value perturbation is DETECTED (commit differs)",
                     !cm);
            PF_CHECK("TEETH: count still matches after value-only edit", nm);
            PF_CHECK("TEETH: gate go/no-go == NO-GO on divergence", !ok);

            /* repair: restore the value so we can test count teeth cleanly */
            sqlite3_stmt *rst = NULL;
            if (sqlite3_prepare_v2(legacy,
                "UPDATE utxos SET value = value - 1 WHERE txid = ? AND vout = ?",
                -1, &rst, NULL) == SQLITE_OK) {
                sqlite3_bind_blob(rst, 1, txid0, 32, SQLITE_TRANSIENT);
                sqlite3_bind_int(rst, 2, (int)set[0].vout);
                (void)sqlite3_step(rst);  // raw-sql-ok:test-fixture
                sqlite3_finalize(rst);
            }
            bool cm2 = false;
            (void)pf_gate_ok(legacy, proj, &cm2, NULL);
            PF_CHECK("TEETH: repairing the value restores parity", cm2);
        }

        /* (3) COUNT TEETH — drop one coin on the legacy side; counts must
         * diverge so a missing UTXO can't slip past the gate. */
        {
            sqlite3_stmt *st = NULL;
            uint8_t txidN[32];
            pf_make_txid(txidN, set[NU - 1].key);
            if (sqlite3_prepare_v2(legacy,
                "DELETE FROM utxos WHERE txid = ? AND vout = ?",
                -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_blob(st, 1, txidN, 32, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 2, (int)set[NU - 1].vout);
                (void)sqlite3_step(st);  // raw-sql-ok:test-fixture
                sqlite3_finalize(st);
            }
            bool cm = true, nm = true;
            bool ok = pf_gate_ok(legacy, proj, &cm, &nm);
            PF_CHECK("COUNT TEETH: a dropped UTXO is DETECTED (count differs)",
                     !nm);
            PF_CHECK("COUNT TEETH: gate go/no-go == NO-GO on count gap", !ok);
        }

        sqlite3_close(legacy);
    }

    utxo_projection_close(proj);
    event_log_close(log);

done:
    test_cleanup_tmpdir(dir);
    if (failures == 0)
        printf("test_cutover_preflight: all OK\n");
    else
        printf("test_cutover_preflight: %d FAILURE(S)\n", failures);
    return failures;
}
