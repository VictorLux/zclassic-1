/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "sim/postmortem.h"

#include "platform/clock.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define POSTMORTEM_LOG_TAIL_MAX (64u * 1024u)

static int mkdir_if_needed(const char *path)
{
    if (!path || !*path) return -EINVAL;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        return -ENOTDIR;
    }
    return -errno;
}

static int write_bytes_file(const char *path, const void *buf, size_t len)
{
    if (!path || (!buf && len > 0)) return -EINVAL;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -errno;
    size_t wrote = len ? fwrite(buf, 1, len, fp) : 0;
    int close_rc = fclose(fp);
    if (wrote != len) return -EIO;
    if (close_rc != 0) return -errno;
    return 0;
}

static int copy_proc_status(const char *dst_path)
{
    FILE *in = fopen("/proc/self/status", "rb");
    if (!in) return write_bytes_file(dst_path, "", 0);
    uint8_t buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), in);
    fclose(in);
    return write_bytes_file(dst_path, buf, n);
}

static int copy_log_tail(const char *src_path, const char *dst_path)
{
    if (!src_path || !*src_path)
        return write_bytes_file(dst_path, "", 0);

    FILE *in = fopen(src_path, "rb");
    if (!in) return write_bytes_file(dst_path, "", 0);
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return write_bytes_file(dst_path, "", 0);
    }
    long end = ftell(in);
    if (end < 0) {
        fclose(in);
        return write_bytes_file(dst_path, "", 0);
    }
    long start = end > (long)POSTMORTEM_LOG_TAIL_MAX
        ? end - (long)POSTMORTEM_LOG_TAIL_MAX
        : 0;
    if (fseek(in, start, SEEK_SET) != 0) {
        fclose(in);
        return write_bytes_file(dst_path, "", 0);
    }
    size_t want = (size_t)(end - start);
    uint8_t *buf = NULL;
    if (want > 0) {
        buf = (uint8_t *)zcl_malloc(want, "postmortem.log_tail");
        if (!buf) {
            fclose(in);
            return -ENOMEM;
        }
    }
    size_t got = want ? fread(buf, 1, want, in) : 0;
    fclose(in);
    int rc = write_bytes_file(dst_path, buf, got);
    free(buf);
    return rc;
}

static bool has_suffix(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    return sl >= xl && strcmp(s + sl - xl, suffix) == 0;
}

static int64_t parse_capsule_time(const char *name)
{
    if (!name) return 0;
    char *end = NULL;
    long long v = strtoll(name, &end, 10);
    if (end == name) return 0;
    return (int64_t)v;
}

