/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * shadow_replay_proof — offline cutover PROVE harness (CLI).
 *
 * See docs/work/cutover.md §1 (PROVE). This is TEST/TOOLING only: it proves
 * the shadow stage pipeline equals legacy OFFLINE so the eventual flip is
 * safe. It NEVER touches the live node, any service, or any systemd unit.
 *
 * Two tiers, layered over the existing primitives (no reimplemented crypto):
 *
 *   Tier 1 (default, minutes): the full 0→tip linear replay driver.
 *     block_log_legacy_open + iter_from → shadow block_log_file.append →
 *     diff_with_legacy_shadow byte-range diff. A proof passes only when
 *     every fed block is diffed (blocks_fed == blocks_diffed, so a
 *     backpressure drop can't fake convergence) and the byte diff converges.
 *
 *   Tier 2 (--deep / --tier2, slow): ALSO runs the canonical consensus
 *     sweep (replay_verify_run → check_block: Equihash 200,9 + difficulty
 *     target + merkle root) over the replayed range, plus prev-block
 *     linkage. With --sample=N it instead verifies a FlyClient-style bounded
 *     sample window from the start of the range (deep verification when full
 *     is impractical). The crypto is the canonical helper, never reinvented.
 *
 * On a passing Tier-1 proof it emits the canonical one-line artifact:
 *   "shadow_replay_proof: 0 divergences across N blocks, commit <sha>"
 * (Tier 2 extends it with the consensus sweep result.)
 *
 * Exit status: 0 = proof passed, 1 = proof failed / diverged, 2 = usage.
 */

#include "adapters/outbound/persistence/block_log_file.h"
#include "adapters/outbound/persistence/block_log_legacy.h"
#include "application/operations/shadow_replay_proof.h"
#include "services/replay_verify_service.h"

#include "chain/chainparams.h"
#include "util/clientversion.h"

#include <inttypes.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t g_shutdown_requested = 0;

static const char *proof_status_name(enum shadow_replay_proof_status s)
{
    switch (s) {
    case SHADOW_REPLAY_PROOF_OK: return "ok";
    case SHADOW_REPLAY_PROOF_EMPTY: return "empty";
    case SHADOW_REPLAY_PROOF_SHADOW_NOT_EMPTY: return "shadow_not_empty";
    case SHADOW_REPLAY_PROOF_DIVERGED: return "diverged";
    }
    return "unknown";
}

static const char *diff_status_name(enum diff_with_legacy_shadow_status s)
{
    switch (s) {
    case DIFF_STATUS_CONVERGED: return "converged";
    case DIFF_STATUS_DIVERGENT: return "divergent";
    case DIFF_STATUS_SHADOW_MISSING: return "shadow_missing";
    case DIFF_STATUS_PRIMARY_MISSING: return "primary_missing";
    case DIFF_STATUS_EMPTY_RANGE: return "empty_range";
    }
    return "unknown";
}

static bool parse_u32_arg(const char *s, uint32_t *out)
{
    if (!s || !out) return false;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0' || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--deep|--tier2] [--sample=N] <legacy_datadir> "
        "<shadow_dir> [start] [end]\n"
        "\n"
        "  Tier 1 (default): full linear replay 0..tip (or start..end),\n"
        "    byte convergence + blocks_fed == blocks_diffed.\n"
        "  --deep / --tier2: ALSO run the canonical consensus sweep\n"
        "    (Equihash 200,9 + difficulty + merkle + linkage via check_block)\n"
        "    over the replayed range. Slow.\n"
        "  --sample=N: with --deep, verify only the first N blocks of the\n"
        "    range (FlyClient-style bounded deep sample) instead of all.\n"
        "\n"
        "  start/end are inclusive heights; end defaults to legacy tip.\n",
        argv0);
}

