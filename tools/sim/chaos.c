/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Phase 6c chaos harness skeleton.
 *
 * This first slice proves the scenario parser and command dispatcher without
 * booting production node paths. Later Phase 6c tasks replace the stubs with
 * real peer, clock, network, and allocation fault injection.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/net_fault.h"
#include "platform/time_compat.h"
#include "sim/sim_peer.h"
#include "util/safe_alloc.h"

#define CHAOS_MAX_LINE 512
#define CHAOS_MAX_ARGS 16
#define CHAOS_MAX_EXPECTS 64

struct chaos_ctx {
    const char *scenario_path;
    uint64_t seed;
    bool seed_set;
    char boot_phase[32];
    unsigned peer_count;
    struct sim_peer_set peers;
    bool crashed;
    int64_t tip_height;
    int64_t reorg_count;
    int64_t consensus_rejects;
    size_t expect_count;
    bool verbose;
    char alloc_fault_site[64];
    bool alloc_fault_triggered;
    int64_t net_partition_seconds;
    int64_t net_partition_until;
    bool net_partition_triggered;
};

typedef int (*chaos_handler_fn)(struct chaos_ctx *ctx, int argc, char **argv,
                                int line_no);

struct chaos_command {
    const char *name;
    chaos_handler_fn handler;
};

static const char *arg_value(int argc, char **argv, const char *key)
{
    size_t key_len = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], key, key_len) == 0 && argv[i][key_len] == '=')
            return argv[i] + key_len + 1;
    }
    return NULL;
}

