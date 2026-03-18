/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Minimal HTML template engine: {{var}} with escaping, {{{var}}} raw. */

#include "util/template.h"
#include <string.h>

/* Write HTML-escaped value to dst. Returns bytes written. */
static size_t tmpl_html_escape(char *dst, size_t max, const char *src)
{
    size_t w = 0;
    for (size_t i = 0; src[i]; i++) {
        const char *esc = NULL;
        size_t elen = 0;
        switch (src[i]) {
        case '<':  esc = "&lt;";   elen = 4; break;
        case '>':  esc = "&gt;";   elen = 4; break;
        case '&':  esc = "&amp;";  elen = 5; break;
        case '"':  esc = "&quot;"; elen = 6; break;
        case '\'': esc = "&#39;";  elen = 5; break;
        default: break;
        }
        if (esc) {
            if (w + elen >= max) return w;
            memcpy(dst + w, esc, elen);
            w += elen;
        } else {
            if (w + 1 >= max) return w;
            dst[w++] = src[i];
        }
    }
    return w;
}

/* Find variable by key. Returns value or NULL. */
static const char *tmpl_lookup(const struct template_var *vars, size_t n,
                               const char *key, size_t key_len)
{
    for (size_t i = 0; i < n; i++) {
        if (vars[i].key && strlen(vars[i].key) == key_len &&
            memcmp(vars[i].key, key, key_len) == 0)
            return vars[i].value ? vars[i].value : "";
    }
    return NULL;
}

size_t template_render(const char *tmpl,
                       const struct template_var *vars, size_t num_vars,
                       char *out, size_t out_max)
{
    if (!out || out_max == 0) return 0;
    if (!tmpl) { out[0] = '\0'; return 0; }
    if (!vars) num_vars = 0;

    size_t w = 0;
    const char *p = tmpl;

    while (*p && w + 1 < out_max) {
        /* Check for triple-brace {{{key}}} (raw output) */
        if (p[0] == '{' && p[1] == '{' && p[2] == '{') {
            const char *key_start = p + 3;
            const char *end = strstr(key_start, "}}}");
            if (end) {
                size_t key_len = (size_t)(end - key_start);
                const char *val = tmpl_lookup(vars, num_vars,
                                              key_start, key_len);
                if (val) {
                    size_t vlen = strlen(val);
                    size_t avail = out_max - w - 1;
                    size_t copy = vlen < avail ? vlen : avail;
                    memcpy(out + w, val, copy);
                    w += copy;
                } else {
                    /* Missing key: copy placeholder verbatim */
                    size_t span = (size_t)(end + 3 - p);
                    size_t avail = out_max - w - 1;
                    size_t copy = span < avail ? span : avail;
                    memcpy(out + w, p, copy);
                    w += copy;
                }
                p = end + 3;
                continue;
            }
        }

        /* Check for double-brace {{key}} (escaped output).
         * Triple-brace is checked first, so p[2] != '{' here. */
        if (p[0] == '{' && p[1] == '{') {
            const char *key_start = p + 2;
            const char *end = strstr(key_start, "}}");
            if (end) {
                size_t key_len = (size_t)(end - key_start);
                const char *val = tmpl_lookup(vars, num_vars,
                                              key_start, key_len);
                if (val) {
                    size_t added = tmpl_html_escape(out + w,
                                                    out_max - w, val);
                    w += added;
                } else {
                    size_t span = (size_t)(end + 2 - p);
                    size_t avail = out_max - w - 1;
                    size_t copy = span < avail ? span : avail;
                    memcpy(out + w, p, copy);
                    w += copy;
                }
                p = end + 2;
                continue;
            }
        }

        out[w++] = *p++;
    }

    out[w] = '\0';
    return w;
}
