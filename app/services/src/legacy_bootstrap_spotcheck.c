/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * SHA3 evidence checks for legacy bootstrap imports. */

#include "services/legacy_bootstrap_importer.h"
#include "util/log_macros.h"

#include "chain/sha3_windows.h"
#include "core/random.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "storage/blocks_index_legacy_reader.h"
#include "storage/blocks_mmap_reader.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void legacy_bootstrap_hex32(const uint8_t hash[32], char out[65])
{
    static const char hexdigits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hexdigits[hash[i] >> 4];
        out[i * 2 + 1] = hexdigits[hash[i] & 0x0f];
    }
    out[64] = '\0';
}

static void legacy_bootstrap_log_window_loc(
    const struct legacy_block_loc *map,
    size_t map_count,
    int h,
    const char *log_prefix)
{
    if (h < 0 || (size_t)h >= map_count)
        return;
    const struct legacy_block_loc *loc = &map[(size_t)h];
    if (loc->height < 0) {
        LOG_WARN("mirror", "[%s] selected map h=%d: missing", log_prefix, h);
        return;
    }

    bool prev_ok = true;
    if (h > 0 && (size_t)(h - 1) < map_count &&
        map[(size_t)(h - 1)].height >= 0)
        prev_ok = uint256_eq(&loc->hashPrev, &map[(size_t)(h - 1)].hash);

    char hash_hex[65], prev_hex[65];
    legacy_bootstrap_hex32(loc->hash.data, hash_hex);
    legacy_bootstrap_hex32(loc->hashPrev.data, prev_hex);
    LOG_INFO("mirror", "[%s] selected map h=%d hash=%.16s prev=%.16s " "file=%d pos=%u status=0x%x prev_ok=%d", log_prefix, h, hash_hex, prev_hex, loc->nFile, loc->nDataPos, loc->nStatus, prev_ok ? 1 : 0);
}

static void legacy_bootstrap_log_window_map_diagnostics(
    const struct legacy_block_loc *map,
    size_t map_count,
    int start,
    int end,
    const char *log_prefix)
{
    LOG_INFO("mirror", "[%s] selected map diagnostic for h=%d..%d", log_prefix, start, end);

    for (int h = start; h <= end && h < start + 3; h++)
        legacy_bootstrap_log_window_loc(map, map_count, h, log_prefix);
    int tail_start = end - 2;
    if (tail_start < start + 3)
        tail_start = start + 3;
    for (int h = tail_start; h <= end; h++)
        legacy_bootstrap_log_window_loc(map, map_count, h, log_prefix);

    for (int h = start; h <= end; h++) {
        if (h <= 0 || (size_t)h >= map_count)
            continue;
        const struct legacy_block_loc *loc = &map[(size_t)h];
        const struct legacy_block_loc *prev = &map[(size_t)(h - 1)];
        if (loc->height < 0 || prev->height < 0 ||
            !uint256_eq(&loc->hashPrev, &prev->hash)) {
            LOG_INFO("mirror", "[%s] first selected-map break in window near h=%d", log_prefix, h);
            legacy_bootstrap_log_window_loc(map, map_count, h - 1,
                                            log_prefix);
            legacy_bootstrap_log_window_loc(map, map_count, h, log_prefix);
            return;
        }
    }
    LOG_INFO("mirror", "[%s] selected map has no parent break inside h=%d..%d", log_prefix, start, end);
}

static bool legacy_bootstrap_compute_window_hash(
    struct blocks_mmap *bmr,
    const struct legacy_block_loc *map,
    size_t map_count,
    size_t wi,
    const char *log_prefix,
    uint8_t out[32])
{
    if (wi >= g_sha3_windows_count) return false;
    int start = g_sha3_windows[wi].start_height;
    int end = start + SHA3_WINDOW_SIZE - 1;
    if (end < 0 || (size_t)end >= map_count) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    for (int h = start; h <= end; h++) {
        const struct legacy_block_loc *loc = &map[(size_t)h];
        if (loc->height < 0) {
            LOG_WARN("mirror", "[%s] spotcheck w=%zu h=%d MISSING in legacy index", log_prefix, wi, h);
            return false;
        }
        size_t len = 0;
        const uint8_t *bytes =
            bmr_get_payload(bmr, loc->nFile, loc->nDataPos, &len);
        if (!bytes || len == 0) {
            LOG_WARN("mirror", "[%s] spotcheck w=%zu h=%d mmap fetch failed", log_prefix, wi, h);
            return false;
        }
        sha3_256_write(&ctx, bytes, len);
    }

    sha3_256_finalize(&ctx, out);
    return true;
}

