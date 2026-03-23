/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "metrics/metrics.h"
#include "validation/main_state.h"
#include "net/connman.h"
#include "chain/chainparams.h"
#include "consensus/params.h"
#include "core/utiltime.h"
#include "util/timedata.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

_Atomic uint64_t g_transactions_validated = 0;
_Atomic uint64_t g_eh_solver_runs = 0;
_Atomic uint64_t g_solution_target_checks = 0;

static int64_t g_start_time = 0;
static pthread_t g_metrics_thread;

void metrics_print_art(void)
{
    printf(
        "               \033[0;1;30;90;43mXX\033[0;1;31;91;43m8"
        "\033[0;33;5;43;103m:...:\033[0;1;31;91;43m8\033[0;1;30;90;43mXS"
        "\033[0m                         \033[0;1;31;91;41mX"
        "\033[0;31;5;41;101m8XXX@\033[0;1;31;91;41m8\033[0m"
        "               \033[0;1;31;91;41m8\033[0;31;5;41;101mXXXX@"
        "\033[0m         \n");
    printf(
        "          \033[0;1;30;90;43m8\033[0;1;33;93;43m.."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m............."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m..\033[0;1;30;90;43m8"
        "\033[0m                 \033[0;31;5;41;101m.            t"
        "\033[0m       \033[0;31;5;41;101mt             X"
        "\033[0m    \n");
    printf(
        "       \033[0;1;30;90;43m8\033[0;1;33;93;43m."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m...."
        "\033[0;1;31;91;43m88\033[0;33;5;43;103m.\033[0;1;33;93;43m."
        "\033[0;1;31;91;43m8\033[0;31;43mt\033[0;1;30;90;43mX"
        "\033[0;31;43mt\033[0;1;31;91;43m88\033[0;33;5;43;103m."
        "\033[0;1;33;93;43m.\033[0;1;31;91;43m8\033[0;1;33;93;43m...."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m.\033[0;1;30;90;43m8"
        "\033[0m            \033[0;31;5;41;101m                 :"
        "\033[0m   \033[0;31;5;41;101m.                 t"
        "\033[0m  \n");
    printf(
        "     \033[0;1;30;90;43m8\033[0;1;33;93;43m......"
        "\033[0;1;31;91;43m8\033[0;1;30;90;43m8\033[0m"
        "              \033[0;1;31;91;43m8\033[0;1;33;93;43m......"
        "\033[0;1;30;90;43m8\033[0m        "
        "\033[0;31;5;41;101m%%                    ."
        "                     \033[0m \n");
    printf(
        "    \033[0;33;5;43;103m.\033[0;1;33;93;43m...."
        "\033[0;33;5;43;103m.\033[0;1;30;90;43m8\033[0m"
        "                    \033[0;1;31;91;43m88\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m.:\033[0m       "
        "\033[0;31;5;41;101m                                "
        "           .\033[0m\n");
    printf(
        "   \033[0;1;31;91;43m8\033[0;1;33;93;43m...."
        "\033[0;33;5;43;103m.\033[0;1;30;90;43m8\033[0m"
        "                       \033[0;33;5;43;103m:."
        "\033[0;1;33;93;43m..\033[0;33;5;43;103m.\033[0m     "
        "\033[0;31;5;41;101mX                                "
        "            \033[0m\n");
    printf(
        "  \033[0;1;31;91;43m8\033[0;1;33;93;43m................"
        "\033[0;1;31;91;43m8\033[0;37;43mX\033[0m "
        "\033[0;1;33;93;43m....\033[0;1;31;91;43m8\033[0;37;43mX\033[0m"
        "      \033[0;37;43m@\033[0;33;5;43;103m.\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m.\033[0m    "
        "\033[0;31;5;41;101mX                                "
        "            \033[0m\n");
    printf(
        " \033[0;33;5;43;103m.\033[0;1;33;93;43m...."
        "\033[0;33;5;43;103m.............\033[0;33;47m88"
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m....\033[0m"
        "         \033[0;37;5;43;103m@\033[0;1;33;93;43m..."
        "\033[0;33;5;43;103m:\033[0m    "
        "\033[0;31;5;41;101m                                "
        "            \033[0m\n");
    printf(
        "\033[0;33;47m8\033[0;1;31;91;43m8\033[0;1;33;93;43m...."
        "\033[0m             \033[0;1;33;93;43m:\033[0;1;31;91;43m8"
        "\033[0;1;33;93;43m....\033[0m           "
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m.\033[0;37;43m8\033[0m   "
        "\033[0;31;5;41;101m                                "
        "           \033[0m \n");
    printf(
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m..."
        "\033[0;1;31;91;43m8\033[0m             "
        "\033[0;1;33;93;43m......\033[0m            "
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m..."
        "\033[0;37;5;43;103m@\033[0m    "
        "\033[0;31;5;41;101m                                "
        "         .\033[0m \n");
    printf(
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m..."
        "\033[0;1;31;91;43m8\033[0m            "
        "\033[0;1;33;93;43m......\033[0m             "
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m..."
        "\033[0;33;5;43;103m.\033[0m     "
        "\033[0;31;5;41;101m                                "
        "       ;\033[0m  \n");
    printf(
        "\033[0;37;43m@\033[0;1;31;91;43m8\033[0;1;33;93;43m..."
        "\033[0;1;31;91;43m8\033[0m          "
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m..."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m:\033[0m"
        "              \033[0;33;5;43;103m.\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m.\033[0;33;47m8\033[0m      "
        "\033[0;31;5;41;101m.                              "
        "      \033[0m    \n");
    printf(
        " \033[0;1;33;93;43m.....\033[0m         "
        "\033[0;1;33;93;43m.....\033[0;1;31;91;43m8"
        "\033[0;33;5;43;103m................\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m:\033[0m         "
        "\033[0;31;5;41;101m.                             "
        "  .\033[0m      \n");
    printf(
        "  \033[0;1;31;91;43m8\033[0;1;33;93;43m..."
        "\033[0;33;5;43;103m.\033[0m      "
        "\033[0;37;43m@\033[0;1;33;93;43m..................."
        "\033[0;1;31;91;43m8\033[0;33;5;43;103m.."
        "\033[0;1;33;93;43m..\033[0;33;5;43;103m.\033[0m"
        "            \033[0;31;5;41;101m:                     "
        "      t\033[0m        \n");
    printf(
        "  \033[0;1;30;90;43m8\033[0;1;31;91;43m8"
        "\033[0;1;33;93;43m....\033[0m     "
        "\033[0;31;43mSSSSSSSSSSSSSSSSSS\033[0;1;30;90;43mX"
        "\033[0;33;5;43;103m:..\033[0;1;33;93;43m.."
        "\033[0;33;5;43;103m.\033[0m               "
        "\033[0;31;5;41;101mt                       S"
        "\033[0m          \n");
    printf(
        "    \033[0;1;33;93;43m....\033[0;1;31;91;43m8"
        "\033[0;1;33;93;43m.\033[0m                     "
        "\033[0;1;31;91;43m88\033[0;33;5;43;103m...:\033[0m"
        "                  \033[0;1;31;91;41m8"
        "\033[0;31;5;41;101m                   \033[0;1;31;91;41m8"
        "\033[0m            \n");
    printf(
        "     \033[0;1;31;91;43m88\033[0;1;33;93;43m....."
        "\033[0;1;30;90;43m8\033[0m               "
        "\033[0;1;30;90;43m8\033[0;1;33;93;43m."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m..."
        "\033[0;1;31;91;43m8\033[0;37;43m@\033[0m"
        "                      \033[0;31;5;41;101m:"
        "             :\033[0m               \n");
    printf(
        "       \033[0;1;30;90;43m8\033[0;33;5;43;103m."
        "\033[0;1;33;93;43m.....\033[0;1;31;91;43m8"
        "\033[0;33;5;43;103m.\033[0;1;33;93;43m.\033[0;1;31;91;43m8"
        "\033[0;31;43mS\033[0;1;30;90;43m@X8\033[0;31;43mS"
        "\033[0;1;30;90;43mX\033[0;1;31;91;43m8\033[0;33;5;43;103m."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m....."
        "\033[0;33;5;43;103m.\033[0;1;30;90;43m8\033[0m"
        "                          \033[0;31;5;41;101mt"
        "         X\033[0m                 \n");
    printf(
        "          \033[0;1;30;90;43mX\033[0;33;5;43;103m."
        "\033[0;1;31;91;43m8\033[0;1;33;93;43m..............."
        "\033[0;1;31;91;43m8\033[0;33;5;43;103m."
        "\033[0;1;30;90;43m@\033[0m"
        "                               \033[0;1;31;91;41m8"
        "\033[0;31;5;41;101m     \033[0;1;31;91;41m8"
        "\033[0m                   \n");
    printf(
        "               \033[0;1;30;90;43mX\033[0;1;31;91;43m8"
        "\033[0;33;5;43;103m.......\033[0;1;31;91;43m8"
        "\033[0;1;30;90;43mX\033[0m"
        "                                       "
        "\033[0;31;5;41;101m.\033[0m                      \n");
    printf("\n");
    printf("  Thank you for running a ZClassic node!\n");
    printf("  You're helping to strengthen the network"
           " and contributing to a social good :)\n");
    printf("\n");
}

static int estimate_net_height(const struct consensus_params *cp,
                               int cur_height, int64_t cur_time)
{
    int64_t now = GetAdjustedTime();
    if (cur_time >= now)
        return cur_height;
    int spacing = (int)consensus_pow_target_spacing(cp, cur_height);
    if (spacing <= 0) spacing = 150;
    int est = cur_height + (int)((now - cur_time) / spacing);
    return ((est + 5) / 10) * 10;
}

static int print_stats(struct metrics_context *ctx)
{
    int lines = 4;

    zcl_mutex_lock(&ctx->ms->cs_main);
    struct block_index *tip = active_chain_tip(&ctx->ms->chain_active);
    int height = tip ? tip->nHeight : 0;
    int64_t tip_time = tip ? (int64_t)tip->nTime : 0;
    struct block_index *best_hdr = ctx->ms->pindex_best_header;
    int hdr_height = best_hdr ? best_hdr->nHeight : height;
    int64_t hdr_time = best_hdr ? (int64_t)best_hdr->nTime : 0;
    bool importing = atomic_load(&ctx->ms->fImporting);
    bool reindexing = atomic_load(&ctx->ms->fReindex);
    zcl_mutex_unlock(&ctx->ms->cs_main);

    size_t connections = connman_get_node_count(ctx->cm);

    bool downloading = importing || reindexing ||
                       (tip_time > 0 &&
                        (GetTime() - tip_time) > 24 * 60 * 60);

    if (downloading) {
        int net_h = hdr_height;
        if (hdr_time > 0)
            net_h = estimate_net_height(&ctx->params->consensus,
                                        hdr_height, hdr_time);
        if (net_h < 1) net_h = 1;
        int pct = height * 100 / net_h;
        printf("     Downloading blocks | %d / ~%d (%d%%)           \n",
               height, net_h, pct);
    } else {
        printf("           Block height | %d                      \n",
               height);
    }
    printf("            Connections | %zu    \n", connections);
    printf("  Network solution rate | 0 Sol/s    \n");
    printf("\n");

    return lines;
}

static int print_mining_status(bool mining)
{
    int lines = 1;
    if (mining) {
        printf("Mining is active.\n");
        lines += 1;
    } else {
        printf("You are currently not mining.\n");
        printf("To enable mining, add 'gen=1' to your "
               "zclassic.conf and restart.\n");
        lines += 2;
    }
    printf("\n");
    return lines;
}

static int print_metrics(bool mining)
{
    int lines = 3;

    int64_t uptime = GetTime() - g_start_time;
    int days = (int)(uptime / 86400);
    int hours = (int)((uptime % 86400) / 3600);
    int minutes = (int)((uptime % 3600) / 60);
    int seconds = (int)(uptime % 60);

    if (days > 0)
        printf("Since starting this node %d days, %d hours, "
               "%d minutes, %d seconds ago:\n",
               days, hours, minutes, seconds);
    else if (hours > 0)
        printf("Since starting this node %d hours, "
               "%d minutes, %d seconds ago:\n",
               hours, minutes, seconds);
    else if (minutes > 0)
        printf("Since starting this node %d minutes, "
               "%d seconds ago:\n", minutes, seconds);
    else
        printf("Since starting this node %d seconds ago:\n", seconds);

    uint64_t validated = atomic_load(&g_transactions_validated);
    if (validated > 1)
        printf("- You have validated %lu transactions!\n",
               (unsigned long)validated);
    else if (validated == 1)
        printf("- You have validated a transaction!\n");
    else
        printf("- You have validated no transactions.\n");

    if (mining) {
        uint64_t runs = atomic_load(&g_eh_solver_runs);
        printf("- You have completed %lu Equihash solver runs.\n",
               (unsigned long)runs);
        lines++;
    }
    printf("\n");

    return lines;
}

static void *metrics_thread_fn(void *arg)
{
    struct metrics_context *ctx = (struct metrics_context *)arg;
    g_start_time = GetTime();

    bool is_tty = isatty(STDOUT_FILENO);

    if (is_tty) {
        printf("\033[2J");
        metrics_print_art();
    }

    while (atomic_load(&ctx->running)) {
        int lines = 1;

        if (is_tty)
            printf("\033[J");

        lines += print_stats(ctx);
        lines += print_mining_status(ctx->mining);
        lines += print_metrics(ctx->mining);

        if (is_tty) {
            printf("[Press Ctrl+C to exit] "
                   "[Set 'showmetrics=0' to hide]\n");
        } else {
            printf("----------------------------------------\n");
        }

        fflush(stdout);
        sleep(1);

        if (is_tty)
            printf("\033[%dA", lines);
    }
    return NULL;
}

void metrics_start(struct metrics_context *ctx)
{
    atomic_store(&ctx->running, true);
    pthread_create(&g_metrics_thread, NULL, metrics_thread_fn, ctx);
}

void metrics_stop(struct metrics_context *ctx)
{
    atomic_store(&ctx->running, false);
    pthread_join(g_metrics_thread, NULL);
}
