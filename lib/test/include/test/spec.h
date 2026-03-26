/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * User story spec test framework — structured C tests that read like English.
 *
 * Usage:
 *   FEATURE("Wallet Dashboard") {
 *       STORY("user sees total balance") {
 *           GIVEN("dashboard page loads")
 *               wv_get("/wallet");
 *           THEN("balance is displayed")
 *               EXPECT(wv_has("ZCL"));
 *           THEN("privacy meter is visible")
 *               EXPECT(wv_has("% private"));
 *       }
 *   }
 *
 * Each FEATURE/STORY prints its name. EXPECT tracks pass/fail counts.
 * At the end, call spec_summary() to print totals and return exit code.
 *
 * Design goals:
 *   - Zero dependencies (just macros + static counters)
 *   - LLM-friendly: each story is self-contained, copy-pasteable
 *   - Organized by feature: one spec_*.c file per feature area
 *   - GIVEN/WHEN/THEN are documentation only (no runtime effect)
 *   - EXPECT is the only assertion — keeps it simple */

#ifndef ZCL_TEST_SPEC_H
#define ZCL_TEST_SPEC_H

#include <stdio.h>
#include <stdbool.h>

/* ── Counters (one set per translation unit) ─────────────── */

static int _spec_pass = 0;
static int _spec_fail = 0;
static const char *_spec_current_story = "";
static const char *_spec_current_feature = "";

/* ── Feature / Story ─────────────────────────────────────── */

#define FEATURE(name) \
    _spec_current_feature = (name); \
    printf("\n=== %s ===\n", (name)); \
    if (1)

#define STORY(name) \
    _spec_current_story = (name); \
    printf("  %s... ", (name)); \
    if (1)

/* ── Given / When / Then (documentation markers) ─────────── */

#define GIVEN(desc)  /* context setup */
#define WHEN(desc)   /* action */
#define THEN(desc)   /* assertion phase */

/* ── Assertions ──────────────────────────────────────────── */

/* Internal: track if current story already failed */
static bool _spec_story_failed = false;

#define EXPECT(cond) do { \
    if (cond) { _spec_pass++; } \
    else { \
        _spec_fail++; _spec_story_failed = true; \
        printf("FAIL\n    EXPECT failed: %s\n    at %s:%d\n", \
            #cond, __FILE__, __LINE__); \
    } \
} while (0)

#define EXPECT_EQ(a, b) do { \
    if ((a) == (b)) { _spec_pass++; } \
    else { \
        _spec_fail++; _spec_story_failed = true; \
        printf("FAIL\n    EXPECT_EQ failed: %s != %s\n    at %s:%d\n", \
            #a, #b, __FILE__, __LINE__); \
    } \
} while (0)

#define EXPECT_STR(haystack, needle) do { \
    if (strstr((haystack), (needle))) { _spec_pass++; } \
    else { \
        _spec_fail++; _spec_story_failed = true; \
        printf("FAIL\n    EXPECT_STR: \"%s\" not found\n    at %s:%d\n", \
            (needle), __FILE__, __LINE__); \
    } \
} while (0)

/* Mark story as passed (call at end of story block) */
#define PASS() if (!_spec_story_failed) printf("OK\n"); \
    _spec_story_failed = false;

/* ── Summary ─────────────────────────────────────────────── */

#define SPEC_SUMMARY() do { \
    printf("\n%d passed, %d failed\n", _spec_pass, _spec_fail); \
} while (0)

/* Return pass/fail counts to caller */
#define SPEC_FAILURES() (_spec_fail)

#endif /* ZCL_TEST_SPEC_H */
