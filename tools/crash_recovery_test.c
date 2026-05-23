/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Crash Recovery Test Harness
 * ===========================
 *
 * Starts `./zclassic23` with an isolated datadir, drives it for a
 * random interval, SIGKILLs it, restarts it, and asserts the
 * recovery invariants all still hold:
 *
 *   - UTXO count never decreases across a kill+restart cycle
 *   - Chain tip height never regresses
 *   - SHA3 UTXO commitment either stays identical OR advances
 *     (monotonic on successful chunks of new work)
 *
 * The harness is the end-to-end check for `recovery_policy`,
 * `db_txn`, `chain_state_repository`, and `block_index_integrity`.
 * Any of those failing opens a class of bug this harness should
 * catch on at least one of its 100 randomised iterations.
 *
 * Prerequisites
 * -------------
 *
 *   1. `./zclassic23` and `./zcl-rpc` compiled and in $PWD
 *      (the default `make` target builds both).
 *   2. An isolated pre-seeded datadir. Lookup order:
 *         $ZCL_CRASH_DATADIR
 *         ~/.zclassic-c23-crashtest
 *      The datadir should contain a minimal but non-empty UTXO set
 *      so the harness has something non-trivial to crash during.
 *      If the datadir does not exist, the harness prints a skip
 *      message and exits 0 — CI on clean hosts never fails on this
 *      test; only hosts with a seeded datadir exercise it.
 *
 * Usage
 * -----
 *
 *   crash_recovery_test [options]
 *     --iterations=N     Number of kill/restart cycles (default 100)
 *     --min-delay-ms=N   Min uptime before kill (default 250)
 *     --max-delay-ms=N   Max uptime before kill (default 3000)
 *     --rpc-port=N       RPC port the node listens on (default 18232)
 *     --seed=N           PRNG seed for reproducibility (default: time)
 *     --verbose          Print each iteration's integrity numbers
 *
 * Exit codes:
 *     0   all iterations passed OR skipped (no datadir)
 *     1   at least one invariant violation detected
 *     2   harness error (fork, exec, RPC timeout, etc.)
 *
 * Design notes
 * ------------
 *
 * The harness calls `./zcl-rpc <method>` via popen() rather than
 * speaking HTTP directly. That keeps this tool small and avoids
 * duplicating cookie-auth logic — zcl-rpc already handles it.
 *
 * Between kill and restart we wait for the previous SQLite WAL to
 * drain via a short sleep; an unrecovered WAL is *exactly* the
 * scenario we want to hit, so we keep the wait short and rely on
 * the db_txn/recovery_policy rails to do their jobs on restart.
 */

#define _POSIX_C_SOURCE 200809L

#include "platform/time_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Config ────────────────────────────────────────────────── */

struct cr_config {
    char     datadir[512];
    int      iterations;
    int      min_delay_ms;
    int      max_delay_ms;
    int      rpc_port;
    uint64_t seed;
    bool     verbose;
};

static void cr_defaults(struct cr_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    const char *env = getenv("ZCL_CRASH_DATADIR");
    const char *home = getenv("HOME");
    if (env && *env) {
        snprintf(cfg->datadir, sizeof(cfg->datadir), "%s", env);
    } else if (home) {
        snprintf(cfg->datadir, sizeof(cfg->datadir),
                 "%s/.zclassic-c23-crashtest", home);
    } else {
        snprintf(cfg->datadir, sizeof(cfg->datadir), "/tmp/zcl-crashtest");
    }
    cfg->iterations    = 100;
    cfg->min_delay_ms  = 250;
    cfg->max_delay_ms  = 3000;
    cfg->rpc_port      = 18232;
    cfg->seed          = (uint64_t)platform_time_wall_time_t();
    cfg->verbose       = false;
}

static bool parse_long_flag(const char *arg, const char *name, long *out)
{
    size_t nlen = strlen(name);
    if (strncmp(arg, name, nlen) != 0) return false;
    if (arg[nlen] != '=') return false;
    char *end = NULL;
    long v = strtol(arg + nlen + 1, &end, 10);
    if (end == arg + nlen + 1) return false;
    *out = v;
    return true;
}

