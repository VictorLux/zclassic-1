/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the conservation_diff_job (cutover finish, Item 3).
 *
 * The Job drives the shadow-pipeline `diffed` counter — the safety proof
 * that no FED block was silently dropped — by reading the canonical block
 * from legacy and reconciling it byte-for-byte against the fed (shadow)
 * block. THE HONESTY GUARDRAIL: `diffed` advances ONLY on a genuine match;
 * a real mismatch must NOT increment and must return JOB_BLOCKED.
 *
 * Coverage (no live zclassicd — both ports are fixtures):
 *   (a) fed block matches canonical            -> diffed++, JOB_ADVANCED
 *   (b) genuine byte mismatch                  -> diffed unchanged,
 *                                                 JOB_BLOCKED at the height;
 *                                                 reorg re-drive recovers it
 *   (c) cursor persists across init cycles     -> advance, re-init same
 *                                                 datadir, cursor unchanged
 *   (d) zclassicd unreachable (RPC IO error)   -> JOB_BLOCKED, no fake
 *                                                 progress, diffed unchanged
 *   (e) legacy behind the feeder               -> JOB_IDLE (not faked)
 *
 * Each independent case uses its own tmp datadir (progress.kv is one
 * process / one datadir, but close+open switches it cleanly), so cases do
 * not couple through the persisted cursor — except case (c), which
 * deliberately re-inits the SAME datadir to prove persistence.
 */

#include "test/test_helpers.h"

#include "adapters/inbound/shadow_conservation.h"
#include "jobs/conservation_diff_job.h"
#include "jobs/job.h"
#include "ports/block_log_port.h"
#include "storage/progress_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CDJ_CHECK(name, expr) do {                       \
    printf("conservation_diff_job: %s... ", (name));     \
    if ((expr)) { printf("OK\n"); }                      \
    else { printf("FAIL\n"); failures++; }               \
} while (0)

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_cdj_XXXXXX");
    if (!mkdtemp(buf)) { perror("mkdtemp"); buf[0] = '\0'; }
}

/* ── In-memory mock block_log_port ──────────────────────────────────── */

#define MOCK_MAX_H 16

struct mock_log {
    uint8_t  bytes[MOCK_MAX_H][64];
    size_t   len[MOCK_MAX_H];
    bool     present[MOCK_MAX_H];
    int      tip;            /* highest present height, -1 if empty */
    bool     unreachable;    /* simulate zclassicd down: read -> IO error */
};

static void mock_set(struct mock_log *m, uint32_t h,
                     const void *bytes, size_t len)
{
    if (h >= MOCK_MAX_H || len > sizeof m->bytes[0]) return;
    memcpy(m->bytes[h], bytes, len);
    m->len[h] = len;
    m->present[h] = true;
    if ((int)h > m->tip) m->tip = (int)h;
}

static struct zcl_result mock_read_at_height(void *self, uint32_t h,
                                             const uint8_t **out, size_t *len)
{
    struct mock_log *m = self;
    *out = NULL; *len = 0;
    if (m->unreachable)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "mock: zclassicd unreachable");
    if (h >= MOCK_MAX_H || !m->present[h])
        return ZCL_ERR(BLOCK_LOG_ERR_NOT_FOUND, "mock: no block at %u", h);
    *out = m->bytes[h];
    *len = m->len[h];
    return ZCL_OK;
}

static uint32_t mock_tip(void *self)
{
    struct mock_log *m = self;
    return m->tip < 0 ? UINT32_MAX : (uint32_t)m->tip;
}

static struct zcl_result mock_unsupported(void *self, uint32_t h,
                                          const struct block_hash *hash,
                                          const uint8_t *b, size_t l)
{
    (void)self; (void)h; (void)hash; (void)b; (void)l;
    return ZCL_ERR(BLOCK_LOG_ERR_NOT_SUPPORTED, "mock: unsupported");
}

static struct block_log_port mock_port(struct mock_log *m)
{
    struct block_log_port p = {0};
    p.self           = m;
    p.append         = mock_unsupported;
    p.read_at_height = mock_read_at_height;
    p.tip_height     = mock_tip;
    /* read_by_hash / iter_from unused by the Job. */
    return p;
}

/* Open a fresh datadir (switching progress.kv), init the Job there, and
 * override both ports with fixtures. Returns true on success. */
static bool cdj_open(const char *datadir,
                     struct block_log_port *shadow,
                     struct block_log_port *legacy)
{
    conservation_diff_job_shutdown();
    progress_store_close();
    if (!progress_store_open(datadir)) return false;
    if (!conservation_diff_job_init(datadir)) return false;
    conservation_diff_job_set_shadow_port(shadow);
    conservation_diff_job_set_legacy_port(legacy);
    return true;
}

