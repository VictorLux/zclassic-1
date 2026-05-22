/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Wave S S-2 header_admit shadow stage
 * (app/services/src/header_admit_stage.c).
 *
 * Coverage:
 *   - init / shutdown round-trip; idempotent re-init
 *   - drain a 5-block synthetic chain → cursor = 5, log has 5 rows
 *   - extra drain after no-more-blocks → IDLE, cursor unchanged
 *   - missing-pprev → STAGE_BLOCKED with PERMANENT typed blocker
 *   - replay across progress_store close/reopen: cursor + log persist */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "services/header_admit_stage.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HA_CHECK(name, expr) do { \
    printf("header_admit: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int mkdir_p_ha(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void ha_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/header_admit_%d_%s",
             (int)getpid(), tag);
}

/* Build a chain of `n` synthetic block_index entries linked via pprev,
 * with deterministic hashes derived from height. Caller owns the
 * blocks + hashes arrays. */
struct synth_chain {
    struct block_index *blocks;
    struct uint256     *hashes;
    int                 n;
};

static bool synth_chain_build(struct synth_chain *sc, int n)
{
    memset(sc, 0, sizeof(*sc));
    sc->blocks = zcl_malloc(
        (size_t)n * sizeof(struct block_index), "synth_blocks");
    if (!sc->blocks) return false;
    sc->hashes = zcl_malloc(
        (size_t)n * sizeof(struct uint256), "synth_hashes");
    if (!sc->hashes) { free(sc->blocks); return false; }
    for (int i = 0; i < n; i++) {
        block_index_init(&sc->blocks[i]);
        memset(&sc->hashes[i], 0, sizeof(struct uint256));
        sc->hashes[i].data[0] = (uint8_t)(i & 0xFF);
        sc->hashes[i].data[1] = (uint8_t)((i >> 8) & 0xFF);
        sc->hashes[i].data[2] = 0xAB;  /* distinguish from null */
        sc->blocks[i].phashBlock = &sc->hashes[i];
        sc->blocks[i].nHeight = i;
        if (i > 0) sc->blocks[i].pprev = &sc->blocks[i - 1];
    }
    sc->n = n;
    return true;
}

static void synth_chain_free(struct synth_chain *sc)
{
    free(sc->blocks);
    free(sc->hashes);
    memset(sc, 0, sizeof(*sc));
}

/* SELECT COUNT(*) FROM header_admit_log. */
static int log_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM header_admit_log",
        -1, &st, NULL) != SQLITE_OK) return -1;
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* SELECT hash FROM header_admit_log WHERE height=?. Copies up to 32
 * bytes into `out` and sets `*found`. */
static bool log_hash_at(sqlite3 *db, int height,
                        uint8_t out[32], bool *found)
{
    *found = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT hash FROM header_admit_log WHERE height=?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, height);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int nb = sqlite3_column_bytes(st, 0);
        if (blob && nb == 32) {
            memcpy(out, blob, 32);
            *found = true;
        }
    }
    sqlite3_finalize(st);
    return true;
}

