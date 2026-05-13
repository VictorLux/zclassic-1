/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the zclassicd oracle service. The mock RPC server is a
 * single-threaded loop that listens on a high port and replies to one
 * HTTP/1.1 JSON-RPC POST with a canned `result` hex string.
 *
 * Coverage:
 *   1. probe agrees when local block_index hash matches mock response.
 *   2. probe disagrees when mock returns a different hash.
 *   3. probe records rpc_errors when the mock listener is shut down.
 *   4. heartbeat tick increments probes_total.
 */

#include "test/test_helpers.h"
#include "services/zclassicd_oracle_service.h"
#include "controllers/wallet_helpers.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "event/event.h"
#include "health/heartbeat.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ZO_CHECK(name, expr) do {              \
    printf("oracle: %s... ", (name));          \
    if ((expr)) printf("OK\n");                \
    else { printf("FAIL\n"); failures++; }     \
} while (0)

/* ── Mock RPC server ──────────────────────────────────────────────
 *
 * Single-shot listener: accepts one connection, reads (and discards)
 * the HTTP request, writes a canned JSON-RPC response, closes. Spawns
 * a fresh thread per test so each test's port is fresh.
 *
 * `canned_hex` may be NULL (sends a JSON error body) or a 64-char
 * hex string for the .result field. */

struct mock_server {
    int listen_fd;
    int port;
    const char *canned_hex;       /* NULL → respond with JSON error */
    _Atomic int requests_served;
    _Atomic bool stop;
    pthread_t thread;
};

static void *mock_server_loop(void *arg)  /* raw-pthread-ok: test-local */
{
    struct mock_server *m = arg;
    while (!atomic_load(&m->stop)) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(m->listen_fd, (struct sockaddr *)&cli, &cl);
        if (cfd < 0) break;

        /* Read until we see end-of-headers marker (\r\n\r\n), then
         * consume up to Content-Length more bytes. Time-bounded by
         * the recv timeout below. */
        char buf[4096];
        size_t got = 0;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (;;) {
            ssize_t n = recv(cfd, buf + got, sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
            buf[got] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
            if (got >= sizeof(buf) - 1) break;
        }

        /* Build JSON-RPC body */
        char body[256];
        int bl;
        if (m->canned_hex) {
            bl = snprintf(body, sizeof(body),
                "{\"result\":\"%s\",\"error\":null,\"id\":\"zcl-oracle\"}\n",
                m->canned_hex);
        } else {
            bl = snprintf(body, sizeof(body),
                "{\"result\":null,\"error\":{\"code\":-1,"
                "\"message\":\"mock failure\"},\"id\":\"zcl-oracle\"}\n");
        }

        char resp[512];
        int rl = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n%s", bl, body);
        (void)send(cfd, resp, (size_t)rl, 0);
        close(cfd);
        atomic_fetch_add(&m->requests_served, 1);
    }
    return NULL;
}