static char *trim_ascii(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static int split_args(char *line, char **argv, int argv_cap)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        if (argc >= argv_cap) return -E2BIG;
        argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static bool parse_i64(const char *s, int64_t *out)
{
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

static bool parse_u64_auto(const char *s, uint64_t *out)
{
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_duration_seconds(const char *s, int64_t *out)
{
    if (!s || !*s || !out) return false;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || v <= 0) return false;

    int64_t mult = 1;
    if (*end == '\0' || strcmp(end, "s") == 0) {
        mult = 1;
    } else if (strcmp(end, "m") == 0) {
        mult = 60;
    } else if (strcmp(end, "h") == 0) {
        mult = 60 * 60;
    } else if (strcmp(end, "d") == 0) {
        mult = 24 * 60 * 60;
    } else {
        return false;
    }

    if (v > INT64_MAX / mult) return false;
    *out = (int64_t)v * mult;
    return true;
}

static int fail_line(int line_no, const char *msg)
{
    fprintf(stderr, "chaos:%d: %s\n", line_no, msg);
    return -EINVAL;
}

static int handle_seed(struct chaos_ctx *ctx, int argc, char **argv,
                       int line_no)
{
    if (argc != 2) return fail_line(line_no, "seed requires one value");
    uint64_t seed = 0;
    if (!parse_u64_auto(argv[1], &seed))
        return fail_line(line_no, "seed must be an integer or hex value");
    ctx->seed = seed;
    ctx->seed_set = true;
    return 0;
}

static int handle_boot_phase(struct chaos_ctx *ctx, int argc, char **argv,
                             int line_no)
{
    if (argc != 2) return fail_line(line_no, "boot_phase requires one value");
    if (strcmp(argv[1], "idb_complete") != 0 &&
        strcmp(argv[1], "listening") != 0 &&
        strcmp(argv[1], "mempool_open") != 0) {
        return fail_line(line_no, "unknown boot_phase");
    }
    snprintf(ctx->boot_phase, sizeof(ctx->boot_phase), "%s", argv[1]);
    return 0;
}

static int handle_peer_count(struct chaos_ctx *ctx, int argc, char **argv,
                             int line_no)
{
    if (argc != 2) return fail_line(line_no, "peer_count requires one value");
    uint64_t n = 0;
    if (!parse_u64_auto(argv[1], &n) || n > 1024)
        return fail_line(line_no, "peer_count must be 0..1024");
    if (sim_peer_set_resize(&ctx->peers, (unsigned)n) != 0)
        return fail_line(line_no, "failed to create simulated peers");
    ctx->peer_count = (unsigned)n;
    return 0;
}

static int compare_metric(int64_t actual, const char *op, int64_t expected)
{
    if (strcmp(op, "==") == 0) return actual == expected ? 0 : -1;
    if (strcmp(op, "!=") == 0) return actual != expected ? 0 : -1;
    if (strcmp(op, ">=") == 0) return actual >= expected ? 0 : -1;
    if (strcmp(op, "<=") == 0) return actual <= expected ? 0 : -1;
    if (strcmp(op, ">") == 0) return actual > expected ? 0 : -1;
    if (strcmp(op, "<") == 0) return actual < expected ? 0 : -1;
    return -2;
}

static bool metric_value(const struct chaos_ctx *ctx, const char *name,
                         int64_t *out)
{
    if (strcmp(name, "tip_height") == 0) {
        *out = ctx->tip_height;
        return true;
    }
    if (strcmp(name, "reorg_count") == 0) {
        *out = ctx->reorg_count;
        return true;
    }
    if (strcmp(name, "consensus_rejects") == 0) {
        *out = ctx->consensus_rejects;
        return true;
    }
    if (strcmp(name, "active_peers") == 0) {
        *out = (int64_t)ctx->peers.active_count;
        return true;
    }
    if (strcmp(name, "killed_peers") == 0) {
        *out = (int64_t)ctx->peers.killed_count;
        return true;
    }
    if (strcmp(name, "malformed_blocks") == 0) {
        *out = (int64_t)ctx->peers.malformed_blocks_sent;
        return true;
    }
    return false;
}

static int handle_expect(struct chaos_ctx *ctx, int argc, char **argv,
                         int line_no)
{
    if (argc < 2) return fail_line(line_no, "expect requires an assertion");
    if (ctx->expect_count >= CHAOS_MAX_EXPECTS)
        return fail_line(line_no, "too many expect assertions");
    ctx->expect_count++;

    if (argc == 2 && strcmp(argv[1], "no_crash") == 0) {
        if (ctx->crashed) {
            fprintf(stderr, "chaos:%d: expect no_crash failed\n", line_no);
            return -1;
        }
        return 0;
    }

    if (argc == 4) {
        int64_t actual = 0;
        int64_t expected = 0;
        if (!metric_value(ctx, argv[1], &actual))
            return fail_line(line_no, "unknown expect metric");
        if (!parse_i64(argv[3], &expected))
            return fail_line(line_no, "expect value must be an integer");
        int cmp = compare_metric(actual, argv[2], expected);
        if (cmp == -2) return fail_line(line_no, "unknown expect operator");
        if (cmp != 0) {
            fprintf(stderr,
                    "chaos:%d: expect failed: %s %s %" PRId64
                    " (actual=%" PRId64 ")\n",
                    line_no, argv[1], argv[2], expected, actual);
            return -1;
        }
        return 0;
    }

    return fail_line(line_no, "unsupported expect assertion");
}

static int handle_stub(struct chaos_ctx *ctx, int argc, char **argv,
                       int line_no)
{
    (void)ctx;
    (void)argc;
    fprintf(stderr,
            "chaos:%d: command '%s' is recognized but not implemented yet\n",
            line_no, argv[0]);
    return -ENOTSUP;
}

static int handle_trigger_oom_at(struct chaos_ctx *ctx, int argc, char **argv,
                                 int line_no)
{
    if (argc != 2) return fail_line(line_no, "trigger_oom_at requires one label");
    if (strlen(argv[1]) >= sizeof(ctx->alloc_fault_site))
        return fail_line(line_no, "trigger_oom_at label too long");

    snprintf(ctx->alloc_fault_site, sizeof(ctx->alloc_fault_site), "%s",
             argv[1]);
    zcl_alloc_fault_fail_next(ctx->alloc_fault_site);
    void *p = zcl_malloc(1, ctx->alloc_fault_site);
    if (p) {
        free(p);
        return fail_line(line_no, "allocation fault did not fire");
    }
    if (zcl_alloc_fault_armed_label() != NULL)
        return fail_line(line_no, "allocation fault did not clear");
    ctx->alloc_fault_triggered = true;
    return 0;
}

static int handle_kill_peer(struct chaos_ctx *ctx, int argc, char **argv,
                            int line_no)
{
    if (argc != 2) return fail_line(line_no, "kill_peer requires one peer id");
    uint64_t id = 0;
    if (!parse_u64_auto(argv[1], &id) || id > UINT32_MAX)
        return fail_line(line_no, "kill_peer id must be an integer");
    int rc = sim_peer_kill(&ctx->peers, (unsigned)id);
    if (rc == -ENOENT)
        return fail_line(line_no, "kill_peer id is not configured");
    if (rc == -EALREADY)
        return fail_line(line_no, "kill_peer id is already disconnected");
    if (rc != 0)
        return fail_line(line_no, "kill_peer failed");
    return 0;
}

static int handle_send_malformed_block(struct chaos_ctx *ctx, int argc,
                                       char **argv, int line_no)
{
    if (argc != 3)
        return fail_line(line_no,
                         "send_malformed_block requires peer=I type=ENUM");

    const char *peer_arg = arg_value(argc, argv, "peer");
    const char *type = arg_value(argc, argv, "type");
    uint64_t peer_id = 0;
    if (!peer_arg || !parse_u64_auto(peer_arg, &peer_id) ||
        peer_id > UINT32_MAX) {
        return fail_line(line_no, "send_malformed_block peer must be integer");
    }
    if (!type || !sim_peer_malformed_type_known(type))
        return fail_line(line_no, "send_malformed_block unknown type");

    int rc = sim_peer_send_malformed_block(&ctx->peers, (unsigned)peer_id,
                                           type);
    if (rc == -ENOENT)
        return fail_line(line_no, "send_malformed_block peer is not configured");
    if (rc == -ENOTCONN)
        return fail_line(line_no, "send_malformed_block peer is disconnected");
    if (rc != 0)
        return fail_line(line_no, "send_malformed_block failed");

    ctx->consensus_rejects++;
    return 0;
}

static int handle_partition_network(struct chaos_ctx *ctx, int argc,
                                    char **argv, int line_no)
{
    if (argc != 2)
        return fail_line(line_no, "partition_network requires for=DURATION");

    const char *duration = argv[1];
    if (strncmp(duration, "for=", 4) == 0)
        duration += 4;

    int64_t seconds = 0;
    if (!parse_duration_seconds(duration, &seconds))
        return fail_line(line_no,
                         "partition_network duration must be Ns/Nm/Nh/Nd");

    int64_t now = platform_time_wall_unix();
    if (now > INT64_MAX - seconds)
        return fail_line(line_no, "partition_network duration overflows");

    ctx->net_partition_seconds = seconds;
    ctx->net_partition_until = now + seconds;
    net_partition_until_unix(ctx->net_partition_until);
    if (!net_partition_active_at(now))
        return fail_line(line_no, "network partition did not arm");
    ctx->net_partition_triggered = true;
    return 0;
}

static const struct chaos_command COMMANDS[] = {
    { "seed", handle_seed },
    { "boot_phase", handle_boot_phase },
    { "peer_count", handle_peer_count },
    { "expect", handle_expect },
    { "at_event", handle_stub },
    { "kill_peer", handle_kill_peer },
    { "send_block", handle_stub },
    { "send_malformed_block", handle_send_malformed_block },
    { "advance_clock", handle_stub },
    { "trigger_oom_at", handle_trigger_oom_at },
    { "partition_network", handle_partition_network },
};

static const struct chaos_command *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(COMMANDS[i].name, name) == 0)
            return &COMMANDS[i];
    }
    return NULL;
}

