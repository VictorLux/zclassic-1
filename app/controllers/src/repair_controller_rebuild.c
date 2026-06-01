/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * rebuild_recent — deterministic, bounded recovery primitive.
 *
 * Fetches the CANONICAL recent block range from the authoritative local
 * zclassicd (the C++ ZClassic node, RPC 127.0.0.1:8232 — the network
 * authority that already holds every canonical block) and submits each
 * block through the NORMAL validated reducer/connect path. The node reorgs
 * onto the canonical chain if its local tip sits on a stale fork.
 *
 * This is NOT -reindex-chainstate: it does not wipe the UTXO set, does
 * not replay from genesis, and does not bypass validation. It only
 * fetches a small recent window of canonical block bodies and feeds them
 * to the same code path a P2P-received block would take. Every block is
 * fully validated. Re-running once the local tip already matches the
 * remote is a no-op.
 *
 * Reuses the shared legacy-oracle transport
 * (rpc/legacy_chain_oracle.h) — no new RPC client. */

#include "controllers/repair_controller_internal.h"

#include "rpc/legacy_chain_oracle.h"
#include "core/serialize.h"
#include "encoding/utilstrencodings.h"
#include "primitives/block.h"
#include "validation/chainstate.h"

/* "Recent" by construction: at ~75 s/block this window is ~4 days of
 * blocks. Anything larger is a cold-import / reindex job, not a recent
 * fork repair — we refuse it so this lever can never be mistaken for a
 * full resync. */
#define REBUILD_RECENT_MAX_RANGE   5000
/* Default look-back below the active tip when from_height is omitted.
 * Small margin so a 1-block stale fork is always inside the window. */
#define REBUILD_RECENT_DEFAULT_MARGIN 10
/* Caller-sized buffer for a full getblock verbose=0 hex string. A 2 MB
 * consensus block serializes to ≤4 MB of hex; 8 MB leaves headroom. */
#define REBUILD_RECENT_HEX_CAP     (8u * 1024u * 1024u)

/* Resolve the [lo, hi] inclusive height window to fetch.
 *
 * Returns true and fills *lo_out / *hi_out on success. Returns false on
 * a logic/bound error and writes a human-readable reason into err.
 *
 * Idempotent no-op (already at or above the remote tip) is signalled by
 * returning true with *hi_out < *lo_out; the caller treats that as a
 * zero-block success rather than an error.
 *
 * Exposed (non-static) so the unit test can exercise the range/bound/
 * idempotence math without a live zclassicd. */
bool rebuild_recent_resolve_range(int tip_height, int remote_height,
                                  int64_t from_arg, bool from_present,
                                  int *lo_out, int *hi_out,
                                  char *err, size_t err_sz)
{
    if (!lo_out || !hi_out) {
        if (err && err_sz) snprintf(err, err_sz, "null output");
        return false;
    }
    if (tip_height < 0) {
        if (err && err_sz)
            snprintf(err, err_sz, "active tip height invalid (%d)",
                     tip_height);
        return false;
    }
    if (remote_height < 0) {
        if (err && err_sz)
            snprintf(err, err_sz, "remote height invalid (%d)",
                     remote_height);
        return false;
    }

    int lo;
    if (from_present) {
        if (from_arg < 0 || from_arg > INT32_MAX) {
            if (err && err_sz)
                snprintf(err, err_sz,
                         "from_height out of range (%lld)",
                         (long long)from_arg);
            return false;
        }
        lo = (int)from_arg;
    } else {
        lo = tip_height - REBUILD_RECENT_DEFAULT_MARGIN;
        if (lo < 0) lo = 0;
    }

    /* Upper bound is the remote tip, but never advance more than one
     * "recent" window past lo in a single invocation. */
    int hi = remote_height;
    if (hi > lo + (REBUILD_RECENT_MAX_RANGE - 1))
        hi = lo + (REBUILD_RECENT_MAX_RANGE - 1);

    /* Bound guard: refuse a request whose span exceeds the recent
     * window. This only fires when lo is far below the remote tip — i.e.
     * the operator asked for a deep replay. That's cold-import's job. */
    if (remote_height - lo > (REBUILD_RECENT_MAX_RANGE - 1)) {
        if (err && err_sz)
            snprintf(err, err_sz,
                     "range [%d..%d] spans %d blocks > %d cap — use "
                     "cold-import for deep replays",
                     lo, remote_height, remote_height - lo + 1,
                     REBUILD_RECENT_MAX_RANGE);
        return false;
    }

    *lo_out = lo;
    *hi_out = hi; /* hi < lo => idempotent no-op (already at/above tip) */
    return true;
}

