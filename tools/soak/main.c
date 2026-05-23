/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * 7-day soak runner (MVP criterion #6).
 *
 * Separate binary that polls a running zclassic23 every 60 s for
 * a configured duration (default 7 days), feeds each sample
 * through the soak_harness analyzer, writes a timestamped log,
 * and exits non-zero if the run tripped any verdict rule (crash,
 * tip stall, RSS walk, too-short, no-samples).
 *
 * Intentionally minimal — no JSON parser, no libevent, no
 * threads. Every signal the verdict cares about comes from
 * either /proc (/proc/<pid>/status → VmRSS) or one-shot fork/exec
 * of `./zcl-rpc getblockcount` (integer result is trivial to
 * extract by scanning for "result"). The runner doesn't try to
 * recover from a dead node — if the node goes down, the runner
 * keeps polling, records the crash sample, and lets the verdict
 * logic flip to FAIL_CRASH.
 *
 * Usage:
 *     make soak-7day                   (7 days, against installed zclassic23)
 *     tools/soak/soak_runner --help
 *
 * Flags:
 *     --duration-sec=N      total run length (default 7d = 604800)
 *     --interval-sec=N      poll interval (default 60)
 *     --service=NAME        process name to pidof (default zclassic23)
 *     --rpc=PATH            zcl-rpc binary (default ./zcl-rpc)
 *     --log=PATH            output log (default ./soak-YYYYMMDD-HHMM.log)
 *     --stall-sec=N         tip stall threshold (default 1800)
 *     --rss-growth-mib=N    RSS walk threshold (default 512)
 *     --warmup-sec=N        RSS baseline warmup (default 1800)
 */

#define _POSIX_C_SOURCE 200809L

#include "platform/time_compat.h"
#include "test/soak_harness.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Pull the first non-whitespace integer value that follows the
 * literal "result" key in a JSON-RPC response body. Accepts the
 * minimal shape the node emits ({"result":3081601,"error":null,
 * "id":1}) without dragging a full JSON parser into the runner. */
static bool scan_result_int(const char *buf, int64_t *out)
{
    const char *p = strstr(buf, "\"result\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == 'n') return false; /* "result": null */
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = (int64_t)v;
    return true;
}

static pid_t pidof_service(const char *service)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pidof -s %s 2>/dev/null", service);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    char line[64] = {0};
    if (!fgets(line, sizeof(line), f)) { pclose(f); return 0; }
    pclose(f);
    long pid = strtol(line, NULL, 10);
    if (pid <= 0) return 0;
    return (pid_t)pid;
}

static uint64_t rss_bytes_for(pid_t pid)
{
    if (pid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    uint64_t rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            /* "VmRSS:\t   123456 kB" — kB to bytes. */
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            rss = (uint64_t)strtoull(p, NULL, 10) * 1024ULL;
            break;
        }
    }
    fclose(f);
    return rss;
}

static bool height_via_rpc(const char *rpc_bin, int64_t *out)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s getblockcount 2>/dev/null", rpc_bin);
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    return scan_result_int(buf, out);
}

static void default_log_path(char *out, size_t n)
{
    time_t t = platform_time_wall_time_t();
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, n, "soak-%04d%02d%02d-%02d%02d.log",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --duration-sec=N    total run length (default 604800 = 7d)\n"
        "  --interval-sec=N    poll interval (default 60)\n"
        "  --service=NAME      pidof target (default zclassic23)\n"
        "  --rpc=PATH          zcl-rpc binary (default ./zcl-rpc)\n"
        "  --log=PATH          output log (default soak-YYYYMMDD-HHMM.log)\n"
        "  --stall-sec=N       tip-stall threshold (default 1800)\n"
        "  --rss-growth-mib=N  RSS-walk threshold MiB (default 512)\n"
        "  --warmup-sec=N      RSS baseline warmup (default 1800)\n"
        "Exit status: 0 = SOAK_OK, else verdict ordinal.\n",
        argv0);
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s) return false;
    *out = (uint64_t)v;
    return true;
}

