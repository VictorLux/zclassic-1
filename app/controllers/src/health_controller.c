/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Health Controller — exposes detailed sync progress and service health. */

#include "controllers/health_controller.h"
#include "controllers/strong_params.h"
#include "validation/chainstate.h"
#include "net/p2p_game.h"
#include "json/json.h"
#include <string.h>
#include <time.h>

/* ── Controller context ──────────────────────────────────────── */

struct health_context {
    struct main_state *main_state;
    struct bg_validation_service *bg_valid;
    struct bg_hash_verification_service *bg_hash;
    struct connman *connman;
};

static struct health_context g_health_ctx = {0};

void rpc_health_set_state(struct main_state *ms,
                          struct bg_validation_service *bg_valid,
                          struct bg_hash_verification_service *bg_hash,
                          struct connman *cm)
{
    g_health_ctx.main_state = ms;
    g_health_ctx.bg_valid = bg_valid;
    g_health_ctx.bg_hash = bg_hash;
    g_health_ctx.connman = cm;
}

/* ── RPC: getsyncdetail ──────────────────────────────────────── */

static bool rpc_getsyncdetail(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getsyncdetail\n"
        "\nReturn detailed sync progress for all phases.\n"
        "\nResult: object with chain, bg_validation, and bg_hash_verify sections.");

    struct health_context *ctx = &g_health_ctx;
    json_set_object(result);

    /* Chain state */
    {
        struct json_value chain = {0};
        json_set_object(&chain);

        int tip_h = ctx->main_state ?
            active_chain_height(&ctx->main_state->chain_active) : -1;
        json_push_kv_int(&chain, "height", tip_h);

        struct block_index *tip = ctx->main_state ?
            active_chain_tip(&ctx->main_state->chain_active) : NULL;
        if (tip && tip->phashBlock) {
            char hex[65];
            uint256_get_hex(tip->phashBlock, hex);
            json_push_kv_str(&chain, "best_block", hex);
        }

        int peer_count = 0;
        int net_height = -1;
        if (ctx->connman) {
            peer_count = (int)ctx->connman->manager.num_nodes;
            for (size_t i = 0; i < ctx->connman->manager.num_nodes; i++) {
                struct p2p_node *n = ctx->connman->manager.nodes[i];
                if (n && n->starting_height > net_height)
                    net_height = n->starting_height;
            }
        }
        json_push_kv_int(&chain, "peers", peer_count);
        json_push_kv_int(&chain, "network_height", net_height);

        json_push_kv(result, "chain", &chain);
        json_free(&chain);
    }

    /* bg_validation progress */
    if (ctx->bg_valid) {
        struct json_value bgv = {0};
        json_set_object(&bgv);

        struct bg_validation_progress p =
            bg_validation_get_progress(ctx->bg_valid);
        json_push_kv_str(&bgv, "state",
            bg_validation_state_name(p.state));
        json_push_kv_int(&bgv, "verified_height", p.verified_height);
        json_push_kv_int(&bgv, "chain_height", p.chain_height);
        json_push_kv_int(&bgv, "sigs_verified", p.sigs_verified);
        json_push_kv_int(&bgv, "proofs_verified", p.proofs_verified);
        json_push_kv_int(&bgv, "blocks_per_sec", p.blocks_per_sec);

        if (p.chain_height > 0 && p.verified_height >= 0) {
            double pct = 100.0 * (double)(p.verified_height + 1) /
                         (double)(p.chain_height + 1);
            json_push_kv_real(&bgv, "percent_complete", pct);
        }

        json_push_kv(result, "bg_validation", &bgv);
        json_free(&bgv);
    }

    /* bg_hash_verify progress */
    if (ctx->bg_hash) {
        struct json_value bgh = {0};
        json_set_object(&bgh);

        struct bg_hash_verify_progress p =
            bg_hash_verify_get_progress(ctx->bg_hash);
        json_push_kv_str(&bgh, "state",
            bg_hash_verify_state_name(p.state));
        json_push_kv_int(&bgh, "verified_height", p.verified_height);
        json_push_kv_int(&bgh, "chain_height", p.chain_height);
        json_push_kv_int(&bgh, "mismatches", p.mismatches);

        json_push_kv(result, "bg_hash_verify", &bgh);
        json_free(&bgh);
    }

    return true;
}

/* ── RPC: getservicehealth ───────────────────────────────────── */

static bool rpc_getservicehealth(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getservicehealth\n"
        "\nReturn health status of all P2P services.\n"
        "\nResult: array of service objects with name, state, and details.");

    struct health_context *ctx = &g_health_ctx;
    json_set_array(result);

    /* P2P networking */
    {
        struct json_value svc = {0};
        json_set_object(&svc);
        json_push_kv_str(&svc, "name", "p2p");
        bool p2p_ok = ctx->connman && ctx->connman->started;
        json_push_kv_str(&svc, "state", p2p_ok ? "running" : "stopped");
        json_push_kv_int(&svc, "peers",
            ctx->connman ? (int64_t)ctx->connman->manager.num_nodes : 0);
        json_push_back(result, &svc);
        json_free(&svc);
    }

    /* bg_validation */
    {
        struct json_value svc = {0};
        json_set_object(&svc);
        json_push_kv_str(&svc, "name", "bg_validation");
        if (ctx->bg_valid) {
            struct bg_validation_progress p =
                bg_validation_get_progress(ctx->bg_valid);
            json_push_kv_str(&svc, "state",
                bg_validation_state_name(p.state));
            json_push_kv_int(&svc, "verified_height", p.verified_height);
            json_push_kv_int(&svc, "sigs_verified", p.sigs_verified);
        } else {
            json_push_kv_str(&svc, "state", "not_initialized");
        }
        json_push_back(result, &svc);
        json_free(&svc);
    }

    /* bg_hash_verify */
    {
        struct json_value svc = {0};
        json_set_object(&svc);
        json_push_kv_str(&svc, "name", "bg_hash_verify");
        if (ctx->bg_hash) {
            struct bg_hash_verify_progress p =
                bg_hash_verify_get_progress(ctx->bg_hash);
            json_push_kv_str(&svc, "state",
                bg_hash_verify_state_name(p.state));
            json_push_kv_int(&svc, "verified_height", p.verified_height);
            json_push_kv_int(&svc, "mismatches", p.mismatches);
        } else {
            json_push_kv_str(&svc, "state", "not_initialized");
        }
        json_push_back(result, &svc);
        json_free(&svc);
    }

    /* Game platform */
    {
        struct json_value svc = {0};
        json_set_object(&svc);
        json_push_kv_str(&svc, "name", "game_platform");
        json_push_kv_str(&svc, "state", "running");
        json_push_kv_int(&svc, "registered_types",
            (int64_t)game_type_count());
        json_push_back(result, &svc);
        json_free(&svc);
    }

    return true;
}

/* ── REST API helpers ─────────────────────────────────────────── */

bool api_getsyncdetail(struct json_value *result)
{
    return rpc_getsyncdetail(NULL, false, result);
}

bool api_getservicehealth(struct json_value *result)
{
    return rpc_getservicehealth(NULL, false, result);
}

/* ── Registration ────────────────────────────────────────────── */

void register_health_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "getsyncdetail",    rpc_getsyncdetail,    true },
        { "control", "getservicehealth", rpc_getservicehealth, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_append(t, &cmds[i]);
}