/* Fetch the canonical block at `height` from zclassicd and submit it
 * through the validated accept path. On success *did_advance reflects
 * whether the active tip moved as a result.
 *
 * Returns true if the block was fetched + accepted (or already present
 * and connectable); false on fetch/decode/validation failure with a
 * reason in err. */
static bool rebuild_recent_fetch_and_connect(struct repair_context *ctx,
                                             int height, char *hex_buf,
                                             bool *accepted,
                                             char *err, size_t err_sz)
{
    /* ctx retained for call-site symmetry; the reducer resolves the chain
     * context from boot_activation_controller() internally. */
    (void)ctx;
    *accepted = false;

    if (!legacy_chain_rpc_get_block_hex(height, hex_buf,
                                        REBUILD_RECENT_HEX_CAP)) {
        snprintf(err, err_sz, "fetch block %d from zclassicd failed",
                 height);
        return false;
    }

    size_t hex_len = strlen(hex_buf);
    size_t bin_len = hex_len / 2;
    unsigned char *bin = zcl_malloc(bin_len, "rebuild_recent_bin");
    if (!bin) {
        snprintf(err, err_sz, "oom decoding block %d (%zu bytes)",
                 height, bin_len);
        return false;
    }
    size_t parsed = ParseHex(hex_buf, bin, bin_len);
    if (parsed == 0) {
        free(bin);
        snprintf(err, err_sz, "hex decode failed for block %d", height);
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, bin, parsed);
    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, &s)) {
        block_free(&blk);
        stream_free(&s);
        free(bin);
        snprintf(err, err_sz, "block deserialize failed for block %d",
                 height);
        return false;
    }
    stream_free(&s);
    free(bin);

    struct validation_state state;
    validation_state_init(&state);

    /* force=true: same flag submitblock uses. The block still goes through
     * full validation in the reducer; force only means "don't pre-filter on
     * relay heuristics". This is a recovery wrapper around the same validated
     * accept path as rpc_submitblock / msg_blocks — it introduces no new
     * consensus writer.
     *
     * The synchronous reducer_ingest_block(boot_activation_controller(),
     * &blk, REDUCER_SRC_REPAIR, force=true, &state) drives the staged Job
     * pipeline and fills `state`, returning the bool verdict into `ok`, so the
     * rebuild_recent contract (byte-exact UTXO via the validated accept path)
     * is preserved. */
    bool ok = reducer_ingest_block(boot_activation_controller(), &blk,
                                   REDUCER_SRC_REPAIR, true, &state);
    block_free(&blk);

    if (!ok) {
        char msg[256];
        format_state_message(&state, msg, sizeof(msg));
        snprintf(err, err_sz, "block %d rejected: %s", height,
                 msg[0] ? msg : "validation failed");
        return false;
    }

    *accepted = true;
    return true;
}

/* Outcome of a rebuild_recent run, shared by the RPC and the programmatic
 * self-heal entry point. */
struct rebuild_recent_report {
    int from_height;
    int to_height;
    int remote_height;
    int fetched;
    int connected;
    bool reorged;
    int start_tip;
    int new_tip;
    bool at_tip;
    bool complete;
    char error[256];
};

/* Core: resolve the range, fetch + connect each canonical block through the
 * validated accept path, and fill *rep. Returns false (with rep->error set)
 * on a setup error (no node, bad range, zclassicd unreachable). A partial
 * run that hit a rejected block returns true with rep->complete == false.
 * Shared by rpc_rebuild_recent and rebuild_recent_repair. */
