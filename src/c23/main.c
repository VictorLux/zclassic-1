/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZClassic full node — pure C23 implementation. */

#include "init/init.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -datadir=<dir>      Data directory\n");
    printf("  -paramsdir=<dir>    Zcash params directory\n");
    printf("  -testnet            Use testnet\n");
    printf("  -regtest            Use regtest\n");
    printf("  -txindex            Enable transaction index\n");
    printf("  -gen                Enable mining\n");
    printf("  -genproclimit=<n>   Mining threads (default: 1)\n");
    printf("  -mineraddress=<addr> Miner payout address\n");
    printf("  -port=<port>        P2P listen port (default: 8233)\n");
    printf("  -rpcport=<port>     RPC listen port (default: 8232)\n");
    printf("  -listen             Accept incoming P2P connections\n");
    printf("  -addnode=<ip>       Add a peer to connect to\n");
    printf("  -help               Show this help\n");
}

int main(int argc, char **argv)
{
    struct app_context ctx;
    app_context_defaults(&ctx);

    /* Default paths */
    const char *home = getenv("HOME");
    char default_datadir[512];
    char default_paramsdir[512];
    if (home) {
        snprintf(default_datadir, sizeof(default_datadir),
                 "%s/.zclassic", home);
        snprintf(default_paramsdir, sizeof(default_paramsdir),
                 "%s/.zcash-params", home);
    } else {
        snprintf(default_datadir, sizeof(default_datadir), ".zclassic");
        snprintf(default_paramsdir, sizeof(default_paramsdir), ".zcash-params");
    }
    ctx.datadir = default_datadir;
    ctx.params_dir = default_paramsdir;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) {
            ctx.datadir = argv[i] + 9;
        } else if (strncmp(argv[i], "-paramsdir=", 11) == 0) {
            ctx.params_dir = argv[i] + 11;
        } else if (strcmp(argv[i], "-testnet") == 0) {
            ctx.testnet = true;
        } else if (strcmp(argv[i], "-regtest") == 0) {
            ctx.regtest = true;
        } else if (strcmp(argv[i], "-txindex") == 0) {
            ctx.tx_index = true;
        } else if (strcmp(argv[i], "-gen") == 0) {
            ctx.gen = true;
        } else if (strncmp(argv[i], "-port=", 6) == 0) {
            ctx.p2p_port = atoi(argv[i] + 6);
        } else if (strncmp(argv[i], "-rpcport=", 9) == 0) {
            ctx.rpc_port = atoi(argv[i] + 9);
        } else if (strcmp(argv[i], "-listen") == 0) {
            ctx.listen = true;
        } else if (strncmp(argv[i], "-addnode=", 9) == 0) {
            /* handled after init */
        } else if (strncmp(argv[i], "-mineraddress=", 14) == 0) {
            ctx.miner_address = argv[i] + 14;
        } else if (strncmp(argv[i], "-genproclimit=", 14) == 0) {
            ctx.gen_threads = atoi(argv[i] + 14);
        } else if (strcmp(argv[i], "-help") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("ZClassic C23 Full Node\n");
    printf("Data directory: %s\n", ctx.datadir);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!app_init(&ctx)) {
        fprintf(stderr, "Initialization failed.\n");
        return 1;
    }

    /* Process -addnode after init */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-addnode=", 9) == 0) {
            const char *node = argv[i] + 9;
            app_add_node(node, 0);
            printf("Added node: %s\n", node);
        }
    }

    /* Main loop */
    while (!g_shutdown_requested && app_is_running()) {
        sleep(1);
    }

    app_shutdown();
    return 0;
}
