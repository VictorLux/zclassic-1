/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "mining/gen.h"
#include "chain/pow.h"
#include "core/random.h"
#include "validation/chainstate.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t *g_miner_threads = NULL;
static int g_num_miner_threads = 0;

static void *miner_thread(void *arg)
{
    struct gen_context *ctx = (struct gen_context *)arg;
    printf("Miner thread started.\n");

    while (ctx->running) {
        struct block_index *tip = active_chain_tip(&ctx->ms->chain_active);
        if (!tip) {
            sleep(1);
            continue;
        }

        struct block_template *tmpl = create_new_block(
            &ctx->coinbase_script, ctx->ms, ctx->coins_tip,
            ctx->mempool, ctx->params);
        if (!tmpl) {
            sleep(1);
            continue;
        }

        unsigned int extra_nonce = 0;
        increment_extra_nonce(&tmpl->block, tip, &extra_nonce);

        bool found = false;
        for (unsigned int attempt = 0;
             attempt < 1000000 && ctx->running; attempt++) {
            /* Randomize 256-bit nonce */
            for (int b = 0; b < 32; b++)
                tmpl->block.header.nNonce.data[b] =
                    (unsigned char)(GetRand(256));

            struct uint256 hash;
            block_header_get_hash(&tmpl->block.header, &hash);

            if (CheckProofOfWork(hash, tmpl->block.header.nBits,
                                  &ctx->params->consensus)) {
                printf("Found block! attempt=%u\n", attempt);
                if (process_block_found(&tmpl->block, ctx->ms,
                                         ctx->coins_tip, ctx->params,
                                         ctx->datadir)) {
                    struct block_index *new_tip =
                        active_chain_tip(&ctx->ms->chain_active);
                    if (new_tip && new_tip->phashBlock) {
                        char hex[65];
                        uint256_get_hex(new_tip->phashBlock, hex);
                        printf("New block: height=%d hash=%s\n",
                               new_tip->nHeight, hex);
                    }
                }
                found = true;
                break;
            }
        }

        block_template_free(tmpl);
        free(tmpl);

        if (!found)
            sleep(1);
    }

    printf("Miner thread stopped.\n");
    return NULL;
}

void gen_start(struct gen_context *ctx)
{
    if (ctx->num_threads <= 0)
        ctx->num_threads = 1;

    ctx->running = true;
    g_num_miner_threads = ctx->num_threads;
    g_miner_threads = calloc((size_t)g_num_miner_threads, sizeof(pthread_t));
    if (!g_miner_threads) return;

    for (int i = 0; i < g_num_miner_threads; i++)
        pthread_create(&g_miner_threads[i], NULL, miner_thread, ctx);

    printf("Mining started with %d thread(s).\n", g_num_miner_threads);
}

void gen_stop(struct gen_context *ctx)
{
    ctx->running = false;
    for (int i = 0; i < g_num_miner_threads; i++)
        pthread_join(g_miner_threads[i], NULL);
    free(g_miner_threads);
    g_miner_threads = NULL;
    g_num_miner_threads = 0;
    printf("Mining stopped.\n");
}