static const uint8_t B0[] = "block-zero";
static const uint8_t B1[] = "block-one";
static const uint8_t B2[] = "block-two";

int test_conservation_diff_job(void)
{
    int failures = 0;

    /* ── (a) Match at h=0 → diffed++, JOB_ADVANCED. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        shadow_conservation_reset();
        struct mock_log fed = { .tip = -1 }, leg = { .tip = -1 };
        mock_set(&fed, 0, B0, sizeof B0);
        mock_set(&leg, 0, B0, sizeof B0);   /* identical → genuine match */
        struct block_log_port sp = mock_port(&fed), lp = mock_port(&leg);

        CDJ_CHECK("setup ok (match case)", cdj_open(dir, &sp, &lp));

        unsigned long fed_c, diffed0, sk;
        shadow_conservation_snapshot(&fed_c, &diffed0, &sk);

        job_result_t r = conservation_diff_job_step_once();
        unsigned long diffed1;
        shadow_conservation_snapshot(&fed_c, &diffed1, &sk);

        CDJ_CHECK("match -> JOB_ADVANCED", r == JOB_ADVANCED);
        CDJ_CHECK("match -> diffed incremented by exactly 1",
                  diffed1 == diffed0 + 1);
        CDJ_CHECK("match -> Job diffed_total == 1",
                  conservation_diff_job_diffed_total() == 1);
        CDJ_CHECK("match -> cursor advanced to 1",
                  conservation_diff_job_cursor() == 1);
        test_rm_rf(dir);
    }

    /* ── (b) Genuine mismatch at h=0 → diffed NOT incremented, BLOCKED;
     *    then a reorg makes the sides agree and the SAME cursor advances
     *    (proves BLOCKED is transient, not a permanent wedge). */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        shadow_conservation_reset();
        struct mock_log fed = { .tip = -1 }, leg = { .tip = -1 };
        mock_set(&fed, 0, B0, sizeof B0);
        mock_set(&leg, 0, B1, sizeof B1);   /* different bytes → divergence */
        struct block_log_port sp = mock_port(&fed), lp = mock_port(&leg);

        CDJ_CHECK("setup ok (mismatch case)", cdj_open(dir, &sp, &lp));

        unsigned long fed_c, diffed0, sk;
        shadow_conservation_snapshot(&fed_c, &diffed0, &sk);

        job_result_t r = conservation_diff_job_step_once();
        unsigned long diffed1;
        shadow_conservation_snapshot(&fed_c, &diffed1, &sk);

        CDJ_CHECK("mismatch -> JOB_BLOCKED", r == JOB_BLOCKED);
        CDJ_CHECK("mismatch -> diffed UNCHANGED (not faked)",
                  diffed1 == diffed0);
        CDJ_CHECK("mismatch -> Job diffed_total == 0",
                  conservation_diff_job_diffed_total() == 0);
        CDJ_CHECK("mismatch -> cursor UNCHANGED at 0",
                  conservation_diff_job_cursor() == 0);
        CDJ_CHECK("mismatch -> last_blocked_height == 0",
                  conservation_diff_job_last_blocked_height() == 0);

        /* Reorg re-drive: legacy now agrees with fed. */
        mock_set(&leg, 0, B0, sizeof B0);
        job_result_t r2 = conservation_diff_job_step_once();
        unsigned long diffed2;
        shadow_conservation_snapshot(&fed_c, &diffed2, &sk);
        CDJ_CHECK("reorg re-drive -> JOB_ADVANCED", r2 == JOB_ADVANCED);
        CDJ_CHECK("reorg re-drive -> cursor advances to 1",
                  conservation_diff_job_cursor() == 1);
        CDJ_CHECK("reorg re-drive -> diffed now incremented", diffed2 == 1);
        test_rm_rf(dir);
    }

    /* ── (c) Cursor persistence: advance once, re-init the SAME datadir,
     *    confirm the cursor is read back from progress.kv (no rewind),
     *    then drain the rest. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        shadow_conservation_reset();
        struct mock_log fed = { .tip = -1 }, leg = { .tip = -1 };
        mock_set(&fed, 0, B0, sizeof B0); mock_set(&leg, 0, B0, sizeof B0);
        mock_set(&fed, 1, B1, sizeof B1); mock_set(&leg, 1, B1, sizeof B1);
        mock_set(&fed, 2, B2, sizeof B2); mock_set(&leg, 2, B2, sizeof B2);
        struct block_log_port sp = mock_port(&fed), lp = mock_port(&leg);

        CDJ_CHECK("setup ok (persistence case)", cdj_open(dir, &sp, &lp));

        job_result_t r = conservation_diff_job_step_once();
        CDJ_CHECK("first step advances (h=0)",
                  r == JOB_ADVANCED && conservation_diff_job_cursor() == 1);

        /* Re-init the SAME datadir (simulates a restart). The persisted
         * cursor (1) lives in progress.kv; the first step after re-init
         * must resume from there — NOT rewind to 0.
         *
         * Unambiguous proof: after re-init make legacy DISAGREE at h=0
         * only. A wrongful rewind to 0 would re-diff h=0 and BLOCK; a
         * correct resume at 1 sails past h=0 and drains h=1,h=2. */
        conservation_diff_job_shutdown();
        CDJ_CHECK("re-init same datadir",
                  conservation_diff_job_init(dir));
        mock_set(&leg, 0, B2, sizeof B2);   /* h=0 would now MISMATCH */
        struct block_log_port sp2 = mock_port(&fed), lp2 = mock_port(&leg);
        conservation_diff_job_set_shadow_port(&sp2);
        conservation_diff_job_set_legacy_port(&lp2);

        int advanced = conservation_diff_job_drain(8);
        CDJ_CHECK("resume-at-1 drains remaining 2 (h=1,2), never re-diffs h=0",
                  advanced == 2);
        CDJ_CHECK("cursor now at 3 (proves no rewind to 0)",
                  conservation_diff_job_cursor() == 3);

        job_result_t r3 = conservation_diff_job_step_once();
        CDJ_CHECK("no more fed -> JOB_IDLE, cursor holds at 3",
                  r3 == JOB_IDLE && conservation_diff_job_cursor() == 3);
        test_rm_rf(dir);
    }

    /* ── (d) zclassicd unreachable → JOB_BLOCKED, no fake progress. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        shadow_conservation_reset();
        struct mock_log fed = { .tip = -1 };
        struct mock_log leg = { .tip = -1, .unreachable = true };
        mock_set(&fed, 0, B0, sizeof B0);   /* a block IS fed at the cursor */
        struct block_log_port sp = mock_port(&fed), lp = mock_port(&leg);

        CDJ_CHECK("setup ok (unreachable case)", cdj_open(dir, &sp, &lp));

        unsigned long fed_c, diffed0, sk;
        shadow_conservation_snapshot(&fed_c, &diffed0, &sk);

        job_result_t r = conservation_diff_job_step_once();
        unsigned long diffed1;
        shadow_conservation_snapshot(&fed_c, &diffed1, &sk);

        CDJ_CHECK("unreachable -> JOB_BLOCKED", r == JOB_BLOCKED);
        CDJ_CHECK("unreachable -> diffed UNCHANGED (no fake progress)",
                  diffed1 == diffed0);
        CDJ_CHECK("unreachable -> Job diffed_total == 0",
                  conservation_diff_job_diffed_total() == 0);
        CDJ_CHECK("unreachable -> cursor UNCHANGED at 0",
                  conservation_diff_job_cursor() == 0);
        CDJ_CHECK("unreachable -> last_blocked_height == 0",
                  conservation_diff_job_last_blocked_height() == 0);

        /* Recovery: zclassicd back + agrees → advances. */
        leg.unreachable = false;
        mock_set(&leg, 0, B0, sizeof B0);
        job_result_t r2 = conservation_diff_job_step_once();
        unsigned long diffed2;
        shadow_conservation_snapshot(&fed_c, &diffed2, &sk);
        CDJ_CHECK("recovered -> JOB_ADVANCED", r2 == JOB_ADVANCED);
        CDJ_CHECK("recovered -> diffed advances by 1", diffed2 == diffed0 + 1);
        test_rm_rf(dir);
    }

    /* ── (e) Legacy behind the feeder (fed has h, legacy NOT_FOUND) -> IDLE. */
    {
        char dir[64]; make_tmpdir(dir, sizeof dir);
        shadow_conservation_reset();
        struct mock_log fed = { .tip = -1 }, leg = { .tip = -1 };
        mock_set(&fed, 0, B0, sizeof B0);   /* fed has it, legacy doesn't */
        struct block_log_port sp = mock_port(&fed), lp = mock_port(&leg);

        CDJ_CHECK("setup ok (legacy-behind case)", cdj_open(dir, &sp, &lp));

        job_result_t r = conservation_diff_job_step_once();
        CDJ_CHECK("legacy behind feeder -> JOB_IDLE (not blocked/faked)",
                  r == JOB_IDLE);
        CDJ_CHECK("legacy behind -> diffed_total stays 0",
                  conservation_diff_job_diffed_total() == 0);
        CDJ_CHECK("legacy behind -> cursor stays 0",
                  conservation_diff_job_cursor() == 0);
        test_rm_rf(dir);
    }

    conservation_diff_job_shutdown();
    progress_store_close();
    return failures;
}