int main(int argc, char **argv)
{
    soak_thresholds_t cfg;
    soak_thresholds_default_7d(&cfg);

    uint64_t interval_sec = 60;
    const char *service   = "zclassic23";
    const char *rpc_bin   = "./zcl-rpc";
    char log_path[256] = {0};
    default_log_path(log_path, sizeof(log_path));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]); return 0;
        }
        if (strncmp(a, "--duration-sec=", 15) == 0) {
            parse_u64(a + 15, &cfg.min_duration_sec); continue;
        }
        if (strncmp(a, "--interval-sec=", 15) == 0) {
            parse_u64(a + 15, &interval_sec); continue;
        }
        if (strncmp(a, "--service=", 10) == 0) { service = a + 10; continue; }
        if (strncmp(a, "--rpc=",      6) == 0) { rpc_bin = a + 6;  continue; }
        if (strncmp(a, "--log=",      6) == 0) {
            snprintf(log_path, sizeof(log_path), "%s", a + 6); continue;
        }
        if (strncmp(a, "--stall-sec=", 12) == 0) {
            parse_u64(a + 12, &cfg.max_tip_stall_sec); continue;
        }
        if (strncmp(a, "--rss-growth-mib=", 17) == 0) {
            uint64_t mib = 0;
            if (parse_u64(a + 17, &mib))
                cfg.max_rss_growth_bytes = mib * 1024ULL * 1024ULL;
            continue;
        }
        if (strncmp(a, "--warmup-sec=", 13) == 0) {
            parse_u64(a + 13, &cfg.rss_walk_warmup_sec); continue;
        }
        fprintf(stderr, "unknown flag: %s\n", a);
        usage(argv[0]);
        return 2;
    }

    if (interval_sec == 0 || interval_sec > cfg.min_duration_sec) {
        fprintf(stderr, "interval-sec (%" PRIu64 ") out of range\n", interval_sec);
        return 2;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    FILE *log = fopen(log_path, "w");
    if (!log) {
        fprintf(stderr, "cannot open log %s: %s\n", log_path, strerror(errno));
        return 2;
    }
    setvbuf(log, NULL, _IOLBF, 0);

    soak_state_t st;
    soak_state_init(&st, &cfg);

    time_t started = platform_time_wall_time_t();
    fprintf(log,
        "# soak runner\n"
        "# duration_sec=%" PRIu64 " interval_sec=%" PRIu64 "\n"
        "# stall_sec=%" PRIu64 " warmup_sec=%" PRIu64 " rss_growth_bytes=%" PRIu64 "\n"
        "# service=%s rpc=%s\n"
        "# started=%ld\n"
        "# ts\talive\theight\trss_bytes\n",
        cfg.min_duration_sec, interval_sec,
        cfg.max_tip_stall_sec, cfg.rss_walk_warmup_sec,
        cfg.max_rss_growth_bytes,
        service, rpc_bin, (long)started);

    fprintf(stderr,
        "soak: logging to %s; will run %" PRIu64 "s (SIGINT/TERM to stop early)\n",
        log_path, cfg.min_duration_sec);

    time_t deadline = started + (time_t)cfg.min_duration_sec;
    while (!g_stop) {
        time_t now = platform_time_wall_time_t();
        if (now >= deadline) break;

        pid_t pid = pidof_service(service);
        bool alive = pid > 0;
        uint64_t rss = alive ? rss_bytes_for(pid) : 0;
        int64_t h = 0;
        if (alive) {
            if (!height_via_rpc(rpc_bin, &h)) {
                /* RPC failed but process exists → treat as crash: a
                 * node that can't answer getblockcount is, from the
                 * user's perspective, not up. */
                alive = false;
            }
        }
        soak_record_sample(&st, (uint64_t)now, alive, h, rss);
        fprintf(log, "%ld\t%d\t%" PRId64 "\t%" PRIu64 "\n",
                (long)now, alive ? 1 : 0, h, rss);

        /* Wake fractions of interval_sec to stay responsive to SIGTERM;
         * using `sleep()` rounds up and can sit in the syscall for the
         * full 60 s even after the flag is set. */
        for (uint64_t slept = 0; slept < interval_sec && !g_stop; slept++)
            sleep(1);
    }

    soak_verdict_t v = soak_compute_verdict(&st);
    time_t ended = platform_time_wall_time_t();
    fprintf(log,
        "# ended=%ld verdict=%s samples=%zu crashes=%" PRIu32 "\n"
        "# tip_hwm=%" PRId64 " rss_max=%" PRIu64 " rss_baseline=%" PRIu64 "\n",
        (long)ended, soak_verdict_str(v),
        st.n_samples, st.crash_count,
        st.tip_hwm, st.rss_max_seen, st.rss_baseline);
    fclose(log);

    fprintf(stderr,
        "soak: verdict=%s samples=%zu crashes=%" PRIu32 " tip_hwm=%" PRId64
        " rss_max=%" PRIu64 "\n",
        soak_verdict_str(v), st.n_samples, st.crash_count,
        st.tip_hwm, st.rss_max_seen);

    return (int)v;
}
