/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P11.6 RED stub — see test/soak_harness.h.
 *
 * This commit extracts the interface and a placeholder verdict
 * routine so the RED test in test_soak_harness.c has something
 * to link against. The stub unconditionally returns SOAK_OK,
 * which is wrong for every failure mode the harness claims to
 * detect — the RED test exercises all five and will fail until
 * the GREEN commit fills in the real logic.
 */

#include "test/soak_harness.h"

#include <string.h>

void soak_thresholds_default_7d(soak_thresholds_t *out)
{
    out->min_duration_sec     = 7ULL * 24 * 3600;    /* 7 days */
    out->max_tip_stall_sec    = 30ULL * 60;          /* 30 minutes */
    out->rss_walk_warmup_sec  = 30ULL * 60;          /* 30 minutes */
    out->max_rss_growth_bytes = 512ULL * 1024 * 1024;/* 512 MiB */
}

void soak_state_init(soak_state_t *s, const soak_thresholds_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
}

void soak_record_sample(soak_state_t *s,
                        uint64_t unix_ts,
                        bool alive,
                        int64_t height,
                        uint64_t rss_bytes)
{
    (void)s;
    (void)unix_ts;
    (void)alive;
    (void)height;
    (void)rss_bytes;
    /* RED stub: accumulate nothing. */
}

soak_verdict_t soak_compute_verdict(const soak_state_t *s)
{
    (void)s;
    /* RED stub: always claim the soak passed. The test exercises
     * every failure mode, so every assertion except the healthy
     * path will fail here. */
    return SOAK_OK;
}

const char *soak_verdict_str(soak_verdict_t v)
{
    switch (v) {
    case SOAK_OK:               return "OK";
    case SOAK_FAIL_NO_SAMPLES:  return "FAIL_NO_SAMPLES";
    case SOAK_FAIL_CRASH:       return "FAIL_CRASH";
    case SOAK_FAIL_TOO_SHORT:   return "FAIL_TOO_SHORT";
    case SOAK_FAIL_TIP_STALL:   return "FAIL_TIP_STALL";
    case SOAK_FAIL_RSS_WALK:    return "FAIL_RSS_WALK";
    }
    return "?";
}
