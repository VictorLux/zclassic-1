/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_apply_authorship — B3 guards for the single-writer UTXO
 * authority inversion.
 *
 * B3 hands authorship of the UTXO projection from the legacy
 * `update_coins()` projection emitters to `utxo_apply_stage`. Two
 * properties must hold for the flip to be safe:
 *
 *   1. ordering_equivalence — the stage emits a block's delta as
 *      "all adds, then all spends", whereas legacy emits per-tx
 *      (spend-then-add, tx by tx). Because the projection is a set and
 *      every UTXO key created in a block is unique, both orders fold to
 *      the SAME final set. We prove the two emission orders yield a
 *      byte-identical projection commitment — including the tricky case
 *      where a block creates an output and spends it in a later tx.
 *
 *   2. single_writer_gate — only one author writes at a time. With
 *      authority LEGACY the stage stays silent and the legacy emitters
 *      write; with authority STAGE the legacy emitters no-op. */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/utxo_projection.h"
#include "validation/update_coins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define UA_CHECK(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
} while (0)

static void ua_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "/tmp/zcl_ua_%s_%d", tag, (int)getpid());
}

static void ua_mkdir_p(const char *dir)
{
    mkdir(dir, 0700);
}

static void make_txid(uint8_t txid[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        txid[i] = (uint8_t)(seed + i);
}

/* Emit one ADD via the production projection emitter (writes to the
 * global event log set by utxo_projection_set_event_log). */
static bool emit_add(uint8_t seed, uint32_t vout, int64_t value,
                     uint32_t height, bool coinbase, uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t script[64];
    for (uint32_t k = 0; k < script_len && k < sizeof(script); k++)
        script[k] = (uint8_t)((seed * 7 + k) & 0xFF);
    return utxo_projection_emit_add(txid, vout, value, height, coinbase,
                                    script_len ? script : NULL, script_len);
}

static bool emit_spend(uint8_t seed, uint32_t vout)
{
    uint8_t txid[32]; make_txid(txid, seed);
    return utxo_projection_emit_spend(txid, vout);
}

/* ── Test 1: ordering_equivalence ──────────────────────────────────── */

static int run_ordering_equivalence(int *failures_out)
{
    int failures = 0;
    char dir[256];
    ua_tmpdir(dir, sizeof(dir), "order");
    ua_mkdir_p(dir);
    char logA[512], projA[512], logB[512], projB[512];
    snprintf(logA,  sizeof(logA),  "%s/a.log",  dir);
    snprintf(projA, sizeof(projA), "%s/a.db",   dir);
    snprintf(logB,  sizeof(logB),  "%s/b.log",  dir);
    snprintf(projB, sizeof(projB), "%s/b.db",   dir);

    event_log_t *la = event_log_open(logA);
    event_log_t *lb = event_log_open(logB);
    utxo_projection_t *pa = utxo_projection_open(projA, la);
    utxo_projection_t *pb = utxo_projection_open(projB, lb);
    UA_CHECK("order: open A/B", la && lb && pa && pb);
    if (!la || !lb || !pa || !pb) goto done;

    /* A synthetic block:
     *   tx0 (coinbase): adds o0 (seed 0xA0, vout 0)
     *   tx1           : adds o1 (seed 0xA1, vout 0)
     *   tx2           : spends o1, adds o2 (seed 0xA2, vout 0)
     * Final live set: {o0, o2}. o1 is created and spent in-block. */

    /* Path A — legacy per-tx interleaving (spend before add, tx by tx). */
    utxo_projection_set_event_log(la);
    UA_CHECK("order: A add o0",  emit_add(0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("order: A add o1",  emit_add(0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("order: A spend o1", emit_spend(0xA1, 0));
    UA_CHECK("order: A add o2",  emit_add(0xA2, 0, 900, 100, false, 12));

    /* Path B — stage order: all adds, then all spends. */
    utxo_projection_set_event_log(lb);
    UA_CHECK("order: B add o0",  emit_add(0xA0, 0, 5000000000LL, 100, true, 25));
    UA_CHECK("order: B add o1",  emit_add(0xA1, 0, 1000, 100, false, 10));
    UA_CHECK("order: B add o2",  emit_add(0xA2, 0, 900, 100, false, 12));
    UA_CHECK("order: B spend o1", emit_spend(0xA1, 0));

    UA_CHECK("order: A catch_up", utxo_projection_catch_up(pa) != UINT64_MAX);
    UA_CHECK("order: B catch_up", utxo_projection_catch_up(pb) != UINT64_MAX);
    UA_CHECK("order: A count == 2", utxo_projection_count(pa) == 2);
    UA_CHECK("order: B count == 2", utxo_projection_count(pb) == 2);

    uint8_t ca[32], cb[32];
    UA_CHECK("order: A commitment", utxo_projection_commitment(pa, ca) == 0);
    UA_CHECK("order: B commitment", utxo_projection_commitment(pb, cb) == 0);
    UA_CHECK("order: legacy-interleaved == stage-adds-first (byte-exact)",
             memcmp(ca, cb, 32) == 0);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(pa);
    utxo_projection_close(pb);
    event_log_close(la);
    event_log_close(lb);
done:
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

/* ── Test 2: single_writer_gate ────────────────────────────────────── */

static int run_single_writer_gate(int *failures_out)
{
    int failures = 0;
    char dir[256];
    ua_tmpdir(dir, sizeof(dir), "gate");
    ua_mkdir_p(dir);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",         dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    UA_CHECK("gate: open", log && p);
    if (!log || !p) goto done;

    utxo_projection_set_event_log(log);

    uint8_t txid[32]; make_txid(txid, 0x55);
    uint8_t script[8] = {1,2,3,4,5,6,7,8};

    /* Authority STAGE: the legacy projection emitter must yield (no event). */
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
    update_coins_emit_utxo_add_projection(txid, 0, 1234, 7, false, script, 8);
    UA_CHECK("gate: catch_up after STAGE emit",
             utxo_projection_catch_up(p) != UINT64_MAX);
    UA_CHECK("gate: STAGE silences legacy emitter (count==0)",
             utxo_projection_count(p) == 0);

    /* Authority LEGACY: the legacy emitter writes again. */
    utxo_projection_test_set_author(UTXO_AUTHOR_LEGACY);
    update_coins_emit_utxo_add_projection(txid, 0, 1234, 7, false, script, 8);
    UA_CHECK("gate: catch_up after LEGACY emit",
             utxo_projection_catch_up(p) != UINT64_MAX);
    UA_CHECK("gate: LEGACY emitter writes (count==1)",
             utxo_projection_count(p) == 1);

    /* Confirm the test-only selector, then restore the production default. */
    UA_CHECK("gate: LEGACY author selected",
             utxo_projection_get_author() == UTXO_AUTHOR_LEGACY);

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(p);
    event_log_close(log);
done:
    utxo_projection_test_set_author(UTXO_AUTHOR_STAGE);
    test_cleanup_tmpdir(dir);
    *failures_out += failures;
    return failures;
}

int test_utxo_apply_authorship(void);
int test_utxo_apply_authorship(void)
{
    int failures = 0;
    printf("test_utxo_apply_authorship: B3 single-writer authority\n");
    run_ordering_equivalence(&failures);
    run_single_writer_gate(&failures);
    if (failures == 0)
        printf("  all utxo_apply authorship checks passed\n");
    return failures;
}