static bool mock_server_start(struct mock_server *m, const char *canned_hex)
{
    memset(m, 0, sizeof(*m));
    m->canned_hex = canned_hex;
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return false;

    int one = 1;
    setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;  /* OS-chosen */
    if (bind(m->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(m->listen_fd);
        return false;
    }
    socklen_t sl = sizeof(sa);
    if (getsockname(m->listen_fd, (struct sockaddr *)&sa, &sl) < 0) {
        close(m->listen_fd);
        return false;
    }
    m->port = ntohs(sa.sin_port);
    if (listen(m->listen_fd, 4) < 0) {
        close(m->listen_fd);
        return false;
    }
    /* raw-pthread-ok: short-burst-joined-immediately */
    if (pthread_create(&m->thread, NULL, mock_server_loop, m) != 0) {
        close(m->listen_fd);
        return false;
    }
    return true;
}

static void mock_server_stop(struct mock_server *m)
{
    atomic_store(&m->stop, true);
    /* Closing the listen fd interrupts accept() so the thread exits. */
    if (m->listen_fd >= 0) {
        shutdown(m->listen_fd, SHUT_RDWR);
        close(m->listen_fd);
        m->listen_fd = -1;
    }
    pthread_join(m->thread, NULL);
}

/* ── Fixture: a tiny main_state with one block at height 7 ─────── */

static struct main_state g_zo_ms;
static struct uint256    g_zo_hash7;

static void zo_build_fixture(const char *hex64_at_h7)
{
    main_state_init(&g_zo_ms);
    uint256_set_hex(&g_zo_hash7, hex64_at_h7);

    /* Insert blocks 0..7 with synthetic hashes; height 7 = our fixture. */
    struct uint256 fillers[8];
    memset(fillers, 0, sizeof(fillers));
    for (int h = 0; h < 7; h++) {
        fillers[h].data[0] = (uint8_t)(0xC0 + h);
        struct block_index *bi = chainstate_insert_block_index(
            (struct chainstate *)&g_zo_ms, &fillers[h]);
        if (bi) bi->nHeight = h;
    }
    struct block_index *bi7 = chainstate_insert_block_index(
        (struct chainstate *)&g_zo_ms, &g_zo_hash7);
    if (bi7) bi7->nHeight = 7;

    /* Build active_chain[0..7] */
    for (int h = 0; h <= 7; h++) {
        const struct uint256 *hp = (h < 7) ? &fillers[h] : &g_zo_hash7;
        struct block_index *bi = block_map_find(&g_zo_ms.map_block_index, hp);
        active_chain_set_tip(&g_zo_ms.chain_active, bi);
    }

    /* Wire main_state into wallet_rpc_context (read by oracle service). */
    extern struct wallet_rpc_context g_wallet_ctx;
    g_wallet_ctx.main_state = &g_zo_ms;
}

static void zo_teardown(void)
{
    extern struct wallet_rpc_context g_wallet_ctx;
    g_wallet_ctx.main_state = NULL;
    main_state_free(&g_zo_ms);
    zclassicd_oracle_reset_for_test();
}

/* ── Tests ─────────────────────────────────────────────────────── */

int test_zclassicd_oracle(void);

int test_zclassicd_oracle(void)
{
    printf("\n=== zclassicd oracle tests ===\n");
    int failures = 0;

    const char *AGREE_HEX =
        "1111111122222222333333334444444455555555666666667777777788888888";
    const char *DISAGREE_HEX =
        "ffffffffeeeeeeeeddddddddccccccccbbbbbbbbaaaaaaaa9999999988888888";

    /* Test 1: probe agrees when mock returns the matching hash. */
    {
        zo_build_fixture(AGREE_HEX);
        struct mock_server srv;
        ZO_CHECK("mock server starts (agree)",
                 mock_server_start(&srv, AGREE_HEX));

        struct zclassicd_oracle_config cfg = {
            .rpc_host = "127.0.0.1",
            .rpc_port = srv.port,
            .rpc_user = "u",
            .rpc_password = "p",
            .cadence_secs = 60,
            .heights_per_tick = 1,
        };
        ZO_CHECK("init", zclassicd_oracle_init(&cfg));

        struct zclassicd_oracle_probe_result r;
        bool ok = zclassicd_oracle_probe(7, &r);
        ZO_CHECK("probe returned true", ok);
        ZO_CHECK("no rpc error",    !r.error);
        ZO_CHECK("our_have_block",   r.our_have_block);
        ZO_CHECK("hashes match",     r.match);
        ZO_CHECK("their_hash set",
                 strcasecmp(r.their_hash, AGREE_HEX) == 0);

        struct zclassicd_oracle_stats st;
        zclassicd_oracle_stats_snapshot(&st);
        ZO_CHECK("probes_total=1",    st.probes_total == 1);
        ZO_CHECK("probes_agree=1",    st.probes_agree == 1);
        ZO_CHECK("probes_disagree=0", st.probes_disagree == 0);
        ZO_CHECK("rpc_errors=0",      st.rpc_errors == 0);

        mock_server_stop(&srv);
        zo_teardown();
    }

    /* Test 2: probe disagrees when mock returns a different hash. */
    {
        zo_build_fixture(AGREE_HEX);
        struct mock_server srv;
        ZO_CHECK("mock server starts (disagree)",
                 mock_server_start(&srv, DISAGREE_HEX));

        struct zclassicd_oracle_config cfg = {
            .rpc_host = "127.0.0.1",
            .rpc_port = srv.port,
            .rpc_user = "u", .rpc_password = "p",
            .cadence_secs = 60, .heights_per_tick = 1,
        };
        ZO_CHECK("init (disagree)", zclassicd_oracle_init(&cfg));

        struct zclassicd_oracle_probe_result r;
        (void)zclassicd_oracle_probe(7, &r);
        ZO_CHECK("disagree: no rpc error", !r.error);
        ZO_CHECK("disagree: !match",       !r.match);
        ZO_CHECK("disagree: have_block",    r.our_have_block);

        struct zclassicd_oracle_stats st;
        zclassicd_oracle_stats_snapshot(&st);
        ZO_CHECK("probes_disagree=1", st.probes_disagree == 1);
        ZO_CHECK("probes_agree=0",    st.probes_agree == 0);

        mock_server_stop(&srv);
        zo_teardown();
    }

    /* Test 3: RPC error path — kill the mock listener mid-test. */
    {
        zo_build_fixture(AGREE_HEX);
        struct mock_server srv;
        ZO_CHECK("mock server starts (err)",
                 mock_server_start(&srv, AGREE_HEX));
        int dead_port = srv.port;
        mock_server_stop(&srv);  /* listener gone */

        struct zclassicd_oracle_config cfg = {
            .rpc_host = "127.0.0.1",
            .rpc_port = dead_port,
            .rpc_user = "u", .rpc_password = "p",
            .cadence_secs = 60, .heights_per_tick = 1,
        };
        ZO_CHECK("init (err)", zclassicd_oracle_init(&cfg));

        struct zclassicd_oracle_probe_result r;
        (void)zclassicd_oracle_probe(7, &r);
        ZO_CHECK("error flag set", r.error);

        struct zclassicd_oracle_stats st;
        zclassicd_oracle_stats_snapshot(&st);
        ZO_CHECK("rpc_errors >= 1", st.rpc_errors >= 1);

        zo_teardown();
    }

    /* Test 4: heartbeat tick increments probes_total. We fire the
     * sweeper at sub-second cadence by setting a small interval, and
     * lower the tip safety margin so our synthetic chain at h=7 still
     * has a valid probe range. */
    {
        setenv("ZCL_ORACLE_TIP_MARGIN", "0", 1);
        zo_build_fixture(AGREE_HEX);
        struct mock_server srv;
        ZO_CHECK("mock server starts (tick)",
                 mock_server_start(&srv, AGREE_HEX));

        struct zclassicd_oracle_config cfg = {
            .rpc_host = "127.0.0.1",
            .rpc_port = srv.port,
            .rpc_user = "u", .rpc_password = "p",
            .cadence_secs = 1,     /* fastest cadence */
            .heights_per_tick = 1,
        };
        ZO_CHECK("init (tick)", zclassicd_oracle_init(&cfg));

        health_reset_for_test();
        health_set_check_interval_ms(50);
        ZO_CHECK("oracle_start", zclassicd_oracle_start());

        /* Wait up to ~3s for the periodic tick. */
        bool saw_tick = false;
        for (int i = 0; i < 60; i++) {
            struct zclassicd_oracle_stats st;
            zclassicd_oracle_stats_snapshot(&st);
            if (st.probes_total >= 1) { saw_tick = true; break; }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        ZO_CHECK("periodic tick fired", saw_tick);

        zclassicd_oracle_stop();
        health_reset_for_test();
        mock_server_stop(&srv);
        zo_teardown();
        unsetenv("ZCL_ORACLE_TIP_MARGIN");
    }

    if (failures == 0)
        printf("=== zclassicd oracle: all checks passed ===\n");
    else
        printf("=== zclassicd oracle: %d failure(s) ===\n", failures);
    return failures;
}