static bool legacy_bootstrap_verify_window_logged(
    struct blocks_mmap *bmr,
    const struct legacy_block_loc *map,
    size_t map_count,
    size_t wi,
    const char *log_prefix,
    bool dump_map_on_failure)
{
    uint8_t actual[32];
    int start = wi < g_sha3_windows_count ?
        g_sha3_windows[wi].start_height : -1;
    int end = start >= 0 ? start + SHA3_WINDOW_SIZE - 1 : -1;

    LOG_INFO("mirror", "[%s] spotcheck: verifying w=%zu (h=%d..%d)", log_prefix, wi, start, end);
    if (!legacy_bootstrap_compute_window_hash(bmr, map, map_count, wi,
                                              log_prefix, actual)) {
        LOG_WARN("mirror", "[%s] spotcheck FAILED at window %zu (h=%d..%d): " "unable to compute source digest", log_prefix, wi, start, end);
        if (dump_map_on_failure)
            legacy_bootstrap_log_window_map_diagnostics(
                map, map_count, start, end, log_prefix);
        return false;
    }
    if (memcmp(actual, g_sha3_windows[wi].hash, 32) != 0) {
        char expected_hex[65], actual_hex[65];
        legacy_bootstrap_hex32(g_sha3_windows[wi].hash, expected_hex);
        legacy_bootstrap_hex32(actual, actual_hex);
        LOG_WARN("mirror", "[%s] spotcheck FAILED at window %zu (h=%d..%d): " "expected=%s actual=%s", log_prefix, wi, start, end, expected_hex, actual_hex);
        if (dump_map_on_failure)
            legacy_bootstrap_log_window_map_diagnostics(
                map, map_count, start, end, log_prefix);
        return false;
    }
    LOG_INFO("mirror", "[%s] spotcheck: w=%zu OK", log_prefix, wi);
    return true;
}

bool legacy_bootstrap_spotcheck_sha3_windows(
    struct blocks_mmap *bmr,
    const struct legacy_block_loc *map,
    size_t map_count,
    int legacy_tip,
    int k,
    const char *log_prefix,
    const char *debug_env,
    bool dump_map_on_failure)
{
    if (!bmr || !map || !log_prefix) {
        LOG_WARN("legacy_bootstrap", "[legacy_bootstrap] SHA3 spotcheck: bad args");
        return false;
    }
    if (g_sha3_windows_count == 0) {
        LOG_WARN("mirror", "[%s] spotcheck SKIPPED: no compile-time anchor table " "(g_sha3_windows_count=0)", log_prefix);
        return false;
    }

    size_t max_w = g_sha3_windows_count;
    if (legacy_tip > 0) {
        size_t covered = (size_t)(legacy_tip + 1) / SHA3_WINDOW_SIZE;
        if (covered < max_w) max_w = covered;
    }
    if (max_w == 0) {
        LOG_WARN("mirror", "[%s] spotcheck SKIPPED: legacy tip h=%d is below any " "complete anchor window", log_prefix, legacy_tip);
        return false;
    }
    if ((size_t)k > max_w) k = (int)max_w;

    size_t picked[16];
    if (k > (int)(sizeof(picked) / sizeof(picked[0])))
        k = (int)(sizeof(picked) / sizeof(picked[0]));

    if (debug_env && debug_env[0]) {
        const char *debug_window = getenv(debug_env);
        if (debug_window && debug_window[0]) {
            char *endp = NULL;
            errno = 0;
            unsigned long long forced = strtoull(debug_window, &endp, 10);
            if (errno || !endp || *endp != '\0' || forced >= max_w) {
                LOG_WARN("mirror", "[%s] invalid %s=%s (valid range: 0..%zu)", log_prefix, debug_env, debug_window, max_w - 1);
                return false;
            }
            LOG_WARN("mirror", "[%s] debug spotcheck window %llu requested", log_prefix, forced);
            if (!legacy_bootstrap_verify_window_logged(
                    bmr, map, map_count, (size_t)forced, log_prefix,
                    dump_map_on_failure))
                return false;
        }
    }

    unsigned char rand_buf[16 * 4];
    GetRandBytes(rand_buf, sizeof(rand_buf));
    for (int i = 0; i < k; i++) {
        uint32_t r = (uint32_t)rand_buf[i * 4]
                   | ((uint32_t)rand_buf[i * 4 + 1] << 8)
                   | ((uint32_t)rand_buf[i * 4 + 2] << 16)
                   | ((uint32_t)rand_buf[i * 4 + 3] << 24);
        picked[i] = (size_t)(r % max_w);
    }

    LOG_INFO("mirror", "[%s] SHA3 spotcheck: K=%d windows over [0..%zu) " "(legacy_tip=%d)", log_prefix, k, max_w, legacy_tip);

    for (int i = 0; i < k; i++) {
        if (!legacy_bootstrap_verify_window_logged(
                bmr, map, map_count, picked[i], log_prefix,
                dump_map_on_failure))
            return false;
    }
    LOG_INFO("mirror", "[%s] SHA3 spotcheck: %d/%d windows match", log_prefix, k, k);
    return true;
}
