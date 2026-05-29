/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Tests for the diff_with_legacy_shadow use case.
 *
 * The use case takes two block_log_port views and reports first
 * divergence. The test uses two block_log_file instances opened on
 * separate tmp dirs as the two sides — one playing "primary", one
 * "shadow" — and exercises:
 *   - converged (both sides have identical blocks at h=0..N)
 *   - shadow_missing (primary ahead of shadow)
 *   - primary_missing (shadow ahead of primary)
 *   - divergent (same height, different bytes)
 *   - empty_range (one side empty)
 */

#include "test/test_helpers.h"

#include "adapters/outbound/persistence/block_log_file.h"
#include "application/operations/diff_with_legacy_shadow.h"
#include "ports/block_log_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DLS_CHECK(name, expr) do {                       \
    printf("diff_with_legacy_shadow: %s... ", (name));   \
    if ((expr)) { printf("OK\n"); }                      \
    else { printf("FAIL\n"); failures++; }               \
} while (0)

static void make_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_dls_XXXXXX");
    if (!mkdtemp(buf)) { perror("mkdtemp"); buf[0] = '\0'; }
}

static void append_block(struct block_log_port *p, uint32_t h, uint8_t seed,
                         const uint8_t *bytes, size_t len)
{
    struct block_hash hash; memset(hash.bytes, 0, 32); hash.bytes[0] = seed;
    p->append(p->self, h, &hash, bytes, len);
}