int test_header_admit_stage(void)
{
    printf("\n=== header_admit_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── happy path: drain a 5-block synthetic chain ───────────────── */
    {
        char dir[256];
        ha_tmpdir(dir, sizeof(dir), "happy");
        mkdir_p_ha(dir);

        HA_CHECK("progress_store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);

        struct synth_chain sc;
        HA_CHECK("synth chain builds", synth_chain_build(&sc, 5));
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("stage init", header_admit_stage_init(&ms));
        HA_CHECK("init is idempotent (same ms)",
                 header_admit_stage_init(&ms));

        int advanced = header_admit_stage_drain(100);
        HA_CHECK("drain advances 5 times", advanced == 5);
        HA_CHECK("cursor reaches 5",
                 header_admit_stage_cursor() == 5);
        HA_CHECK("admitted_total == 5",
                 header_admit_stage_admitted_total() == 5);

        sqlite3 *db = progress_store_db();
        HA_CHECK("log has 5 rows", log_row_count(db) == 5);

        /* Spot-check hashes round-trip. */
        for (int h = 0; h < 5; h++) {
            uint8_t got[32];
            bool found = false;
            log_hash_at(db, h, got, &found);
            if (!found) { failures++; printf("FAIL log_hash_at(%d) missing\n", h); continue; }
            HA_CHECK("logged hash matches synth hash",
                     memcmp(got, sc.hashes[h].data, 32) == 0);
        }

        /* Extra drain → IDLE, no change. */
        stage_result_t r = header_admit_stage_step_once();
        HA_CHECK("next step is IDLE", r == STAGE_IDLE);
        HA_CHECK("cursor unchanged after IDLE",
                 header_admit_stage_cursor() == 5);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── replay across reopen: cursor + log persist ────────────────── */
    {
        char dir[256];
        ha_tmpdir(dir, sizeof(dir), "replay");
        mkdir_p_ha(dir);

        HA_CHECK("replay: store opens", progress_store_open(dir));
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 3);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[2]);

        HA_CHECK("replay: init", header_admit_stage_init(&ms));
        HA_CHECK("replay: drain 3",
                 header_admit_stage_drain(100) == 3);

        /* Tear down: shutdown stage, close store. */
        header_admit_stage_shutdown();
        progress_store_close();

        /* Reopen. Cursor must be remembered. */
        HA_CHECK("replay: reopen store", progress_store_open(dir));
        HA_CHECK("replay: re-init stage",
                 header_admit_stage_init(&ms));
        /* Stage cursor reflects persisted state on first step. */
        stage_result_t r = header_admit_stage_step_once();
        HA_CHECK("replay: first step after reopen is IDLE (cursor=3)",
                 r == STAGE_IDLE);
        HA_CHECK("replay: cursor restored to 3",
                 header_admit_stage_cursor() == 3);

        sqlite3 *db = progress_store_db();
        HA_CHECK("replay: log still has 3 rows",
                 log_row_count(db) == 3);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── missing-pprev → STAGE_BLOCKED ─────────────────────────────── */
    {
        char dir[256];
        ha_tmpdir(dir, sizeof(dir), "blocked");
        mkdir_p_ha(dir);
        HA_CHECK("blocked: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 3);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[2]);
        /* Sabotage AFTER set_tip: a NULL pprev set first would make the
         * chain-walker stop at the broken link and leave chain[0] NULL,
         * which would short-circuit genesis admission to IDLE before
         * step 1 ever runs. Setting it after set_tip preserves the
         * chain array but still leaves bi->pprev NULL when step 1
         * inspects it. */
        sc.blocks[1].pprev = NULL;

        HA_CHECK("blocked: init", header_admit_stage_init(&ms));
        /* Step 0 (genesis) succeeds; step 1 hits missing pprev. */
        HA_CHECK("blocked: genesis admits OK",
                 header_admit_stage_step_once() == STAGE_ADVANCED);
        stage_result_t r = header_admit_stage_step_once();
        HA_CHECK("blocked: step 1 returns BLOCKED", r == STAGE_BLOCKED);
        HA_CHECK("blocked: cursor stuck at 1",
                 header_admit_stage_cursor() == 1);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── dump_state_json shape ─────────────────────────────────────── */
    {
        char dir[256];
        ha_tmpdir(dir, sizeof(dir), "dump");
        mkdir_p_ha(dir);
        progress_store_open(dir);

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 2);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[1]);

        header_admit_stage_init(&ms);
        header_admit_stage_drain(100);

        struct json_value v;
        json_init(&v);
        HA_CHECK("dump returns true",
                 header_admit_stage_dump_state_json(&v, NULL));
        char buf[1024];
        size_t n = json_write(&v, buf, sizeof(buf));
        HA_CHECK("dump serializes", n > 0 && n < sizeof(buf));
        HA_CHECK("dump reports initialised=true",
                 strstr(buf, "\"initialised\":true") != NULL);
        HA_CHECK("dump reports cursor",
                 strstr(buf, "\"cursor\":2") != NULL);
        HA_CHECK("dump reports admitted_total",
                 strstr(buf, "\"admitted_total\":2") != NULL);
        json_free(&v);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── pre-init guard ────────────────────────────────────────────── */
    {
        HA_CHECK("step_once with no init returns IDLE",
                 header_admit_stage_step_once() == STAGE_IDLE);
        HA_CHECK("init(NULL) rejected",
                 !header_admit_stage_init(NULL));
    }

    printf("header_admit_stage: %d failures\n", failures);
    return failures;
}