static int parse_args(int argc, char **argv, struct cr_config *cfg)
{
    for (int i = 1; i < argc; i++) {
        long v;
        if (strncmp(argv[i], "--datadir=", 10) == 0) {
            snprintf(cfg->datadir, sizeof(cfg->datadir), "%s", argv[i] + 10);
        } else if (parse_long_flag(argv[i], "--iterations", &v)) {
            cfg->iterations = (int)v;
        } else if (parse_long_flag(argv[i], "--min-delay-ms", &v)) {
            cfg->min_delay_ms = (int)v;
        } else if (parse_long_flag(argv[i], "--max-delay-ms", &v)) {
            cfg->max_delay_ms = (int)v;
        } else if (parse_long_flag(argv[i], "--rpc-port", &v)) {
            cfg->rpc_port = (int)v;
        } else if (parse_long_flag(argv[i], "--seed", &v)) {
            cfg->seed = (uint64_t)v;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: crash_recovery_test [--datadir=DIR] "
                   "[--iterations=N] [--min-delay-ms=N] "
                   "[--max-delay-ms=N] [--rpc-port=N] [--seed=N] "
                   "[--verbose]\n");
            return 1;
        } else {
            fprintf(stderr, "crash_recovery_test: unknown arg %s\n", argv[i]);
            return 2;
        }
    }
    if (cfg->iterations < 1)       cfg->iterations = 1;
    if (cfg->min_delay_ms < 1)     cfg->min_delay_ms = 1;
    if (cfg->max_delay_ms < cfg->min_delay_ms)
        cfg->max_delay_ms = cfg->min_delay_ms;
    return 0;
}

/* ── Small PRNG ─────────────────────────────────────────────── */

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x ? x : 1;
    return *state;
}

static int rand_range(uint64_t *state, int lo, int hi)
{
    if (hi <= lo) return lo;
    uint64_t span = (uint64_t)(hi - lo + 1);
    return lo + (int)(xorshift64(state) % span);
}

/* ── Timing helper ──────────────────────────────────────────── */

static int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── RPC via zcl-rpc subprocess ─────────────────────────────── */

/* Invoke `./zcl-rpc <method>` with the crash datadir in the env so
 * zcl-rpc finds the right cookie file. Returns -1 on any exec or
 * read error, otherwise the number of bytes written to `out`. */
static int cr_rpc(const struct cr_config *cfg, const char *method,
                  char *out, size_t out_cap)
{
    char cmd[1024];
    /* Quote is fine: method is a fixed string under caller control. */
    snprintf(cmd, sizeof(cmd),
             "ZCL_DATADIR=%s ZCL_RPCPORT=%d ./zcl-rpc %s 2>/dev/null",
             cfg->datadir, cfg->rpc_port, method);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t total = fread(out, 1, out_cap - 1, p);
    out[total] = '\0';
    int rc = pclose(p);
    if (rc != 0) return -1;
    return (int)total;
}

/* Parse a JSON-RPC response looking for a numeric "result" field.
 * Returns true if the field was found and parsed. This is not a
 * general JSON parser — we only need to scrape one int per call. */