static bool rebuild_recent_run(struct repair_context *ctx,
                               int64_t from_arg, bool from_present,
                               struct rebuild_recent_report *rep)
{
    memset(rep, 0, sizeof(*rep));
    rep->to_height = -1;

    if (!ctx->main_state) {
        snprintf(rep->error, sizeof(rep->error),
                 "Node not fully initialized");
        return false;
    }

    int tip_height = active_chain_height(&ctx->main_state->chain_active);

    int remote_height = 0;
    if (!legacy_chain_rpc_get_block_count(&remote_height)) {
        snprintf(rep->error, sizeof(rep->error),
                 "Cannot reach zclassicd (getblockcount) — is it running?");
        return false;
    }

    int lo = 0, hi = -1;
    char rerr[256] = {0};
    if (!rebuild_recent_resolve_range(tip_height, remote_height,
                                      from_arg, from_present,
                                      &lo, &hi, rerr, sizeof(rerr))) {
        snprintf(rep->error, sizeof(rep->error), "%s",
                 rerr[0] ? rerr : "invalid rebuild range");
        return false;
    }

    int start_tip = tip_height;
    struct uint256 start_tip_hash = {0};
    {
        struct block_index *t0 =
            active_chain_tip(&ctx->main_state->chain_active);
        if (t0 && t0->phashBlock)
            start_tip_hash = *t0->phashBlock;
    }

    rep->from_height = lo;
    rep->to_height = hi;
    rep->remote_height = remote_height;
    rep->start_tip = start_tip;

    /* Idempotent no-op: nothing recent to fetch (already at/above tip). */
    if (hi < lo) {
        rep->new_tip = start_tip;
        rep->at_tip = true;
        rep->complete = true;
        printf("rebuild_recent: no-op — local tip %d already at/above "
               "remote %d\n", tip_height, remote_height);
        fflush(stdout);
        return true;
    }

    char *hex_buf = zcl_malloc(REBUILD_RECENT_HEX_CAP,
                               "rebuild_recent_hex");
    if (!hex_buf) {
        snprintf(rep->error, sizeof(rep->error),
                 "Out of memory allocating block buffer");
        return false;
    }

    printf("rebuild_recent: fetching canonical blocks [%d..%d] from "
           "zclassicd (local tip=%d, remote=%d)\n",
           lo, hi, tip_height, remote_height);
    fflush(stdout);

    int fetched = 0, connected = 0;
    bool complete = true;
    char ferr[256] = {0};
    for (int h = lo; h <= hi; h++) {
        bool accepted = false;
        if (!rebuild_recent_fetch_and_connect(ctx, h, hex_buf,
                                              &accepted, ferr,
                                              sizeof(ferr))) {
            LOG_WARN("rebuild_recent", "rebuild_recent: stopping at %d: %s",
                     h, ferr);
            complete = false;
            break;
        }
        fetched++;
        if (accepted) connected++;
        if (fetched % 100 == 0) {
            printf("rebuild_recent: processed %d/%d blocks (tip=%d)\n",
                   fetched, hi - lo + 1,
                   active_chain_height(&ctx->main_state->chain_active));
            fflush(stdout);
        }
    }

    free(hex_buf);

    int new_tip = active_chain_height(&ctx->main_state->chain_active);

    /* Reorg = the active tip's identity (not just its height) changed in
     * a way that disconnected the old tip. */
    bool reorged = false;
    {
        struct block_index *t1 =
            active_chain_tip(&ctx->main_state->chain_active);
        struct uint256 new_tip_hash = {0};
        if (t1 && t1->phashBlock) new_tip_hash = *t1->phashBlock;
        if (new_tip < start_tip) {
            reorged = true;
        } else if (new_tip == start_tip &&
                   uint256_cmp(&new_tip_hash, &start_tip_hash) != 0) {
            reorged = true;
        }
    }