int test_diff_with_legacy_shadow(void)
{
    int failures = 0;

    /* ── 1. Both empty → EMPTY_RANGE. */
    {
        char d1[64], d2[64]; make_tmpdir(d1, sizeof d1); make_tmpdir(d2, sizeof d2);
        struct block_log_file *h1 = NULL, *h2 = NULL;
        struct block_log_port p1 = {0}, p2 = {0};
        block_log_file_open(d1, &h1, &p1);
        block_log_file_open(d2, &h2, &p2);

        struct diff_with_legacy_shadow_inputs in = {
            .primary = &p1, .shadow = &p2,
            .start_height = 0, .end_height = UINT32_MAX,
        };
        struct diff_with_legacy_shadow_report rep;
        struct zcl_result r = diff_with_legacy_shadow(&in, &rep);
        DLS_CHECK("both empty -> EMPTY_RANGE",
                  r.ok && rep.status == DIFF_STATUS_EMPTY_RANGE);

        block_log_file_close(h1); block_log_file_close(h2);
        test_rm_rf(d1); test_rm_rf(d2);
    }

    /* ── 2. Converged: same N blocks on both sides. */
    {
        char d1[64], d2[64]; make_tmpdir(d1, sizeof d1); make_tmpdir(d2, sizeof d2);
        struct block_log_file *h1 = NULL, *h2 = NULL;
        struct block_log_port p1 = {0}, p2 = {0};
        block_log_file_open(d1, &h1, &p1);
        block_log_file_open(d2, &h2, &p2);

        uint8_t b0[] = "block-0", b1[] = "block-1", b2[] = "block-2";
        append_block(&p1, 0, 0xa0, b0, sizeof b0);
        append_block(&p2, 0, 0xa0, b0, sizeof b0);
        append_block(&p1, 1, 0xa1, b1, sizeof b1);
        append_block(&p2, 1, 0xa1, b1, sizeof b1);
        append_block(&p1, 2, 0xa2, b2, sizeof b2);
        append_block(&p2, 2, 0xa2, b2, sizeof b2);

        struct diff_with_legacy_shadow_inputs in = {
            .primary = &p1, .shadow = &p2,
            .start_height = 0, .end_height = UINT32_MAX,
        };
        struct diff_with_legacy_shadow_report rep;
        diff_with_legacy_shadow(&in, &rep);
        DLS_CHECK("3 matching blocks -> CONVERGED",
                  rep.status == DIFF_STATUS_CONVERGED);
        DLS_CHECK("checked_count = 3", rep.checked_count == 3);
        DLS_CHECK("primary_tip = 2", rep.primary_tip == 2);
        DLS_CHECK("shadow_tip = 2", rep.shadow_tip == 2);

        block_log_file_close(h1); block_log_file_close(h2);
        test_rm_rf(d1); test_rm_rf(d2);
    }

    /* ── 3. Shadow missing: primary has h=5 that shadow doesn't.
     *    We compare an explicit range so the missing height is
     *    inside it. */
    {
        char d1[64], d2[64]; make_tmpdir(d1, sizeof d1); make_tmpdir(d2, sizeof d2);
        struct block_log_file *h1 = NULL, *h2 = NULL;
        struct block_log_port p1 = {0}, p2 = {0};
        block_log_file_open(d1, &h1, &p1);
        block_log_file_open(d2, &h2, &p2);

        uint8_t common[] = "common";
        uint8_t p_only[] = "primary-only";
        append_block(&p1, 0, 0xb0, common, sizeof common);
        append_block(&p2, 0, 0xb0, common, sizeof common);
        append_block(&p1, 5, 0xb5, p_only, sizeof p_only);
        /* shadow does NOT get h=5 */

        struct diff_with_legacy_shadow_inputs in = {
            .primary = &p1, .shadow = &p2,
            .start_height = 0, .end_height = 5,
        };
        struct diff_with_legacy_shadow_report rep;
        diff_with_legacy_shadow(&in, &rep);
        DLS_CHECK("primary has h=5, shadow doesn't -> SHADOW_MISSING",
                  rep.status == DIFF_STATUS_SHADOW_MISSING);
        DLS_CHECK("first_divergent_height = 5",
                  rep.first_divergent_height == 5);

        block_log_file_close(h1); block_log_file_close(h2);
        test_rm_rf(d1); test_rm_rf(d2);
    }

    /* ── 4. Primary missing: symmetric to #3. */
    {
        char d1[64], d2[64]; make_tmpdir(d1, sizeof d1); make_tmpdir(d2, sizeof d2);
        struct block_log_file *h1 = NULL, *h2 = NULL;
        struct block_log_port p1 = {0}, p2 = {0};
        block_log_file_open(d1, &h1, &p1);
        block_log_file_open(d2, &h2, &p2);

        uint8_t common[] = "common";
        uint8_t s_only[] = "shadow-only";
        append_block(&p1, 0, 0xc0, common, sizeof common);
        append_block(&p2, 0, 0xc0, common, sizeof common);
        append_block(&p2, 3, 0xc3, s_only, sizeof s_only);

        struct diff_with_legacy_shadow_inputs in = {
            .primary = &p1, .shadow = &p2,
            .start_height = 0, .end_height = 3,
        };
        struct diff_with_legacy_shadow_report rep;
        diff_with_legacy_shadow(&in, &rep);
        DLS_CHECK("shadow has h=3, primary doesn't -> PRIMARY_MISSING",
                  rep.status == DIFF_STATUS_PRIMARY_MISSING);
        DLS_CHECK("first_divergent_height = 3 (primary missing)",
                  rep.first_divergent_height == 3);

        block_log_file_close(h1); block_log_file_close(h2);
        test_rm_rf(d1); test_rm_rf(d2);
    }

    /* ── 5. Divergent: same height, different bytes. */
    {
        char d1[64], d2[64]; make_tmpdir(d1, sizeof d1); make_tmpdir(d2, sizeof d2);
        struct block_log_file *h1 = NULL, *h2 = NULL;
        struct block_log_port p1 = {0}, p2 = {0};
        block_log_file_open(d1, &h1, &p1);
        block_log_file_open(d2, &h2, &p2);

        uint8_t v1[] = "primary-bytes";
        uint8_t v2[] = "shadow-bytes-differ";
        /* Two sides put DIFFERENT hashes (and bytes) at the same
         * height. Our adapter keys by hash; read_at_height returns
         * the latest hash at that height — so each side gives its
         * own bytes back. The diff use case calls read_at_height,
         * not read_by_hash. */
        append_block(&p1, 0, 0xd1, v1, sizeof v1);
        append_block(&p2, 0, 0xd2, v2, sizeof v2);

        struct diff_with_legacy_shadow_inputs in = {
            .primary = &p1, .shadow = &p2,
            .start_height = 0, .end_height = 0,
        };
        struct diff_with_legacy_shadow_report rep;
        diff_with_legacy_shadow(&in, &rep);
        DLS_CHECK("same height, different bytes -> DIVERGENT",
                  rep.status == DIFF_STATUS_DIVERGENT);
        DLS_CHECK("first_divergent_height = 0",
                  rep.first_divergent_height == 0);

        block_log_file_close(h1); block_log_file_close(h2);
        test_rm_rf(d1); test_rm_rf(d2);
    }

    /* ── 6. NULL guards. */
    {
        struct diff_with_legacy_shadow_report rep;
        struct zcl_result r = diff_with_legacy_shadow(NULL, &rep);
        DLS_CHECK("NULL inputs -> ERR_NULL_ARG",
                  !r.ok && r.code == DIFF_ERR_NULL_ARG);
    }

    return failures;
}
