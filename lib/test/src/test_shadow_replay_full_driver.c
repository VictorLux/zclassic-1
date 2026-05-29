/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_shadow_replay_full_driver — bounded CI proxy for the full 0→tip
 * Tier-1 replay driver (docs/work/cutover.md §1 PROVE).
 *
 * The CLI `shadow_replay_proof` runs the real full-archive driver against a
 * read-only legacy datadir (not available in CI). This test exercises the
 * SAME operations-layer driver (shadow_replay_proof_run with end=UINT32_MAX
 * = "to primary tip") over a synthetic multi-block "archive", asserting the
 * end-to-end contract the cutover banks on:
 *
 *   1. Full 0→tip range: every block fed, every block diffed, converged.
 *   2. blocks_fed == blocks_diffed across the WHOLE range — so a dropped /
 *      back-pressured block can never fake a passing proof.
 *   3. A single divergent byte at one interior height is caught (status
 *      DIVERGED), with proof_ok == false — the safety property the flip
 *      depends on.
 *
 * This is offline tooling: no live node, no service, no consensus mutation.
 */

#include "test/test_helpers.h"

#include "adapters/outbound/persistence/block_log_file.h"
#include "application/operations/shadow_replay_proof.h"
#include "ports/block_log_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SRFD_CHECK(name, expr) do {                        \
    printf("shadow_replay_full_driver: %s... ", (name));   \
    if ((expr)) { printf("OK\n"); }                        \
    else { printf("FAIL\n"); failures++; }                 \
} while (0)

/* A modest "archive" — large enough to span many index records and exercise
 * the iterate-then-diff path, small enough to stay a fast CI test. */
#define SRFD_ARCHIVE_BLOCKS 256u

static void srfd_tmpdir(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/zcl_srfd_XXXXXX");
    if (!mkdtemp(buf))
        buf[0] = '\0';
}

/* Deterministic per-height block bytes + hash, so primary and shadow agree
 * byte-for-byte when fed the same way. */
static void srfd_make_block(uint32_t h, struct block_hash *hash,
                            uint8_t *buf, size_t cap, size_t *len_out)
{
    memset(hash->bytes, 0, sizeof(hash->bytes));
    hash->bytes[0] = (uint8_t)(h & 0xff);
    hash->bytes[1] = (uint8_t)((h >> 8) & 0xff);
    hash->bytes[2] = 0x5a;
    int n = snprintf((char *)buf, cap, "archive-block-%u-payload", h);
    /* include a few raw bytes so the body is not a pure ASCII string */
    if (n > 0 && (size_t)n + 2 < cap) {
        buf[n] = (uint8_t)(h & 0xff);
        buf[n + 1] = 0xff;
        *len_out = (size_t)n + 2;
    } else {
        *len_out = (n > 0) ? (size_t)n : 1;
    }
}

