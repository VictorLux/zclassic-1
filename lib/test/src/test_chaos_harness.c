/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the Phase 6c chaos scenario parser skeleton.
 */

#include "test/test_helpers.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAOS_NO_MAIN
#include "../../../tools/sim/chaos.c"

#define CHAOS_CHECK(name, expr) do { \
    printf("chaos_harness: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int write_temp_scenario(const char *body, char *path, size_t path_cap)
{
    int n = snprintf(path, path_cap, "/tmp/zcl_chaos_harness_%d_XXXXXX",
                     (int)getpid());
    if (n < 0 || (size_t)n >= path_cap) return -1;
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(path);
        return -1;
    }
    size_t len = strlen(body);
    size_t wrote = fwrite(body, 1, len, fp);
    int close_rc = fclose(fp);
    if (wrote != len || close_rc != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int run_temp_scenario(const char *body, struct chaos_ctx *ctx_out)
{
    char path[128];
    if (write_temp_scenario(body, path, sizeof(path)) != 0)
        return 99;

    struct chaos_ctx ctx;
    chaos_ctx_init(&ctx);
    ctx.scenario_path = path;

    int rc = run_scenario(&ctx);
    if (ctx_out) *ctx_out = ctx;
    unlink(path);
    return rc;
}

int test_chaos_harness(void)
{
    printf("\n=== chaos_harness tests ===\n");
    int failures = 0;

    struct chaos_ctx ctx;
    int rc = run_temp_scenario(
        "# valid smoke\n"
        "seed 0x2a\n"
        "boot_phase listening\n"
        "peer_count 3\n"
        "expect no_crash\n"
        "expect tip_height >= 0\n"
        "expect reorg_count == 0\n",
        &ctx);
    CHAOS_CHECK("valid scenario passes", rc == 0);
    CHAOS_CHECK("valid scenario records seed",
                ctx.seed_set && ctx.seed == 0x2a);
    CHAOS_CHECK("valid scenario records boot phase",
                strcmp(ctx.boot_phase, "listening") == 0);
    CHAOS_CHECK("valid scenario records peers", ctx.peer_count == 3);
    CHAOS_CHECK("valid scenario creates simulated peers",
                ctx.peers.count == 3 && ctx.peers.active_count == 3);
    CHAOS_CHECK("valid scenario counts expects", ctx.expect_count == 3);

    rc = run_temp_scenario(
        "# comments only\n"
        "\n"
        "   # also ignored\n",
        NULL);
    CHAOS_CHECK("comment-only scenario fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "unknown_command yes\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("unknown command fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "send_block peer=0 file=missing\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("send_block without peer setup fails", rc != 0);

    char block_path[128];
    CHAOS_CHECK("block fixture write succeeds",
                write_temp_scenario("synthetic block bytes\n", block_path,
                                    sizeof(block_path)) == 0);
    char send_block_scenario[512];
    int send_block_len = snprintf(
        send_block_scenario, sizeof(send_block_scenario),
        "seed 1\n"
        "peer_count 1\n"
        "send_block peer=0 file=%s\n"
        "expect blocks_sent == 1\n"
        "expect tip_height == 1\n",
        block_path);
    rc = send_block_len > 0 && (size_t)send_block_len < sizeof(send_block_scenario)
        ? run_temp_scenario(send_block_scenario, &ctx)
        : 99;
    unlink(block_path);
    CHAOS_CHECK("send_block scenario passes", rc == 0);
    const struct sim_peer *block_peer = sim_peer_get(&ctx.peers, 0);
    CHAOS_CHECK("send_block records peer block state",
                ctx.peers.blocks_sent == 1 &&
                ctx.tip_height == 1 &&
                block_peer && block_peer->blocks_sent == 1);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 1\n"
        "send_block peer=0 file=tests/fixtures/blocks/synthetic_good_block.fixture\n"
        "expect blocks_sent == 1\n"
        "expect tip_height == 1\n",
        &ctx);
    CHAOS_CHECK("send_block checked-in fixture passes", rc == 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 3\n"
        "kill_peer 1\n"
        "expect active_peers == 2\n"
        "expect killed_peers == 1\n",
        &ctx);
    CHAOS_CHECK("kill_peer scenario passes", rc == 0);
    CHAOS_CHECK("kill_peer records simulated peer state",
                ctx.peers.count == 3 &&
                ctx.peers.active_count == 2 &&
                ctx.peers.killed_count == 1 &&
                sim_peer_get(&ctx.peers, 1) &&
                !sim_peer_get(&ctx.peers, 1)->connected);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 1\n"
        "kill_peer 3\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("kill_peer unknown peer fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 2\n"
        "send_malformed_block peer=1 type=invalid_pow\n"
        "send_malformed_block type=bad_merkle peer=1\n"
        "expect malformed_blocks == 2\n"
        "expect consensus_rejects == 2\n"
        "expect active_peers == 2\n",
        &ctx);
    CHAOS_CHECK("send_malformed_block scenario passes", rc == 0);
    const struct sim_peer *mal_peer = sim_peer_get(&ctx.peers, 1);
    CHAOS_CHECK("send_malformed_block records peer rejection state",
                ctx.consensus_rejects == 2 &&
                ctx.peers.malformed_blocks_sent == 2 &&
                ctx.peers.malformed_blocks_rejected == 2 &&
                mal_peer && mal_peer->malformed_blocks_sent == 2 &&
                strcmp(mal_peer->last_malformed_type, "bad_merkle") == 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 1\n"
        "kill_peer 0\n"
        "send_malformed_block peer=0 type=invalid_pow\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("send_malformed_block disconnected peer fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 1\n"
        "send_malformed_block peer=0 type=unknown_badness\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("send_malformed_block unknown type fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 1\n"
        "advance_clock +60s\n"
        "advance_clock 2m\n"
        "expect clock_advance_count == 2\n"
        "expect tip_height == 3\n"
        "expect clock_advance_seconds == 180\n"
        "expect no_crash\n",
        &ctx);
    CHAOS_CHECK("advance_clock scenario passes", rc == 0);
    CHAOS_CHECK("advance_clock updates virtual clock",
                ctx.clock_advance_count == 2 &&
                ctx.clock_advance_seconds == 180 &&
                ctx.sim_monotonic_us == 180000000LL &&
                ctx.tip_height == 3);

    rc = run_temp_scenario(
        "seed 1\n"
        "peer_count 2\n"
        "at_event 10 kill_peer 0\n"
        "expect scheduled_events == 1\n"
        "expect active_peers == 1\n",
        &ctx);
    CHAOS_CHECK("at_event scenario passes", rc == 0);
    CHAOS_CHECK("at_event records dispatched event",
                ctx.scheduled_event_count == 1 &&
                ctx.last_event_height == 10 &&
                ctx.peers.active_count == 1);

    rc = run_temp_scenario(
        "seed 1\n"
        "boot_phase mempool_open\n"
        "advance_clock +1h\n"
        "expect mempool_prune_runs == 1\n",
        &ctx);
    CHAOS_CHECK("clock skew records mempool prune run", rc == 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "boot_phase mempool_open\n"
        "advance_clock +1h\n"
        "expect clock_advance_seconds == 3600\n"
        "expect mempool_prune_runs == 1\n"
        "expect mempool_prunes == 1\n",
        &ctx);
    CHAOS_CHECK("advance_clock scenario passes", rc == 0);
    CHAOS_CHECK("advance_clock records simulated time effects",
                ctx.clock_advance_seconds == 3600 &&
                ctx.mempool_prune_runs == 1);

    rc = run_temp_scenario(
        "seed 1\n"
        "advance_clock bad-duration\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("bad advance_clock duration fails", rc != 0);

    rc = run_temp_scenario(
        "seed 1\n"
        "trigger_oom_at chaos_test_alloc\n"
        "expect no_crash\n",
        &ctx);
    CHAOS_CHECK("trigger_oom_at scenario passes", rc == 0);
    CHAOS_CHECK("trigger_oom_at records synthetic fire",
                ctx.alloc_fault_triggered &&
                ctx.alloc_fault_count == 1 &&
                zcl_alloc_fault_armed_label() == NULL &&
                ctx.graceful_shutdowns == 1);

    zcl_alloc_fault_fail_next("unit_alloc");
    void *failed = zcl_malloc(16, "unit_alloc");
    void *after = zcl_malloc(16, "unit_alloc");
    CHAOS_CHECK("safe_alloc fault fails once",
                failed == NULL && after != NULL &&
                zcl_alloc_fault_armed_label() == NULL);
    free(after);
    zcl_alloc_fault_clear();

    rc = run_temp_scenario(
        "seed 1\n"
        "partition_network for=5s\n"
        "expect no_crash\n",
        &ctx);
    CHAOS_CHECK("partition_network scenario passes", rc == 0);
    CHAOS_CHECK("partition_network records armed window",
                ctx.net_partition_triggered &&
                ctx.net_partition_seconds == 5 &&
                net_partition_armed_until_unix() == ctx.net_partition_until);

    net_partition_until_unix(42);
    CHAOS_CHECK("net partition active before deadline",
                net_partition_active_at(41) &&
                !net_partition_active_at(42));
    net_partition_clear();

    rc = run_temp_scenario(
        "seed 1\n"
        "expect tip_height > 0\n",
        NULL);
    CHAOS_CHECK("failing expect fails scenario", rc != 0);

    rc = run_temp_scenario(
        "seed not-a-number\n"
        "expect no_crash\n",
        NULL);
    CHAOS_CHECK("bad seed fails", rc != 0);

    if (failures == 0)
        printf("=== chaos_harness tests: ALL PASS ===\n\n");
    else
        printf("chaos_harness: failures=%d\n", failures);
    return failures;
}