static int run_scenario(struct chaos_ctx *ctx)
{
    FILE *fp = fopen(ctx->scenario_path, "rb");
    if (!fp) {
        fprintf(stderr, "chaos: failed to open %s: %s\n",
                ctx->scenario_path, strerror(errno));
        return 1;
    }

    char line[CHAOS_MAX_LINE];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        } else if (!feof(fp)) {
            fprintf(stderr, "chaos:%d: line too long\n", line_no);
            fclose(fp);
            return 1;
        }

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *body = trim_ascii(line);
        if (*body == '\0') continue;

        char *argv[CHAOS_MAX_ARGS];
        int argc = split_args(body, argv, CHAOS_MAX_ARGS);
        if (argc < 0) {
            fprintf(stderr, "chaos:%d: too many arguments\n", line_no);
            fclose(fp);
            return 1;
        }

        const struct chaos_command *cmd = find_command(argv[0]);
        if (!cmd) {
            fprintf(stderr, "chaos:%d: unknown command '%s'\n",
                    line_no, argv[0]);
            fclose(fp);
            return 1;
        }
        int rc = cmd->handler(ctx, argc, argv, line_no);
        if (rc != 0) {
            fclose(fp);
            return 1;
        }
        if (ctx->verbose)
            printf("chaos:%d: %s OK\n", line_no, argv[0]);
    }

    fclose(fp);
    if (ctx->expect_count == 0) {
        fprintf(stderr, "chaos: scenario has no expect assertions\n");
        return 1;
    }
    return 0;
}

#ifndef CHAOS_NO_MAIN
static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --scenario=PATH [--verbose]\n", argv0);
}

int main(int argc, char **argv)
{
    struct chaos_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.boot_phase, sizeof(ctx.boot_phase), "idb_complete");

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--scenario=", 11) == 0) {
            ctx.scenario_path = argv[i] + 11;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            ctx.verbose = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!ctx.scenario_path || !*ctx.scenario_path) {
        usage(argv[0]);
        return 2;
    }

    int rc = run_scenario(&ctx);
    if (rc == 0) {
        printf("PASS %s seed=%s0x%016" PRIx64
               " boot_phase=%s peers=%u expects=%zu\n",
               ctx.scenario_path,
               ctx.seed_set ? "" : "(default)",
               ctx.seed,
               ctx.boot_phase,
               ctx.peer_count,
               ctx.expect_count);
    } else {
        printf("FAIL %s\n", ctx.scenario_path);
    }
    return rc;
}
#endif
