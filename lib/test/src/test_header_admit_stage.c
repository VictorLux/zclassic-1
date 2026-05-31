/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Wave S S-2 header_admit shadow stage
 * (app/services/src/header_admit_stage.c).
 *
 * Coverage:
 *   - init / shutdown round-trip; idempotent re-init
 *   - drain a 5-block synthetic chain → cursor = 5, log has 5 rows
 *   - extra drain after no-more-blocks → IDLE, cursor unchanged
 *   - missing-pprev → JOB_BLOCKED with PERMANENT typed blocker
 *   - replay across progress_store close/reopen: cursor + log persist */

#include "test/test_helpers.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "primitives/block.h"
#include "jobs/header_admit_stage.h"
#include "services/cutover_modes.h"
#include "services/header_admit_inbox.h"
#include "jobs/validate_headers_stage.h"
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

static bool ha_stamp_cursor(sqlite3 *db, const char *name, int64_t cursor)
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

struct auth_hook_state {
    int calls;
    int height;
};

static bool auth_observer(struct main_state *ms,
                          struct block_index *bi,
                          void *user)
{
    struct auth_hook_state *st = user;
    if (!st || !ms || !bi)
        return false;
    st->calls++;
    st->height = bi->nHeight;
    return true;
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

int test_header_admit_stage(void)
{
    printf("\n=== header_admit_stage tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── cutover mode defaults to AUTHORITATIVE (post-flip, step 13) ── */
    {
        HA_CHECK("mode defaults to AUTHORITATIVE",
                 header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE);
        header_admit_set_mode((header_admit_mode_t)999);
        HA_CHECK("invalid mode coerces to SHADOW",
                 header_admit_get_mode() == HEADER_ADMIT_MODE_SHADOW);
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_AUTHORITATIVE,
                                          CUTOVER_STAGE_MODE_AUTHORITATIVE);
        HA_CHECK("combined mode sets header admit authoritative",
                 header_admit_get_mode() ==
                     HEADER_ADMIT_MODE_AUTHORITATIVE);
        HA_CHECK("combined mode sets validate headers authoritative",
                 validate_headers_get_mode() ==
                     VALIDATE_HEADERS_MODE_AUTHORITATIVE);
        cutover_modes_set_header_pipeline(CUTOVER_STAGE_MODE_SHADOW,
                                          CUTOVER_STAGE_MODE_SHADOW);
    }

    /* ── happy path: drain a 5-block synthetic chain ───────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","happy");
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
        job_result_t r = header_admit_stage_step_once();
        HA_CHECK("next step is IDLE", r == JOB_IDLE);
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
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","replay");
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
        job_result_t r = header_admit_stage_step_once();
        HA_CHECK("replay: first step after reopen is IDLE (cursor=3)",
                 r == JOB_IDLE);
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

    /* ── missing-pprev → JOB_BLOCKED ─────────────────────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","blocked");
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
                 header_admit_stage_step_once() == JOB_ADVANCED);
        job_result_t r = header_admit_stage_step_once();
        HA_CHECK("blocked: step 1 returns BLOCKED", r == JOB_BLOCKED);
        HA_CHECK("blocked: cursor stuck at 1",
                 header_admit_stage_cursor() == 1);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── authoritative path is gated behind mode flag ──────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","authoritative");
        mkdir_p_ha(dir);
        HA_CHECK("auth: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 2);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[1]);

        struct auth_hook_state st = {0, -1};
        header_admit_stage_set_authoritative_hook(auth_observer, &st);
        header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);

        HA_CHECK("auth: init", header_admit_stage_init(&ms));
        HA_CHECK("auth: step calls authoritative hook",
                 header_admit_stage_step_once() == JOB_ADVANCED);
        HA_CHECK("auth: hook saw height 0",
                 st.calls == 1 && st.height == 0);
        HA_CHECK("auth: log still records row",
                 log_row_count(progress_store_db()) == 1);

        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── producer path: a staged raw header CREATES a block_index ──────
     * Reducer step 2. AUTHORITATIVE-only: when the active chain has no
     * block at the needed height, a raw header pushed through the inbox
     * lets the stage build the block_index via add_to_block_index — the
     * reducer extends the chain without legacy accept_block_header.
     * Dormant in SHADOW (the hash-hint path never sets has_header). */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","producer");
        mkdir_p_ha(dir);
        HA_CHECK("producer: store opens", progress_store_open(dir));

        struct main_state ms;
        main_state_init(&ms);

        /* Parent at height 0, in the map AND the active chain. */
        struct uint256 parent_hash;
        memset(&parent_hash, 0, sizeof(parent_hash));
        parent_hash.data[0] = 0x77;
        struct block_index *parent = chainstate_insert_block_index(
            (struct chainstate *)&ms, &parent_hash);
        HA_CHECK("producer: parent inserted", parent != NULL);
        if (parent) {
            parent->nHeight = 0;
            parent->nStatus = BLOCK_VALID_TREE;
            active_chain_set_tip(&ms.chain_active, parent);
            ms.pindex_best_header = parent;
        }

        header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
        HA_CHECK("producer: stage init", header_admit_stage_init(&ms));
        HA_CHECK("producer: produced_total starts at 0",
                 header_admit_stage_produced_total() == 0);

        /* A child header for height 1, carried as raw bytes in the inbox. */
        struct block_header child;
        block_header_init(&child);
        child.hashPrevBlock = parent_hash;
        child.nTime = 1;
        child.nBits = 1;
        child.nSolutionSize = 0;
        struct uint256 child_hash;
        block_header_get_hash(&child, &child_hash);

        struct header_admit_msg msg;
        memset(&msg, 0, sizeof(msg));
        msg.height = 1;
        msg.hash = child_hash;
        msg.has_header = true;
        msg.header = child;
        HA_CHECK("producer: push raw header to inbox",
                 mailbox_header_admit_push(&msg));

        /* Before production, the child is not in the block_index. */
        HA_CHECK("producer: child absent from map pre-step",
                 block_map_find(&ms.map_block_index, &child_hash) == NULL);

        /* Step 1 drains the inbox (staging the header) and admits the
         * height-0 parent (already in the active chain). Step 2 hits the
         * absent height 1 → producer path CREATES the entry. */
        HA_CHECK("producer: step 0 admits parent (ADVANCED)",
                 header_admit_stage_step_once() == JOB_ADVANCED);
        HA_CHECK("producer: step 1 produces+admits child (ADVANCED)",
                 header_admit_stage_step_once() == JOB_ADVANCED);

        HA_CHECK("producer: produced_total == 1",
                 header_admit_stage_produced_total() == 1);
        struct block_index *created =
            block_map_find(&ms.map_block_index, &child_hash);
        HA_CHECK("producer: child now in block_index", created != NULL);
        HA_CHECK("producer: created entry at height 1, VALID_TREE",
                 created != NULL && created->nHeight == 1 &&
                 (created->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE);
        HA_CHECK("producer: created links to parent",
                 created != NULL && created->pprev == parent);
        HA_CHECK("producer: cursor advanced to 2",
                 header_admit_stage_cursor() == 2);

        /* SHADOW dormancy: a fresh header pushed in SHADOW is NOT staged,
         * so no production happens (the path is gated AUTHORITATIVE). */
        header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
        struct block_header child2;
        block_header_init(&child2);
        child2.hashPrevBlock = child_hash;
        child2.nTime = 2;
        child2.nBits = 1;
        struct uint256 child2_hash;
        block_header_get_hash(&child2, &child2_hash);
        struct header_admit_msg msg2;
        memset(&msg2, 0, sizeof(msg2));
        msg2.height = 2;
        msg2.hash = child2_hash;
        msg2.has_header = true;
        msg2.header = child2;
        HA_CHECK("producer: push raw header in SHADOW",
                 mailbox_header_admit_push(&msg2));
        HA_CHECK("producer: SHADOW step is IDLE (no production)",
                 header_admit_stage_step_once() == JOB_IDLE);
        HA_CHECK("producer: SHADOW left produced_total at 1",
                 header_admit_stage_produced_total() == 1);
        HA_CHECK("producer: SHADOW did NOT create child2",
                 block_map_find(&ms.map_block_index, &child2_hash) == NULL);

        header_admit_stage_shutdown();
        main_state_free(&ms);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── authoritative legacy ingress guard detects missing stage row ─ */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","cutover_guard");
        mkdir_p_ha(dir);
        HA_CHECK("guard: store opens", progress_store_open(dir));

        const struct chain_params *params = chain_params_get();
        if (!params) {
            printf("header_admit: guard skipped (no chain params)\n");
        } else {
            struct main_state ms;
            main_state_init(&ms);

            struct uint256 parent_hash;
            memset(&parent_hash, 0, sizeof(parent_hash));
            parent_hash.data[0] = 0x55;
            struct block_index *parent = chainstate_insert_block_index(
                (struct chainstate *)&ms, &parent_hash);
            HA_CHECK("guard: parent inserted", parent != NULL);
            if (parent) {
                parent->nHeight = 0;
                parent->nStatus = BLOCK_VALID_TREE;
                active_chain_set_tip(&ms.chain_active, parent);
                ms.pindex_best_header = parent;
            }

            HA_CHECK("guard: stage init creates schema",
                     header_admit_stage_init(&ms));

            struct block_header hdr;
            block_header_init(&hdr);
            hdr.hashPrevBlock = parent_hash;
            hdr.nTime = 1;
            hdr.nBits = 1;
            hdr.nSolutionSize = 0;

            int guard_events = 0;
            event_log_init();
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            HA_CHECK("guard: observer registers",
                     event_observe(EV_CUTOVER_GUARD_DIVERGED,
                                   cutover_guard_observer,
                                   &guard_events));

            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *out = NULL;
            header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
            bool ok = accept_block_header(&hdr, &vs, &ms, params, &out);
            HA_CHECK("guard: legacy path rejects missing stage row", !ok);
            HA_CHECK("guard: divergence event emitted",
                     guard_events == 1);

            header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            header_admit_stage_shutdown();
            main_state_free(&ms);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── cold-import fast-forward lets first post-anchor header in ─── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","fast_forward_guard");
        mkdir_p_ha(dir);
        HA_CHECK("ff: store opens", progress_store_open(dir));

        const struct chain_params *params = chain_params_get();
        if (!params) {
            printf("header_admit: fast-forward guard skipped (no chain params)\n");
        } else {
            sqlite3 *db = progress_store_db();
            HA_CHECK("ff: stamp header cursor",
                     ha_stamp_cursor(db, "header_admit", 1));
            HA_CHECK("ff: stamp validate cursor",
                     ha_stamp_cursor(db, "validate_headers", 1));
            int32_t legacy_tip = 0;
            HA_CHECK("ff: stamp legacy attach tip meta",
                     progress_meta_set(db, "legacy_attach_tip_height",
                                       &legacy_tip, sizeof(legacy_tip)));

            struct main_state ms;
            main_state_init(&ms);

            struct uint256 parent_hash;
            memset(&parent_hash, 0, sizeof(parent_hash));
            parent_hash.data[0] = 0x66;
            struct block_index *parent = chainstate_insert_block_index(
                (struct chainstate *)&ms, &parent_hash);
            HA_CHECK("ff: parent inserted", parent != NULL);
            if (parent) {
                parent->nHeight = 0;
                parent->nStatus = BLOCK_VALID_TREE;
                parent->nTime = 0; /* force contextual-header skip */
                active_chain_set_tip(&ms.chain_active, parent);
                ms.pindex_best_header = parent;
            }

            HA_CHECK("ff: header stage init", header_admit_stage_init(&ms));
            HA_CHECK("ff: validate stage init",
                     validate_headers_stage_init(&ms));
            HA_CHECK("ff: header cursor observes persisted fast-forward",
                     header_admit_stage_cursor() == 1);
            HA_CHECK("ff: validate cursor observes persisted fast-forward",
                     validate_headers_stage_cursor() == 1);

            struct block_header hdr;
            block_header_init(&hdr);
            hdr.hashPrevBlock = parent_hash;
            hdr.nTime = 1;
            hdr.nBits = 1;
            hdr.nSolutionSize = 0;

            int guard_events = 0;
            event_log_init();
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            HA_CHECK("ff: observer registers",
                     event_observe(EV_CUTOVER_GUARD_DIVERGED,
                                   cutover_guard_observer,
                                   &guard_events));

            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *out = NULL;
            header_admit_set_mode(HEADER_ADMIT_MODE_AUTHORITATIVE);
            validate_headers_set_mode(VALIDATE_HEADERS_MODE_AUTHORITATIVE);
            bool ok = accept_block_header(&hdr, &vs, &ms, params, &out);
            HA_CHECK("ff: legacy path accepts height at fast-forward cursor",
                     ok && out != NULL && out->nHeight == 1);
            HA_CHECK("ff: no divergence event emitted", guard_events == 0);

            header_admit_set_mode(HEADER_ADMIT_MODE_SHADOW);
            validate_headers_set_mode(VALIDATE_HEADERS_MODE_SHADOW);
            event_clear_observers(EV_CUTOVER_GUARD_DIVERGED);
            validate_headers_stage_shutdown();
            header_admit_stage_shutdown();
            main_state_free(&ms);
        }

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── dump_state_json shape ─────────────────────────────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","dump");
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
                 header_admit_stage_step_once() == JOB_IDLE);
        HA_CHECK("init(NULL) rejected",
                 !header_admit_stage_init(NULL));
    }

    /* ── S-11 diff: NOT_READY before init ──────────────────────────── */
    {
        /* Note: previous block left state un-init, and pre-init guard
         * confirmed that. progress.kv is also closed at this point. */
        struct header_admit_diff_report rep;
        HA_CHECK("diff: returns true with NOT_READY before init",
                 header_admit_stage_diff(-1, -1, &rep) &&
                 rep.status == HEADER_ADMIT_DIFF_NOT_READY);
        HA_CHECK("diff: NOT_READY report has -1 sentinels",
                 rep.log_max_height == -1 &&
                 rep.chain_tip_height == -1 &&
                 rep.first_divergent_height == -1);
    }

    /* ── S-11 diff: CONVERGED on fully drained chain ───────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_conv");
        mkdir_p_ha(dir);
        HA_CHECK("diff_conv: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 5);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("diff_conv: stage init", header_admit_stage_init(&ms));
        HA_CHECK("diff_conv: drain 5",
                 header_admit_stage_drain(100) == 5);

        struct header_admit_diff_report rep;
        HA_CHECK("diff_conv: diff returns true",
                 header_admit_stage_diff(-1, -1, &rep));
        HA_CHECK("diff_conv: status CONVERGED",
                 rep.status == HEADER_ADMIT_DIFF_CONVERGED);
        HA_CHECK("diff_conv: matched 5",
                 rep.match_count == 5 && rep.checked_count == 5);
        HA_CHECK("diff_conv: no mismatches",
                 rep.mismatch_count == 0 &&
                 rep.missing_in_log_count == 0 &&
                 rep.missing_in_chain_count == 0);
        HA_CHECK("diff_conv: first_divergent_height == -1",
                 rep.first_divergent_height == -1);
        HA_CHECK("diff_conv: bounds resolved [0..4]",
                 rep.start_height == 0 && rep.end_height == 4);
        HA_CHECK("diff_conv: log_max=4 chain_tip=4 cursor=5",
                 rep.log_max_height == 4 &&
                 rep.chain_tip_height == 4 &&
                 rep.cursor == 5);
        HA_CHECK("diff_conv: no samples on converged",
                 rep.sample_count == 0);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: CHAIN_AHEAD when cursor lags behind tip ────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_chainahead");
        mkdir_p_ha(dir);
        HA_CHECK("diff_chainahead: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 5);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("diff_chainahead: init", header_admit_stage_init(&ms));
        /* Only drain 3 of 5 — heights 3,4 will be missing from log. */
        for (int i = 0; i < 3; i++)
            HA_CHECK("diff_chainahead: step advances",
                     header_admit_stage_step_once() == JOB_ADVANCED);

        struct header_admit_diff_report rep;
        HA_CHECK("diff_chainahead: diff(-1,-1) uses min(log,chain) for end",
                 header_admit_stage_diff(-1, -1, &rep));
        HA_CHECK("diff_chainahead: auto-end=2 (log_max=2)",
                 rep.end_height == 2 &&
                 rep.log_max_height == 2 &&
                 rep.chain_tip_height == 4);
        HA_CHECK("diff_chainahead: auto bound makes it converged",
                 rep.status == HEADER_ADMIT_DIFF_CONVERGED);

        /* Explicit end=4 should reveal heights 3,4 missing in log. */
        HA_CHECK("diff_chainahead: explicit diff(0,4)",
                 header_admit_stage_diff(0, 4, &rep));
        HA_CHECK("diff_chainahead: status CHAIN_AHEAD",
                 rep.status == HEADER_ADMIT_DIFF_CHAIN_AHEAD);
        HA_CHECK("diff_chainahead: 3 matches + 2 missing_in_log",
                 rep.match_count == 3 &&
                 rep.missing_in_log_count == 2 &&
                 rep.missing_in_chain_count == 0 &&
                 rep.mismatch_count == 0);
        HA_CHECK("diff_chainahead: first_divergent=3",
                 rep.first_divergent_height == 3);
        HA_CHECK("diff_chainahead: 2 samples for the 2 missings",
                 rep.sample_count == 2);
        HA_CHECK("diff_chainahead: sample[0] chain_present log_absent at h=3",
                 rep.samples[0].height == 3 &&
                 !rep.samples[0].log_present &&
                 rep.samples[0].chain_present);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: DIVERGENT on real hash mismatch ────────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_div");
        mkdir_p_ha(dir);
        HA_CHECK("diff_div: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 5);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("diff_div: init", header_admit_stage_init(&ms));
        HA_CHECK("diff_div: drain 5",
                 header_admit_stage_drain(100) == 5);

        /* Mutate chain hashes at heights 2 and 3 AFTER admission.
         * The log still has the original hashes; the chain now has
         * different ones — that's a real DIVERGENT. */
        sc.hashes[2].data[31] ^= 0xFF;
        sc.hashes[3].data[31] ^= 0xFF;

        struct header_admit_diff_report rep;
        HA_CHECK("diff_div: diff(0,4) returns true",
                 header_admit_stage_diff(0, 4, &rep));
        HA_CHECK("diff_div: status DIVERGENT",
                 rep.status == HEADER_ADMIT_DIFF_DIVERGENT);
        HA_CHECK("diff_div: 3 matches + 2 mismatches",
                 rep.match_count == 3 && rep.mismatch_count == 2);
        HA_CHECK("diff_div: first_divergent=2",
                 rep.first_divergent_height == 2);
        HA_CHECK("diff_div: 2 samples for the 2 mismatches",
                 rep.sample_count == 2);
        HA_CHECK("diff_div: sample[0] both-present at h=2",
                 rep.samples[0].height == 2 &&
                 rep.samples[0].log_present &&
                 rep.samples[0].chain_present);
        HA_CHECK("diff_div: sample[0] hashes differ",
                 memcmp(rep.samples[0].log_hash,
                        rep.samples[0].chain_hash, 32) != 0);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: LOG_AHEAD when chain shrinks below log ─────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_logahead");
        mkdir_p_ha(dir);
        HA_CHECK("diff_logahead: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 5);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("diff_logahead: init", header_admit_stage_init(&ms));
        HA_CHECK("diff_logahead: drain 5",
                 header_admit_stage_drain(100) == 5);

        /* Shrink the chain back to height 2 — log still has 0..4. */
        HA_CHECK("diff_logahead: shrink chain to height 2",
                 active_chain_set_tip(&ms.chain_active, &sc.blocks[2]));

        struct header_admit_diff_report rep;
        HA_CHECK("diff_logahead: diff(0,4)",
                 header_admit_stage_diff(0, 4, &rep));
        HA_CHECK("diff_logahead: status LOG_AHEAD",
                 rep.status == HEADER_ADMIT_DIFF_LOG_AHEAD);
        HA_CHECK("diff_logahead: 3 matches + 2 missing_in_chain",
                 rep.match_count == 3 &&
                 rep.missing_in_chain_count == 2 &&
                 rep.missing_in_log_count == 0 &&
                 rep.mismatch_count == 0);
        HA_CHECK("diff_logahead: first_divergent=3",
                 rep.first_divergent_height == 3);
        HA_CHECK("diff_logahead: chain_tip=2 log_max=4",
                 rep.chain_tip_height == 2 &&
                 rep.log_max_height == 4);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: EMPTY when range inverted ──────────────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_empty");
        mkdir_p_ha(dir);
        progress_store_open(dir);

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 3);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[2]);

        header_admit_stage_init(&ms);
        header_admit_stage_drain(100);

        struct header_admit_diff_report rep;
        HA_CHECK("diff_empty: explicit start>end → EMPTY",
                 header_admit_stage_diff(10, 5, &rep) &&
                 rep.status == HEADER_ADMIT_DIFF_EMPTY);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: sample_count caps at MAX_SAMPLES ───────────────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_cap");
        mkdir_p_ha(dir);
        progress_store_open(dir);

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        const int N = HEADER_ADMIT_DIFF_MAX_SAMPLES + 8;  /* 40 */
        struct synth_chain sc;
        synth_chain_build(&sc, N);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[N - 1]);

        header_admit_stage_init(&ms);
        header_admit_stage_drain(N * 2);

        /* Flip last byte on every chain hash → every height diverges. */
        for (int i = 0; i < N; i++) sc.hashes[i].data[31] ^= 0xAA;

        struct header_admit_diff_report rep;
        HA_CHECK("diff_cap: diff(0,N-1)",
                 header_admit_stage_diff(0, N - 1, &rep));
        HA_CHECK("diff_cap: status DIVERGENT",
                 rep.status == HEADER_ADMIT_DIFF_DIVERGENT);
        HA_CHECK("diff_cap: mismatch_count == N",
                 rep.mismatch_count == N);
        HA_CHECK("diff_cap: sample_count capped at MAX_SAMPLES",
                 rep.sample_count == HEADER_ADMIT_DIFF_MAX_SAMPLES);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── S-11 diff: auto range is the recent tail, not genesis ─────── */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","diff_tail");
        mkdir_p_ha(dir);
        progress_store_open(dir);

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        const int N = HEADER_ADMIT_DIFF_MAX_RANGE + 7;
        struct synth_chain sc;
        synth_chain_build(&sc, N);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[N - 1]);

        header_admit_stage_init(&ms);
        header_admit_stage_drain(N * 2);

        struct header_admit_diff_report rep;
        HA_CHECK("diff_tail: auto diff succeeds",
                 header_admit_stage_diff(-1, -1, &rep));
        HA_CHECK("diff_tail: auto start is recent tail",
                 rep.start_height == N - HEADER_ADMIT_DIFF_MAX_RANGE);
        HA_CHECK("diff_tail: auto end is chain tip",
                 rep.end_height == N - 1);
        HA_CHECK("diff_tail: auto checked capped range",
                 rep.checked_count == HEADER_ADMIT_DIFF_MAX_RANGE &&
                 rep.match_count == HEADER_ADMIT_DIFF_MAX_RANGE);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── Reorg self-heal: DIVERGENT below a matching tip → CONVERGED ──
     * Mirrors the live first_divergent_height=3129671 case at small
     * scale: the stale log row sits BELOW the (still-matching) tip. The
     * forward-only stage would never revisit it; the new reorg-rewind
     * must detect it, rewind the cursor to the fork point, and re-admit
     * (INSERT OR REPLACE) so the canonical hash overwrites the stale one. */
    {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "header_admit","reorg_heal");
        mkdir_p_ha(dir);
        HA_CHECK("reorg_heal: store opens", progress_store_open(dir));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        active_chain_init(&ms.chain_active);
        struct synth_chain sc;
        synth_chain_build(&sc, 5);
        active_chain_set_tip(&ms.chain_active, &sc.blocks[4]);

        HA_CHECK("reorg_heal: init", header_admit_stage_init(&ms));
        HA_CHECK("reorg_heal: drain 5 → cursor=5",
                 header_admit_stage_drain(100) == 5 &&
                 header_admit_stage_cursor() == 5);
        HA_CHECK("reorg_heal: rewind counter starts at 0",
                 header_admit_stage_reorg_rewind_total() == 0);

        /* Reorg height 2 ONLY (cursor-3). The tip rows (3,4) still match
         * the active chain, so a tip-only check would miss this — the
         * divergence is strictly below the matching tip. */
        sc.hashes[2].data[31] ^= 0xFF;

        struct header_admit_diff_report rep;
        HA_CHECK("reorg_heal: pre status DIVERGENT",
                 header_admit_stage_diff(0, 4, &rep) &&
                 rep.status == HEADER_ADMIT_DIFF_DIVERGENT);
        HA_CHECK("reorg_heal: pre mismatch=1 at height 2",
                 rep.mismatch_count == 1 &&
                 rep.first_divergent_height == 2);

        /* Drive steps: the first step's reorg-rewind rewinds the cursor
         * to 2; subsequent steps re-admit 2,3,4 → cursor back to 5. */
        for (int i = 0; i < 8; i++)
            (void)header_admit_stage_step_once();

        HA_CHECK("reorg_heal: rewind fired (counter incremented)",
                 header_admit_stage_reorg_rewind_total() >= 1);
        HA_CHECK("reorg_heal: cursor restored to 5",
                 header_admit_stage_cursor() == 5);
        HA_CHECK("reorg_heal: post status CONVERGED",
                 header_admit_stage_diff(0, 4, &rep) &&
                 rep.status == HEADER_ADMIT_DIFF_CONVERGED);
        HA_CHECK("reorg_heal: post mismatch=0, 5 matches",
                 rep.mismatch_count == 0 && rep.match_count == 5);

        header_admit_stage_shutdown();
        active_chain_free(&ms.chain_active);
        synth_chain_free(&sc);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    printf("header_admit_stage: %d failures\n", failures);
    return failures;
}