static void json_escape_string(const char *in, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    size_t w = 0;
    if (!in) in = "";
    for (size_t r = 0; in[r] && w + 1 < out_cap; r++) {
        unsigned char c = (unsigned char)in[r];
        if ((c == '"' || c == '\\') && w + 2 < out_cap) {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c == '\n' && w + 2 < out_cap) {
            out[w++] = '\\';
            out[w++] = 'n';
        } else if (c == '\r' && w + 2 < out_cap) {
            out[w++] = '\\';
            out[w++] = 'r';
        } else if (c == '\t' && w + 2 < out_cap) {
            out[w++] = '\\';
            out[w++] = 't';
        } else if (c >= 0x20) {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

int postmortem_capture_write(const struct postmortem_capture_opts *opts,
                             char *capsule_path_out,
                             size_t capsule_path_cap)
{
    if (!opts || !opts->dir || !*opts->dir || !opts->tape)
        return -EINVAL;

    int rc = mkdir_if_needed(opts->dir);
    if (rc != 0) return rc;

    int64_t ts = opts->crash_unix > 0
        ? opts->crash_unix
        : clock_now_wall_ms() / 1000;
    char name[128];
    snprintf(name, sizeof(name), "%lld-%d.cap",
             (long long)ts, (int)getpid());

    char cap_dir[512];
    int n = snprintf(cap_dir, sizeof(cap_dir), "%s/%s", opts->dir, name);
    if (n < 0 || (size_t)n >= sizeof(cap_dir)) return -ENAMETOOLONG;

    rc = mkdir_if_needed(cap_dir);
    if (rc != 0) return rc;

    char tape_path[576];
    snprintf(tape_path, sizeof(tape_path), "%s/tape.bin", cap_dir);
    rc = seed_tape_save(opts->tape, tape_path);
    if (rc != 0) return rc;

    char manifest_path[576];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
             cap_dir);
    char reason_json[256];
    json_escape_string(opts->reason, reason_json, sizeof(reason_json));
    char manifest[1024];
    int mn = snprintf(manifest, sizeof(manifest),
        "{\n"
        "  \"version\": 1,\n"
        "  \"format\": \"unpacked-cap-v1\",\n"
        "  \"crash_signal\": %d,\n"
        "  \"crash_unix\": %lld,\n"
        "  \"reason\": \"%s\",\n"
        "  \"tape_size_bytes\": %zu,\n"
        "  \"rng_count\": %llu,\n"
        "  \"clock_advance_count\": %llu,\n"
        "  \"inject_count\": %llu\n"
        "}\n",
        opts->crash_signal,
        (long long)ts,
        reason_json,
        seed_tape_size_bytes(opts->tape),
        (unsigned long long)seed_tape_rng_count(opts->tape),
        (unsigned long long)seed_tape_clock_advance_count(opts->tape),
        (unsigned long long)seed_tape_inject_count(opts->tape));
    if (mn < 0 || (size_t)mn >= sizeof(manifest)) return -EOVERFLOW;
    rc = write_bytes_file(manifest_path, manifest, (size_t)mn);
    if (rc != 0) return rc;

    char proc_path[576];
    snprintf(proc_path, sizeof(proc_path), "%s/procstatus.txt", cap_dir);
    rc = copy_proc_status(proc_path);
    if (rc != 0) return rc;

    char log_path[576];
    snprintf(log_path, sizeof(log_path), "%s/log.txt", cap_dir);
    rc = copy_log_tail(opts->log_path, log_path);
    if (rc != 0) return rc;

    char marker_path[576];
    snprintf(marker_path, sizeof(marker_path), "%s/coremarker.txt", cap_dir);
    char marker[256];
    int marker_n = snprintf(marker, sizeof(marker),
        "postmortem capsule captured at %lld; match with corefile near this timestamp\n",
        (long long)ts);
    if (marker_n < 0 || (size_t)marker_n >= sizeof(marker)) return -EOVERFLOW;
    rc = write_bytes_file(marker_path, marker, (size_t)marker_n);
    if (rc != 0) return rc;

    if (capsule_path_out && capsule_path_cap > 0) {
        int pn = snprintf(capsule_path_out, capsule_path_cap, "%s", cap_dir);
        if (pn < 0 || (size_t)pn >= capsule_path_cap) return -ENAMETOOLONG;
    }
    fprintf(stderr, "[postmortem] capsule written: %s\n", cap_dir);
    return 0;
}

seed_tape_t *postmortem_capsule_load_tape(const char *capsule_path)
{
    if (!capsule_path || !*capsule_path) return NULL;
    char tape_path[576];
    int n = snprintf(tape_path, sizeof(tape_path), "%s/tape.bin",
                     capsule_path);
    if (n < 0 || (size_t)n >= sizeof(tape_path)) return NULL;
    return seed_tape_load(tape_path);
}

bool postmortem_capsule_validate(const char *capsule_path)
{
    if (!capsule_path || !*capsule_path) return false;
    char manifest_path[576];
    int n = snprintf(manifest_path, sizeof(manifest_path),
                     "%s/manifest.json", capsule_path);
    if (n < 0 || (size_t)n >= sizeof(manifest_path)) return false;
    struct stat st;
    if (stat(manifest_path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    seed_tape_t *t = postmortem_capsule_load_tape(capsule_path);
    if (!t) return false;
    seed_tape_close(t);
    return true;
}

int postmortem_capsule_list(const char *dir,
                            struct postmortem_capsule_entry *entries,
                            size_t entry_cap,
                            size_t *count_out)
{
    if (!dir || !count_out || (entry_cap > 0 && !entries)) return -EINVAL;
    *count_out = 0;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -errno;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!has_suffix(de->d_name, ".cap")) continue;
        if (*count_out < entry_cap) {
            struct postmortem_capsule_entry *e = &entries[*count_out];
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            snprintf(e->path, sizeof(e->path), "%s/%s", dir, de->d_name);
            e->crash_unix = parse_capsule_time(de->d_name);
        }
        (*count_out)++;
    }
    closedir(d);
    return 0;
}