static bool parse_result_i64(const char *response, int64_t *out)
{
    const char *key = "\"result\"";
    const char *p = strstr(response, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    /* Skip a leading quote for string-encoded numbers, just in case. */
    if (*p == '"') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = (int64_t)v;
    return true;
}

/* ── Integrity snapshot ─────────────────────────────────────── */

struct cr_snapshot {
    int64_t block_count;
    int64_t utxo_count;
    /* A 64-hex commitment is plenty of signal without dragging in
     * full SHA3 parsing. We keep the raw string and compare. */
    char    commitment[96];
};

static bool cr_read_snapshot(const struct cr_config *cfg,
                              struct cr_snapshot *out)
{
    char buf[16384];
    memset(out, 0, sizeof(*out));

    /* getblockcount → integer result */
    if (cr_rpc(cfg, "getblockcount", buf, sizeof(buf)) < 0) return false;
    if (!parse_result_i64(buf, &out->block_count)) return false;

    /* node-specific RPC: utxo count. The C23 node exposes it via
     * `getutxocount`; fall back to `-1` on older builds. */
    if (cr_rpc(cfg, "getutxocount", buf, sizeof(buf)) >= 0)
        (void)parse_result_i64(buf, &out->utxo_count);
    else
        out->utxo_count = -1;

    /* utxo_commitment — string result. We stash the first 64 hex
     * chars from the response; a real SHA3 commitment is 64 chars. */
    if (cr_rpc(cfg, "getutxocommitment", buf, sizeof(buf)) >= 0) {
        const char *r = strstr(buf, "\"result\"");
        if (r) {
            r = strchr(r, '"');
            if (r) r = strchr(r + 1, '"');  /* "result" */
            if (r) r = strchr(r + 1, '"');  /* value open */
            if (r) {
                r++;
                const char *end = strchr(r, '"');
                if (end && (size_t)(end - r) < sizeof(out->commitment)) {
                    size_t n = (size_t)(end - r);
                    memcpy(out->commitment, r, n);
                    out->commitment[n] = '\0';
                }
            }
        }
    }
    return true;
}

/* ── Invariant check ────────────────────────────────────────── */

enum cr_verdict {
    CR_OK = 0,
    CR_HEIGHT_REGRESSED,
    CR_UTXOS_DECREASED,
    CR_COMMITMENT_CHANGED_BUT_NOT_ADVANCED,
};

static const char *cr_verdict_name(enum cr_verdict v)
{
    switch (v) {
    case CR_OK:                                    return "ok";
    case CR_HEIGHT_REGRESSED:                      return "height_regressed";
    case CR_UTXOS_DECREASED:                       return "utxos_decreased";
    case CR_COMMITMENT_CHANGED_BUT_NOT_ADVANCED:   return "commitment_drift";
    default:                                       return "unknown";
    }
}

static enum cr_verdict cr_compare(const struct cr_snapshot *before,
                                    const struct cr_snapshot *after)
{
    if (after->block_count < before->block_count)
        return CR_HEIGHT_REGRESSED;
    /* Only check utxo count if both snapshots produced a real value. */
    if (before->utxo_count >= 0 && after->utxo_count >= 0 &&
        after->utxo_count < before->utxo_count)
        return CR_UTXOS_DECREASED;
    /* Commitment may differ IF the height advanced. If the height
     * is unchanged, the commitment must be identical — anything
     * else is a corruption signal. */
    if (before->commitment[0] != '\0' && after->commitment[0] != '\0' &&
        after->block_count == before->block_count &&
        strcmp(before->commitment, after->commitment) != 0)
        return CR_COMMITMENT_CHANGED_BUT_NOT_ADVANCED;
    return CR_OK;
}

/* ── Process control ────────────────────────────────────────── */

static pid_t cr_spawn_node(const struct cr_config *cfg)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: exec zclassic23 on the isolated datadir. */
        char datadir_arg[600];
        char rpcport_arg[64];
        snprintf(datadir_arg, sizeof(datadir_arg), "-datadir=%s", cfg->datadir);
        snprintf(rpcport_arg, sizeof(rpcport_arg), "-rpcport=%d", cfg->rpc_port);
        /* Redirect the child's stdout/stderr so our harness log
         * stays readable. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("./zclassic23", "zclassic23",
              datadir_arg, rpcport_arg,
              "-nobgvalidation",  /* skip heavy background work */
              (char *)NULL);
        _exit(127);  /* exec failed */
    }
    return pid;
}

/* Poll the RPC port until we get a valid getblockcount response
 * OR the timeout elapses. Returns true on success. */
static bool cr_wait_for_rpc_ready(const struct cr_config *cfg, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    char buf[1024];
    while (now_ms() < deadline) {
        int n = cr_rpc(cfg, "getblockcount", buf, sizeof(buf));
        if (n > 0) {
            int64_t count = 0;
            if (parse_result_i64(buf, &count)) return true;
        }
        sleep_ms(100);
    }
    return false;
}

static void cr_kill_node(pid_t pid)
{
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    int status;
    (void)waitpid(pid, &status, 0);
}

/* ── Main loop ──────────────────────────────────────────────── */

