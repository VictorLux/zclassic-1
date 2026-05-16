/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Checked allocation wrappers. Every malloc/calloc/realloc in
 * application code should use these instead of raw libc calls.
 *
 * Why: raw malloc returns NULL silently. These log the failure with
 * context (size, label, file, line) to stderr (and thus node.log via
 * the redirect) so OOM is observable. An agent writing `malloc(n)`
 * instead of `zcl_malloc(n, "label")` will be caught by `make lint`.
 */

#ifndef ZCL_SAFE_ALLOC_H
#define ZCL_SAFE_ALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Checked allocators ─────────────────────────────────────────── */

/* Returns NULL on failure, but logs + emits event first.
 * Use when graceful degradation is possible. */
static inline void *zcl_malloc_impl(size_t size, const char *label,
                                     const char *file, int line)
{
    void *p = malloc(size);
    if (!p && size > 0) {
        fprintf(stderr, "zcl_malloc FAILED: %zu bytes for '%s' at %s:%d\n",
                size, label, file, line);
    }
    return p;
}

static inline void *zcl_calloc_impl(size_t count, size_t size,
                                     const char *label,
                                     const char *file, int line)
{
    void *p = calloc(count, size);
    if (!p && count > 0 && size > 0) {
        fprintf(stderr, "zcl_calloc FAILED: %zu x %zu bytes for '%s' at %s:%d\n",
                count, size, label, file, line);
    }
    return p;
}

/* Checked realloc — never leaks the original pointer on failure.
 * Returns NULL on failure; original ptr is NOT freed (caller decides). */
static inline void *zcl_realloc_impl(void *ptr, size_t size,
                                      const char *label,
                                      const char *file, int line)
{
    void *p = realloc(ptr, size);
    if (!p && size > 0) {
        fprintf(stderr, "zcl_realloc FAILED: %zu bytes for '%s' at %s:%d\n",
                size, label, file, line);
    }
    return p;
}

/* Convenience macros that inject __FILE__ and __LINE__ automatically. */
#define zcl_malloc(size, label)        zcl_malloc_impl((size), (label), __FILE__, __LINE__)
#define zcl_calloc(count, size, label) zcl_calloc_impl((count), (size), (label), __FILE__, __LINE__)
#define zcl_realloc(ptr, size, label)  zcl_realloc_impl((ptr), (size), (label), __FILE__, __LINE__)

/* Abort variant — use when there is no reasonable fallback.
 * Prefer zcl_malloc + NULL check when graceful degradation is possible. */
static inline void *zcl_malloc_or_die_impl(size_t size, const char *label,
                                            const char *file, int line)
{
    void *p = malloc(size);
    if (!p && size > 0) {
        fprintf(stderr, "FATAL: zcl_malloc_or_die: %zu bytes for '%s' at %s:%d\n",
                size, label, file, line);
        abort();
    }
    return p;
}

#define zcl_malloc_or_die(size, label) \
    zcl_malloc_or_die_impl((size), (label), __FILE__, __LINE__)

#endif /* ZCL_SAFE_ALLOC_H */
