/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reject stderr diagnostics in changed app/lib C files unless the nearby
 * code also emits an observable event, propagates a terminal failure, or
 * carries an explicit obs-ok justification.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 4096
#define LINE_LEN 4096

static bool has_c_suffix(const char *path)
{
    size_t len = strlen(path);
    return len >= 2 && strcmp(path + len - 2, ".c") == 0;
}

static bool is_test_path(const char *path)
{
    return strncmp(path, "lib/test/", 9) == 0 ||
           strstr(path, "/lib/test/") != NULL;
}

static bool line_has_obs_ok(const char *line)
{
    const char *tag = strstr(line, "// obs-ok:");
    return tag && tag[10] != '\0' && tag[10] != '\n' && tag[10] != ' ';
}

static bool line_has_event_emit(const char *line)
{
    return strstr(line, "event_emit(") || strstr(line, "event_emitf(");
}

static bool line_has_terminal_propagation(const char *line)
{
    return strstr(line, "return false;") ||
           strstr(line, "return -1;") ||
           strstr(line, "return 1;") ||
           strstr(line, "return NULL;") ||
           strstr(line, "exit(") ||
           strstr(line, "abort(");
}

static bool observability_line_allowed(char lines[][LINE_LEN], size_t count,
                                       size_t idx)
{
    if (line_has_obs_ok(lines[idx])) return true;

    size_t start = idx > 3 ? idx - 3 : 0;
    size_t end = idx + 3 < count ? idx + 3 : count - 1;
    for (size_t i = start; i <= end; i++) {
        if (line_has_event_emit(lines[i])) return true;
        if (i >= idx && line_has_terminal_propagation(lines[i])) return true;
    }
    return false;
}

static int check_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "check_observability_pairing: cannot open %s\n", path);
        return -1;
    }

    static char lines[MAX_LINES][LINE_LEN];
    memset(lines, 0, sizeof(lines));
    size_t count = 0;
    while (count < MAX_LINES && fgets(lines[count], sizeof(lines[count]), fp))
        count++;

    if (ferror(fp)) {
        fclose(fp);
        fprintf(stderr, "check_observability_pairing: read failed: %s\n", path);
        return -1;
    }
    fclose(fp);

    int bad = 0;
    for (size_t i = 0; i < count; i++) {
        if (strstr(lines[i], "fprintf(stderr") &&
            !observability_line_allowed(lines, count, i)) {
            printf("%s:%zu:%s", path, i + 1, lines[i]);
            if (lines[i][0] == '\0' || lines[i][strlen(lines[i]) - 1] != '\n')
                printf("\n");
            bad = 1;
        }
    }
    return bad;
}

static int run_list_command(const char *cmd, int *checked)
{
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "check_observability_pairing: command failed: %s\n", cmd);
        return -1;
    }

    int rc = 0;
    char path[LINE_LEN];
    while (fgets(path, sizeof(path), pipe)) {
        path[strcspn(path, "\n")] = '\0';
        if (!has_c_suffix(path) || is_test_path(path))
            continue;
        (*checked)++;
        int file_rc = check_file(path);
        if (file_rc < 0) rc = -1;
        if (file_rc > 0 && rc == 0) rc = 1;
    }

    int close_rc = pclose(pipe);
    if (close_rc != 0 && rc == 0) rc = -1;
    return rc;
}

static int read_first_line(const char *cmd, char *out, size_t outsz)
{
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return -1;
    if (!fgets(out, outsz, pipe)) out[0] = '\0';
    out[strcspn(out, "\n")] = '\0';
    int close_rc = pclose(pipe);
    return close_rc == 0 && out[0] != '\0' ? 0 : -1;
}

static int run_default_scan(int *checked)
{
    const char *scan_all = getenv("ZCL_OBS_SCAN_ALL");
    if (scan_all && strcmp(scan_all, "1") == 0) {
        return run_list_command("find app lib -type f -name '*.c' "
                                "! -path 'lib/test/*' | sort",
                                checked);
    }

    char base[128];
    if (read_first_line("git merge-base HEAD origin/main 2>/dev/null",
                        base, sizeof(base)) == 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "git diff --name-only --diff-filter=ACMR %s -- app lib",
                 base);
        return run_list_command(cmd, checked);
    }

    return run_list_command("git diff --name-only --diff-filter=ACMR -- app lib",
                            checked);
}

int main(int argc, char **argv)
{
    int rc = 0;
    int checked = 0;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (!has_c_suffix(argv[i]))
                continue;
            checked++;
            int file_rc = check_file(argv[i]);
            if (file_rc < 0) rc = -1;
            if (file_rc > 0 && rc == 0) rc = 1;
        }
    } else {
        rc = run_default_scan(&checked);
    }

    if (rc > 0) {
        printf("\ncheck_observability_pairing: unpaired stderr diagnostics found\n");
        printf("Pair stderr with event_emit/event_emitf, terminal propagation, "
               "or // obs-ok:<reason>.\n");
        return 1;
    }
    if (rc < 0)
        return 2;

    if (checked == 0)
        printf("check_observability_pairing: clean -- no changed app/lib C files\n");
    else
        printf("check_observability_pairing: clean -- stderr diagnostics are "
               "observable or justified\n");
    return 0;
}