int main(int argc, char **argv)
{
    bool deep = false;
    uint64_t sample = 0;  /* 0 == full range in deep mode */

    /* Parse leading option flags. */
    int ai = 1;
    for (; ai < argc; ai++) {
        const char *a = argv[ai];
        if (strcmp(a, "--deep") == 0 || strcmp(a, "--tier2") == 0) {
            deep = true;
        } else if (strncmp(a, "--sample=", 9) == 0) {
            char *end = NULL;
            unsigned long long v = strtoull(a + 9, &end, 10);
            if (!end || *end != '\0' || v == 0) {
                usage(argv[0]);
                return 2;
            }
            sample = (uint64_t)v;
        } else if (a[0] == '-' && a[1] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            break;  /* first positional */
        }
    }

    int pos = argc - ai;  /* positional arg count */
    if (pos < 2 || pos > 4) {
        usage(argv[0]);
        return 2;
    }

    const char *legacy_datadir = argv[ai];
    const char *shadow_dir = argv[ai + 1];
    uint32_t start = 0;
    uint32_t end = UINT32_MAX;
    if (pos >= 3 && !parse_u32_arg(argv[ai + 2], &start)) {
        usage(argv[0]);
        return 2;
    }
    if (pos >= 4 && !parse_u32_arg(argv[ai + 3], &end)) {
        usage(argv[0]);
        return 2;
    }

    /* The consensus sweep (Tier 2) needs chain params selected; harmless for
     * Tier 1. This is offline tooling — never touches the live node. */
    chain_params_select(CHAIN_MAIN);

    struct block_log_legacy *legacy_h = NULL;
    struct block_log_port primary = {0};
    struct zcl_result r =
        block_log_legacy_open(legacy_datadir, &legacy_h, &primary);
    if (!r.ok) {
        fprintf(stderr, "legacy open failed: code=%d %s\n", r.code, r.message);
        return 1;
    }

    struct block_log_file *shadow_h = NULL;
    struct block_log_port shadow = {0};
    r = block_log_file_open(shadow_dir, &shadow_h, &shadow);
    if (!r.ok) {
        fprintf(stderr, "shadow open failed: code=%d %s\n", r.code, r.message);
        block_log_legacy_close(legacy_h);
        return 1;
    }

    /* ── Tier 1: full linear replay + byte convergence ──────────────────── */
    struct shadow_replay_proof_report rep;
    struct shadow_replay_proof_inputs in = {
        .primary = &primary,
        .shadow = &shadow,
        .start_height = start,
        .end_height = end,
    };
    r = shadow_replay_proof_run(&in, &rep);
    if (!r.ok) {
        fprintf(stderr, "proof failed: code=%d %s\n", r.code, r.message);
        block_log_file_close(shadow_h);
        block_log_legacy_close(legacy_h);
        return 1;
    }

    printf("{"
           "\"tier\":%d,"
           "\"proof_ok\":%s,"
           "\"status\":\"%s\","
           "\"diff_status\":\"%s\","
           "\"start_height\":%" PRIu32 ","
           "\"end_height\":%" PRIu32 ","
           "\"primary_tip_before\":%" PRIu32 ","
           "\"shadow_tip_before\":%" PRIu32 ","
           "\"shadow_tip_after\":%" PRIu32 ","
           "\"blocks_fed\":%" PRIu32 ","
           "\"blocks_diffed\":%" PRIu32 ","
           "\"first_divergent_height\":%" PRIu32 ","
           "\"commit\":\"%s\""
           "}\n",
           deep ? 2 : 1,
           rep.proof_ok ? "true" : "false",
           proof_status_name(rep.status),
           diff_status_name(rep.diff.status),
           rep.start_height,
           rep.end_height,
           rep.primary_tip_before,
           rep.shadow_tip_before,
           rep.shadow_tip_after,
           rep.blocks_fed,
           rep.blocks_diffed,
           rep.diff.first_divergent_height,
           zcl_build_commit());

    if (!rep.proof_ok) {
        fprintf(stderr,
                "shadow_replay_proof: DIVERGED (status=%s diff=%s "
                "first_divergent_height=%" PRIu32 ")\n",
                proof_status_name(rep.status),
                diff_status_name(rep.diff.status),
                rep.diff.first_divergent_height);
        block_log_file_close(shadow_h);
        block_log_legacy_close(legacy_h);
        return 1;
    }

    /* ── Tier 2 (opt-in): canonical consensus sweep over the range ──────── */
    if (deep) {
        /* max_blocks: 0 == to tip; --sample=N == first N of the range. */
        uint64_t max_blocks = sample;  /* 0 means full when no sample given */
        struct replay_verify_report vr;
        struct zcl_result vrr =
            replay_verify_run(legacy_datadir, rep.start_height,
                              max_blocks, &vr);
        if (!vrr.ok) {
            fprintf(stderr, "tier2 consensus sweep failed: code=%d %s\n",
                    vrr.code, vrr.message);
            block_log_file_close(shadow_h);
            block_log_legacy_close(legacy_h);
            return 1;
        }

        bool consensus_ok = vr.pow_failures == 0 &&
                            vr.linkage_failures == 0 &&
                            vr.merkle_failures == 0;

        printf("{"
               "\"tier2_sweep\":true,"
               "\"sample\":%llu,"
               "\"blocks_checked\":%llu,"
               "\"pow_failures\":%llu,"
               "\"linkage_failures\":%llu,"
               "\"merkle_failures\":%llu,"
               "\"first_fail_height\":%lld,"
               "\"first_fail_reason\":\"%s\","
               "\"consensus_ok\":%s"
               "}\n",
               (unsigned long long)sample,
               (unsigned long long)vr.blocks_checked,
               (unsigned long long)vr.pow_failures,
               (unsigned long long)vr.linkage_failures,
               (unsigned long long)vr.merkle_failures,
               (long long)vr.first_fail_height,
               vr.first_fail_reason ? vr.first_fail_reason : "none",
               consensus_ok ? "true" : "false");

        if (!consensus_ok) {
            fprintf(stderr,
                    "shadow_replay_proof: TIER2 CONSENSUS FAIL at "
                    "height %lld (%s)\n",
                    (long long)vr.first_fail_height,
                    vr.first_fail_reason ? vr.first_fail_reason : "?");
            block_log_file_close(shadow_h);
            block_log_legacy_close(legacy_h);
            return 1;
        }
    }

    /* Canonical PROVE artifact line (docs/work/cutover.md §1). */
    if (deep) {
        printf("shadow_replay_proof: 0 divergences across %" PRIu32
               " blocks (tier-2 consensus verified), commit %s\n",
               rep.blocks_diffed, zcl_build_commit());
    } else {
        printf("shadow_replay_proof: 0 divergences across %" PRIu32
               " blocks, commit %s\n",
               rep.blocks_diffed, zcl_build_commit());
    }

    block_log_file_close(shadow_h);
    block_log_legacy_close(legacy_h);
    return 0;
}
