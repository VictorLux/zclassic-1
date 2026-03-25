/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Build tool: converts .chtml template files into a C header.
 * Each foo.chtml becomes: static const char TMPL_FOO[] = "...";
 *
 * Usage: gen_templates <template_dir> <output.h>
 * Example: gen_templates app/views/templates app/views/include/views/wallet_templates_gen.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <ctype.h>

static void to_upper_underscore(const char *in, char *out, size_t max) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < max - 1; i++) {
        if (in[i] == '-' || in[i] == '.')
            out[j++] = '_';
        else
            out[j++] = (char)toupper((unsigned char)in[i]);
    }
    out[j] = '\0';
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <template_dir> <output.h>\n", argv[0]);
        return 1;
    }

    const char *dir = argv[1];
    const char *out_path = argv[2];

    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "Cannot open directory: %s\n", dir);
        return 1;
    }

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "Cannot write: %s\n", out_path);
        closedir(d);
        return 1;
    }

    fprintf(out,
        "/* Auto-generated from .chtml template files -- do not edit.\n"
        " * Regenerate: make templates */\n\n"
        "#ifndef ZCL_VIEWS_WALLET_TEMPLATES_GEN_H\n"
        "#define ZCL_VIEWS_WALLET_TEMPLATES_GEN_H\n\n");

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 7 || strcmp(ent->d_name + nlen - 6, ".chtml") != 0)
            continue;

        /* Validate filename: alphanumeric + hyphens only */
        bool valid_name = true;
        for (size_t i = 0; i < nlen - 6; i++) {
            char c = ent->d_name[i];
            if (!isalnum((unsigned char)c) && c != '-') {
                valid_name = false;
                break;
            }
        }
        if (!valid_name) {
            fprintf(stderr, "gen_templates: skipping invalid name: %s\n",
                ent->d_name);
            continue;
        }

        /* Build full path */
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        /* Read file (max 256KB per template) */
        FILE *f = fopen(path, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0 || fsize > 256 * 1024) {
            fprintf(stderr, "gen_templates: skipping %s (size %ld)\n",
                ent->d_name, fsize);
            fclose(f);
            continue;
        }

        char *buf = malloc((size_t)fsize + 1);
        if (!buf) { fclose(f); continue; }
        size_t nread = fread(buf, 1, (size_t)fsize, f);
        buf[nread] = '\0';
        fclose(f);

        /* Check for NUL bytes (binary file protection) */
        bool has_nul = false;
        for (size_t i = 0; i < nread; i++) {
            if (buf[i] == '\0') { has_nul = true; break; }
        }
        if (has_nul) {
            fprintf(stderr, "gen_templates: skipping binary file: %s\n",
                ent->d_name);
            free(buf);
            continue;
        }

        /* Convert filename: backup-warning.chtml → BACKUP_WARNING */
        char name_base[256];
        snprintf(name_base, sizeof(name_base), "%.*s",
            (int)(nlen - 6), ent->d_name);
        char name_upper[256];
        to_upper_underscore(name_base, name_upper, sizeof(name_upper));

        /* Write C string */
        fprintf(out, "static const char TMPL_%s[] =\n    \"", name_upper);
        for (size_t i = 0; i < nread; i++) {
            switch (buf[i]) {
            case '\\': fputs("\\\\", out); break;
            case '"':  fputs("\\\"", out); break;
            case '\n': fputs("\\n", out);  break;
            case '\r': break; /* skip CR */
            case '\t': fputs("\\t", out);  break;
            default:   fputc(buf[i], out); break;
            }
        }
        fprintf(out, "\";\n\n");

        free(buf);
        count++;
    }

    fprintf(out, "#endif\n");
    fclose(out);
    closedir(d);

    fprintf(stderr, "gen_templates: %d .chtml files -> %s\n", count, out_path);
    return 0;
}