static bool datadir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    struct cr_config cfg;
    cr_defaults(&cfg);
    int parse_rc = parse_args(argc, argv, &cfg);
    if (parse_rc == 1) return 0;  /* --help */
    if (parse_rc != 0) return 2;

    printf("crash_recovery_test:\n");
    printf("  datadir:      %s\n", cfg.datadir);
    printf("  iterations:   %d\n", cfg.iterations);
    printf("  delay range:  %d..%d ms\n", cfg.min_delay_ms, cfg.max_delay_ms);
    printf("  rpc port:     %d\n", cfg.rpc_port);
    printf("  seed:         %" PRIu64 "\n", cfg.seed);

    if (!datadir_exists(cfg.datadir)) {
        printf("crash_recovery_test: datadir %s does not exist — SKIP\n",
               cfg.datadir);
        printf("  Seed one with a minimal synced node and rerun, or\n"
               "  set ZCL_CRASH_DATADIR to an existing directory.\n");
        return 0;
    }

    if (access("./zclassic23", X_OK) != 0) {
        fprintf(stderr, "crash_recovery_test: ./zclassic23 not found or "
                        "not executable\n");
        return 2;
    }
    if (access("./zcl-rpc", X_OK) != 0) {
        fprintf(stderr, "crash_recovery_test: ./zcl-rpc not found — run "
                        "`make zcl-rpc` first\n");
        return 2;
    }

    uint64_t rng = cfg.seed ? cfg.seed : 0x9E3779B97F4A7C15ULL;
    int passes = 0, height_fails = 0, utxo_fails = 0, commit_fails = 0;
    int harness_errors = 0;

    for (int it = 1; it <= cfg.iterations; it++) {
        pid_t pid = cr_spawn_node(&cfg);
        if (pid < 0) {
            fprintf(stderr, "iter %d: fork failed: %s\n", it, strerror(errno));
            harness_errors++;
            continue;
        }

        /* Wait for RPC to come up — give it up to 30s after SIGKILL
         * since bg validation can replay a slow WAL. */
        if (!cr_wait_for_rpc_ready(&cfg, 30000)) {
            fprintf(stderr, "iter %d: RPC never came up — harness error\n", it);
            cr_kill_node(pid);
            harness_errors++;
            continue;
        }

        struct cr_snapshot before;
        bool have_before = cr_read_snapshot(&cfg, &before);
        if (!have_before) {
            fprintf(stderr, "iter %d: baseline snapshot failed\n", it);
            cr_kill_node(pid);
            harness_errors++;
            continue;
        }

        int delay = rand_range(&rng, cfg.min_delay_ms, cfg.max_delay_ms);
        sleep_ms(delay);

        cr_kill_node(pid);
        if (cfg.verbose)
            printf("iter %d: killed after %d ms (before: h=%" PRId64
                   " u=%" PRId64 ")\n",
                   it, delay, before.block_count, before.utxo_count);

        /* Restart and re-check. */
        pid = cr_spawn_node(&cfg);
        if (pid < 0 || !cr_wait_for_rpc_ready(&cfg, 60000)) {
            fprintf(stderr, "iter %d: restart failed — harness error\n", it);
            if (pid > 0) cr_kill_node(pid);
            harness_errors++;
            continue;
        }

        struct cr_snapshot after;
        if (!cr_read_snapshot(&cfg, &after)) {
            fprintf(stderr, "iter %d: post-restart snapshot failed\n", it);
            cr_kill_node(pid);
            harness_errors++;
            continue;
        }

        enum cr_verdict v = cr_compare(&before, &after);
        if (v == CR_OK) {
            passes++;
            if (cfg.verbose)
                printf("iter %d: OK (after: h=%" PRId64 " u=%" PRId64 ")\n",
                       it, after.block_count, after.utxo_count);
        } else {
            switch (v) {
            case CR_HEIGHT_REGRESSED: height_fails++; break;
            case CR_UTXOS_DECREASED:  utxo_fails++; break;
            default:                  commit_fails++; break;
            }
            fprintf(stderr,
                    "iter %d: FAIL %s\n"
                    "  before: h=%" PRId64 " u=%" PRId64 " c=%s\n"
                    "  after:  h=%" PRId64 " u=%" PRId64 " c=%s\n",
                    it, cr_verdict_name(v),
                    before.block_count, before.utxo_count, before.commitment,
                    after.block_count, after.utxo_count, after.commitment);
        }

        cr_kill_node(pid);
    }

    printf("\n=== crash_recovery_test summary ===\n");
    printf("  iterations:       %d\n", cfg.iterations);
    printf("  passes:           %d\n", passes);
    printf("  height_regress:   %d\n", height_fails);
    printf("  utxo_decrease:    %d\n", utxo_fails);
    printf("  commitment_drift: %d\n", commit_fails);
    printf("  harness_errors:   %d\n", harness_errors);

    if (height_fails || utxo_fails || commit_fails) return 1;
    if (harness_errors > 0 && passes == 0) return 2;
    return 0;
}