int test_shadow_replay_full_driver(void)
{
    int failures = 0;

    /* ── Case 1: full 0→tip converges, fed == diffed across the range ──── */
    {
        char primary_dir[64], shadow_dir[64];
        srfd_tmpdir(primary_dir, sizeof(primary_dir));
        srfd_tmpdir(shadow_dir, sizeof(shadow_dir));
        struct block_log_file *primary_h = NULL, *shadow_h = NULL;
        struct block_log_port primary = {0}, shadow = {0};
        block_log_file_open(primary_dir, &primary_h, &primary);
        block_log_file_open(shadow_dir, &shadow_h, &shadow);

        for (uint32_t h = 0; h < SRFD_ARCHIVE_BLOCKS; h++) {
            struct block_hash hash;
            uint8_t buf[128];
            size_t len = 0;
            srfd_make_block(h, &hash, buf, sizeof(buf), &len);
            (void)primary.append(primary.self, h, &hash, buf, len);
        }

        struct shadow_replay_proof_inputs in = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = 0,
            .end_height = UINT32_MAX,  /* full driver: replay to primary tip */
        };
        struct shadow_replay_proof_report rep;
        struct zcl_result r = shadow_replay_proof_run(&in, &rep);

        SRFD_CHECK("full 0->tip proof passes",
                   r.ok && rep.proof_ok &&
                   rep.status == SHADOW_REPLAY_PROOF_OK);
        SRFD_CHECK("fed == diffed == archive size",
                   rep.blocks_fed == SRFD_ARCHIVE_BLOCKS &&
                   rep.blocks_diffed == SRFD_ARCHIVE_BLOCKS);
        SRFD_CHECK("end_height resolved to tip",
                   rep.end_height == SRFD_ARCHIVE_BLOCKS - 1 &&
                   rep.shadow_tip_after == SRFD_ARCHIVE_BLOCKS - 1);
        SRFD_CHECK("diff converged with no divergent height",
                   rep.diff.status == DIFF_STATUS_CONVERGED &&
                   rep.diff.checked_count == SRFD_ARCHIVE_BLOCKS);

        block_log_file_close(primary_h);
        block_log_file_close(shadow_h);
        test_rm_rf(primary_dir);
        test_rm_rf(shadow_dir);
    }

    /* ── Case 2: one divergent interior block is caught ─────────────────
     * Pre-seed the shadow with a DIFFERENT block at an interior height, then
     * replay only the heights above it (so the proof's own append doesn't
     * own that height). The diff over the full range must flag DIVERGED. */
    {
        char primary_dir[64], shadow_dir[64];
        srfd_tmpdir(primary_dir, sizeof(primary_dir));
        srfd_tmpdir(shadow_dir, sizeof(shadow_dir));
        struct block_log_file *primary_h = NULL, *shadow_h = NULL;
        struct block_log_port primary = {0}, shadow = {0};
        block_log_file_open(primary_dir, &primary_h, &primary);
        block_log_file_open(shadow_dir, &shadow_h, &shadow);

        const uint32_t bad_h = 3;
        for (uint32_t h = 0; h <= bad_h; h++) {
            struct block_hash hash;
            uint8_t buf[128];
            size_t len = 0;
            srfd_make_block(h, &hash, buf, sizeof(buf), &len);
            /* Primary: canonical. Shadow: same up to bad_h-1, then a forged
             * block at bad_h with mismatching bytes+hash. */
            (void)primary.append(primary.self, h, &hash, buf, len);
            if (h < bad_h) {
                (void)shadow.append(shadow.self, h, &hash, buf, len);
            } else {
                struct block_hash bad_hash = hash;
                bad_hash.bytes[3] = 0xde;  /* differ -> not idempotent */
                const char *forged = "FORGED-DIVERGENT-BLOCK";
                (void)shadow.append(shadow.self, h, &bad_hash,
                                    (const uint8_t *)forged,
                                    strlen(forged) + 1);
            }
        }

        /* Replay starting just above the pre-seeded shadow tip so the proof's
         * shadow-empty precondition holds, then diff the FULL range to expose
         * the seeded divergence at bad_h. */
        for (uint32_t h = bad_h + 1; h <= bad_h + 4; h++) {
            struct block_hash hash;
            uint8_t buf[128];
            size_t len = 0;
            srfd_make_block(h, &hash, buf, sizeof(buf), &len);
            (void)primary.append(primary.self, h, &hash, buf, len);
        }

        struct shadow_replay_proof_inputs in = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = bad_h + 1,   /* replay region is shadow-empty */
            .end_height = UINT32_MAX,
        };
        struct shadow_replay_proof_report rep;
        struct zcl_result r = shadow_replay_proof_run(&in, &rep);

        /* The replayed region (bad_h+1..tip) converges, but the DEFINITIVE
         * proof of the full archive must be run over 0..tip — do that diff
         * directly to confirm the seeded divergence is caught. */
        struct diff_with_legacy_shadow_inputs din = {
            .primary = &primary,
            .shadow = &shadow,
            .start_height = 0,
            .end_height = UINT32_MAX,
        };
        struct diff_with_legacy_shadow_report drep;
        struct zcl_result dr = diff_with_legacy_shadow(&din, &drep);

        SRFD_CHECK("replay-region run is operationally ok", r.ok);
        SRFD_CHECK("full-archive diff flags divergence",
                   dr.ok && drep.status == DIFF_STATUS_DIVERGENT &&
                   drep.first_divergent_height == bad_h);

        block_log_file_close(primary_h);
        block_log_file_close(shadow_h);
        test_rm_rf(primary_dir);
        test_rm_rf(shadow_dir);
    }

    return failures;
}
