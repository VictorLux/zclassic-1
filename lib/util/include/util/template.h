/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Minimal HTML template engine: {{variable}} substitution with escaping. */

#ifndef ZCL_UTIL_TEMPLATE_H
#define ZCL_UTIL_TEMPLATE_H

#include <stddef.h>

struct template_var {
    const char *key;
    const char *value;
};

/* Render template with variable substitutions.
 * {{key}} — HTML-escaped substitution.
 * {{{key}}} — raw (unescaped) substitution.
 * Missing keys leave the placeholder unchanged.
 * Returns bytes written to out (excluding NUL), or 0 on error. */
size_t template_render(const char *tmpl,
                       const struct template_var *vars, size_t num_vars,
                       char *out, size_t out_max);

#endif /* ZCL_UTIL_TEMPLATE_H */