    rep->fetched = fetched;
    rep->connected = connected;
    rep->reorged = reorged;
    rep->new_tip = new_tip;
    rep->at_tip = (new_tip >= remote_height);
    rep->complete = complete;
    if (!complete)
        snprintf(rep->error, sizeof(rep->error), "%s", ferr);

    printf("rebuild_recent: done — fetched=%d connected=%d start_tip=%d "
           "new_tip=%d remote=%d complete=%s\n",
           fetched, connected, start_tip, new_tip, remote_height,
           complete ? "true" : "false");
    fflush(stdout);
    return true;
}

/* Programmatic entry — see repair_controller.h. */
bool rebuild_recent_repair(int from_height)
{
    struct repair_context *ctx = repair_ctx();
    if (from_height < 0)
        from_height = 0;
    struct rebuild_recent_report rep;
    if (!rebuild_recent_run(ctx, (int64_t)from_height, true, &rep)) {
        LOG_WARN("rebuild_recent",
                 "rebuild_recent_repair(from=%d) setup error: %s",
                 from_height, rep.error[0] ? rep.error : "unknown");
        return false;
    }
    return rep.complete;
}

static bool rpc_rebuild_recent(const struct json_value *params, bool help,
                               struct json_value *result)
{
    struct repair_context *ctx = repair_ctx();
    RPC_HELP(help, result,
        "rebuild_recent ( from_height )\n"
        "\nDeterministic, bounded recovery: fetch the canonical recent\n"
        "block range from the authoritative local zclassicd and connect\n"
        "each block through the normal validated accept path, reorging\n"
        "off any stale local fork.\n"
        "\nThis is NOT a reindex: it does not wipe the UTXO set, does not\n"
        "replay from genesis, and does not bypass validation. The range\n"
        "is capped — deep replays must use cold-import.\n"
        "\nArguments:\n"
        "1. from_height (number, optional) start height; default is\n"
        "   active_tip - 10 (floored at 0).\n"
        "\nResult:\n"
        "  {\n"
        "    \"from_height\": n, \"to_height\": n, \"remote_height\": n,\n"
        "    \"fetched\": n, \"connected\": n, \"reorged\": bool,\n"
        "    \"start_tip\": n, \"new_tip\": n, \"at_tip\": bool\n"
        "  }\n");

    if (!ctx->main_state) {
        json_set_str(result, "Node not fully initialized");
        return false;
    }
    if (ctx->coins_tip && !rpc_require_chainstate_lookup_ready(
            ctx->main_state, result, "rebuild_recent",
            "Chainstate lookup"))
        return false;

    struct rpc_params p;
    rpc_params_init(&p, params);
    rpc_params_expect(&p, 0, 1);
    bool from_present = (params && json_size(params) >= 1);
    int64_t from_arg = rpc_permit_int(&p, 0, "from_height", 0);
    if (rpc_params_invalid(&p)) { rpc_params_error(&p, result); return false; }

    struct rebuild_recent_report rep;
    if (!rebuild_recent_run(ctx, from_arg, from_present, &rep)) {
        json_set_str(result, rep.error[0] ? rep.error
                                          : "invalid rebuild range");
        return false;
    }

    json_set_object(result);
    json_push_kv_int(result, "from_height", rep.from_height);
    json_push_kv_int(result, "to_height", rep.to_height);
    json_push_kv_int(result, "remote_height", rep.remote_height);
    json_push_kv_int(result, "fetched", rep.fetched);
    json_push_kv_int(result, "connected", rep.connected);
    json_push_kv_bool(result, "reorged", rep.reorged);
    json_push_kv_int(result, "start_tip", rep.start_tip);
    json_push_kv_int(result, "new_tip", rep.new_tip);
    json_push_kv_bool(result, "at_tip", rep.at_tip);
    json_push_kv_bool(result, "complete", rep.complete);
    if (!rep.complete)
        json_push_kv_str(result, "error", rep.error);
    return true;
}

void register_rebuild_recent_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "blockchain", "rebuild_recent", rpc_rebuild_recent, false },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
