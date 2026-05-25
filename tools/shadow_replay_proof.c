/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "adapters/outbound/persistence/block_log_file.h"
#include "adapters/outbound/persistence/block_log_legacy.h"
#include "application/operations/shadow_replay_proof.h"

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
            "usage: %s <legacy_datadir> <shadow_dir> [start] [end]\n",
            argv0);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5) {
        usage(argv[0]);
        return 2;
    }

    uint32_t start = 0;
    uint32_t end = UINT32_MAX;
    if (argc >= 4 && !parse_u32_arg(argv[3], &start)) {
        usage(argv[0]);
        return 2;
    }
    if (argc >= 5 && !parse_u32_arg(argv[4], &end)) {
        usage(argv[0]);
        return 2;
    }

    struct block_log_legacy *legacy_h = NULL;
    struct block_log_port primary = {0};
    struct zcl_result r = block_log_legacy_open(argv[1], &legacy_h, &primary);
    if (!r.ok) {
        fprintf(stderr, "legacy open failed: code=%d %s\n", r.code, r.message);
        return 1;
    }

    struct block_log_file *shadow_h = NULL;
    struct block_log_port shadow = {0};
    r = block_log_file_open(argv[2], &shadow_h, &shadow);
    if (!r.ok) {
        fprintf(stderr, "shadow open failed: code=%d %s\n", r.code, r.message);
        block_log_legacy_close(legacy_h);
        return 1;
    }

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
           "\"first_divergent_height\":%" PRIu32
           "}\n",
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
           rep.diff.first_divergent_height);

    block_log_file_close(shadow_h);
    block_log_legacy_close(legacy_h);
    return rep.proof_ok ? 0 : 1;
}
