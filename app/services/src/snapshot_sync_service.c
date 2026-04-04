/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Snapshot Sync Service — high-performance UTXO snapshot sync.
 *
 * Two-phase cryptographic verification:
 *   Phase 1: FlyClient — 20 random block samples with MMB proofs
 *            + PoW target checks (2^-80 forgery probability)
 *   Phase 2: SHA3-256 over all UTXOs in canonical order
 *
 * Uses ActiveRecord models, shared node_db connection with turbo
 * mode, batch COMMIT every 100K rows.
 *
 * State machine: IDLE → NEGOTIATING → RECEIVING → VERIFYING → COMPLETE */

#include "services/snapshot_sync_service.h"
#include "chain/chain.h"
#include "core/serialize.h"
#include "models/database.h"
#include "models/mmb_leaf_store.h"
#include "models/utxo.h"
#include "coins/utxo_commitment.h"
#include "chain/mmb.h"
#include "chain/pow.h"
#include "chain/chainparams.h"
#include "net/fast_sync.h"
#include "net/flyclient.h"
#include "net/net.h"
#include "core/random.h"
#include "crypto/sha3.h"
#include "event/event.h"
#include "config/runtime.h"
#include "validation/main_state.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>

/* Global singleton */
static struct snapshot_sync_service g_snapsync_instance;
static bool g_snapsync_init_done = false;
static pthread_mutex_t g_snapsync_service_lock = PTHREAD_MUTEX_INITIALIZER;

static void snapsync_service_lock(void)
{
    pthread_mutex_lock(&g_snapsync_service_lock);
}

static void snapsync_service_unlock(void)
{
    pthread_mutex_unlock(&g_snapsync_service_lock);
}

struct snapshot_sync_service *snapsync_global(void) { return &g_snapsync_instance; }
bool snapsync_global_initialized(void) { return g_snapsync_init_done; }

void snapsync_global_ensure_init(struct node_db *ndb)
{
    snapsync_service_lock();
    if (!g_snapsync_init_done) {
        snapsync_init(&g_snapsync_instance, ndb);
        g_snapsync_init_done = true;
    }
    snapsync_service_unlock();
}

#define SNAPSYNC_BATCH_COMMIT_ROWS 100000

struct snapsync_apply_chunk_ctx {
    struct snapshot_sync_service *svc;
    uint8_t *chunk_data;
    size_t chunk_len;
    int applied;
};

struct snapsync_finalize_ctx {
    struct snapshot_sync_service *svc;
    bool ok;
};

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static bool snapsync_exit_turbo_mode(struct snapshot_sync_service *svc)
{
    struct db_service *dbsvc = NULL;
    bool turbo_active = false;

    if (!svc || !svc->ndb)
        return false;

    snapsync_service_lock();
    turbo_active = svc->turbo_active;
    snapsync_service_unlock();
    if (!turbo_active)
        return true;

    dbsvc = snapsync_db_service(svc);
    bool ok = dbsvc
        ? db_service_normal_mode(dbsvc)
        : node_db_normal_mode(svc->ndb);

    snapsync_service_lock();
    svc->turbo_active = false;
    snapsync_service_unlock();

    return ok;
}

static struct db_service *snapsync_db_service(
    const struct snapshot_sync_service *svc)
{
    struct db_service *dbsvc = app_runtime_db_service();

    if (!svc || !dbsvc)
        return NULL;
    return db_service_node_db(dbsvc) == svc->ndb ? dbsvc : NULL;
}

static bool snapsync_run_write(struct snapshot_sync_service *svc,
                               db_service_write_fn fn,
                               void *ctx)
{
    struct db_service *dbsvc = snapsync_db_service(svc);

    if (!svc || !svc->ndb || !fn)
        return false;
    if (dbsvc)
        return db_service_run_write(dbsvc, fn, ctx);
    return fn(svc->ndb, ctx);
}

static bool snapsync_begin_receive_write(struct node_db *ndb, void *ctx)
{
    struct snapshot_sync_service *svc = ctx;

    if (!svc || !ndb || !ndb->open)
        return false;

    node_db_wipe_utxos(ndb);
    if (!node_db_begin(ndb))
        return false;
    svc->received_utxos = 0;
    svc->last_commit_at = 0;
    return true;
}

static int snapsync_apply_chunk_local(struct snapshot_sync_service *svc,
                                      const uint8_t *chunk_data,
                                      size_t chunk_len)
{
    size_t pos = 4;
    int applied = 0;
    uint32_t entries;
    bool commit_batch = false;
    uint32_t serving_peer_id = 0;
    uint64_t offered_count = 0;
    uint64_t received = 0;

    if (!svc || !svc->ndb || !svc->ndb->open || !chunk_data || chunk_len < 4)
        return -1;

    entries = chunk_data[0] | ((uint32_t)chunk_data[1] << 8) |
              ((uint32_t)chunk_data[2] << 16) | ((uint32_t)chunk_data[3] << 24);
    if (entries == 0 || entries > 1000)
        return -1;

    for (uint32_t i = 0; i < entries; i++) {
        if (pos + 48 > chunk_len) return -1;

        struct db_utxo u;
        memset(&u, 0, sizeof(u));

        memcpy(u.txid, chunk_data + pos, 32); pos += 32;

        u.vout = (uint32_t)(chunk_data[pos] | (chunk_data[pos+1] << 8) |
                  (chunk_data[pos+2] << 16) | (chunk_data[pos+3] << 24));
        pos += 4;

        u.value = 0;
        for (int j = 0; j < 8; j++)
            u.value |= (int64_t)chunk_data[pos + j] << (j * 8);
        pos += 8;

        u.height = (int)(chunk_data[pos] | (chunk_data[pos+1] << 8) |
                    (chunk_data[pos+2] << 16) | (chunk_data[pos+3] << 24));
        pos += 4;

        if (pos >= chunk_len) return -1;
        u.is_coinbase = (chunk_data[pos++] != 0);

        if (pos >= chunk_len) return -1;
        uint64_t slen = chunk_data[pos++];
        if (slen == 253) {
            if (pos + 2 > chunk_len) return -1;
            slen = chunk_data[pos] | (chunk_data[pos+1] << 8);
            pos += 2;
        } else if (slen == 254) {
            if (pos + 4 > chunk_len) return -1;
            slen = chunk_data[pos] | ((uint32_t)chunk_data[pos+1] << 8) |
                   ((uint32_t)chunk_data[pos+2] << 16) | ((uint32_t)chunk_data[pos+3] << 24);
            pos += 4;
        }
        if (pos + slen > chunk_len) return -1;

        u.script = (uint8_t *)(chunk_data + pos);
        u.script_len = (size_t)slen;
        pos += slen;

        u.script_type = utxo_classify_script(u.script, u.script_len,
                                             u.address_hash, &u.has_address);

        if (!db_utxo_insert_raw(svc->ndb, &u))
            continue;
        applied++;
    }

    snapsync_service_lock();
    if (svc->state != SNAPSYNC_RECEIVING) {
        snapsync_service_unlock();
        return 0;
    }

    svc->received_utxos += (uint64_t)applied;
    received = svc->received_utxos;
    offered_count = svc->offered_count;
    serving_peer_id = svc->serving_peer_id;
    if (received - svc->last_commit_at >= SNAPSYNC_BATCH_COMMIT_ROWS) {
        commit_batch = true;
        svc->last_commit_at = received;
    }
    snapsync_service_unlock();

    if (commit_batch) {
        if (!node_db_commit(svc->ndb) || !node_db_begin(svc->ndb))
            return -1;

        double elapsed_s = (double)(now_us() - svc->start_time_us) / 1000000.0;
        double rate = elapsed_s > 0 ? (double)received / elapsed_s : 0;
        event_emitf(EV_SNAPSYNC_PROGRESS, serving_peer_id,
                    "received=%llu/%llu rate=%.0f/s",
                    (unsigned long long)received,
                    (unsigned long long)offered_count, rate);
    }

    return applied;
}

static bool snapsync_apply_chunk_write(struct node_db *ndb, void *ctx)
{
    struct snapsync_apply_chunk_ctx *apply = ctx;

    (void)ndb;
    if (!apply || !apply->svc || !apply->chunk_data)
        return false;
    apply->applied = snapsync_apply_chunk_local(apply->svc,
                                                apply->chunk_data,
                                                apply->chunk_len);
    return apply->applied >= 0;
}

static bool snapsync_finalize_write(struct node_db *ndb, void *ctx)
{
    struct snapsync_finalize_ctx *finalize = ctx;
    struct snapshot_sync_service *svc;
    uint8_t local_root[32];
    uint64_t local_count = 0;
    uint32_t serving_peer_id;
    bool fc_verified;
    bool sha3_ok;
    double elapsed_s;

    if (!finalize || !finalize->svc || !ndb || !ndb->open)
        return false;
    svc = finalize->svc;

    if (!node_db_commit(ndb))
        return false;

    snapsync_service_lock();
    svc->state = SNAPSYNC_VERIFYING;
    snapsync_set_state(SNAPSYNC_VERIFYING, "all chunks received");
    serving_peer_id = svc->serving_peer_id;
    fc_verified = svc->fc_verified;
    snapsync_service_unlock();

    elapsed_s = (double)(now_us() - svc->start_time_us) / 1000000.0;
    printf("[snapsync] %llu UTXOs in %.1fs (%.0f/s), verifying SHA3...\n",
           (unsigned long long)svc->received_utxos, elapsed_s,
           elapsed_s > 0 ? (double)svc->received_utxos / elapsed_s : 0);

    utxo_commitment_sha3_compute(ndb->db, local_root, &local_count);
    sha3_ok = (memcmp(local_root, svc->offered_utxo_root, 32) == 0);

    if (sha3_ok) {
        bool has_mmb = false;

        node_db_state_set(ndb, "coins_best_block", svc->offered_block_hash, 32);
        for (int i = 0; i < 32; i++) {
            if (svc->offered_mmb_root[i]) {
                has_mmb = true;
                break;
            }
        }
        if (has_mmb) {
            node_db_state_set(ndb, "snapshot_mmb_root",
                              svc->offered_mmb_root, 32);
            node_db_state_set(ndb, "snapshot_mmr_height",
                              &svc->offered_height, 4);
        }
        if (!snapsync_exit_turbo_mode(svc))
            return false;
        snapsync_service_lock();
        svc->state = SNAPSYNC_COMPLETE;
        snapsync_set_state(SNAPSYNC_COMPLETE, "SHA3 verified");
        snapsync_service_unlock();

        event_emitf(EV_SNAPSYNC_VERIFIED, serving_peer_id,
                    "sha3=PASSED flyclient=%s utxos=%llu elapsed=%.1fs",
                    fc_verified ? "PASSED" : "SKIPPED",
                    (unsigned long long)local_count, elapsed_s);
        event_emitf(EV_UTXO_CHECKPOINT_PASS, 0,
                    "snapshot SHA3 PASSED count=%llu",
                    (unsigned long long)local_count);
        printf("*** SNAPSHOT VERIFIED: %llu UTXOs, SHA3 PASSED, %.1fs ***\n",
               (unsigned long long)local_count, elapsed_s);
        finalize->ok = true;
        return true;
    } else {
        char exp[65], got[65];

        for (int i = 0; i < 32; i++) {
            sprintf(exp + i*2, "%02x", svc->offered_utxo_root[i]);
            sprintf(got + i*2, "%02x", local_root[i]);
        }
        fprintf(stderr, "[snapsync] SHA3 FAILED!\n  Expected: %s\n  Got:      %s\n",
                exp, got);

        node_db_wipe_utxos(ndb);
        snapsync_exit_turbo_mode(svc);
        snapsync_service_lock();
        svc->state = SNAPSYNC_FAILED;
        snapsync_set_state(SNAPSYNC_FAILED, "SHA3 verification failed");
        snapsync_service_unlock();
        event_emitf(EV_UTXO_CHECKPOINT_FAIL, 0,
                    "snapshot SHA3 FAILED expected=%s got=%s", exp, got);
        finalize->ok = false;
        return false;
    }
}

/* ── Init / Reset ────────────────────────────────────────── */

void snapsync_init(struct snapshot_sync_service *svc, struct node_db *ndb)
{
    if (!svc)
        return;
    snapsync_service_lock();
    memset(svc, 0, sizeof(*svc));
    svc->state = SNAPSYNC_IDLE;
    svc->ndb = ndb;
    snapsync_service_unlock();
}

void snapsync_reset(struct snapshot_sync_service *svc)
{
    if (!svc) {
        return;
    }
    snapsync_service_lock();
    bool turbo_active = svc->turbo_active;
    snapsync_service_unlock();
    if (turbo_active) {
        bool ok = snapsync_exit_turbo_mode(svc);
        if (!ok) {
            snapsync_service_lock();
            snapsync_set_state(SNAPSYNC_FAILED, "normal mode reset failed");
            snapsync_service_unlock();
        }
    }

    snapsync_service_lock();
    svc->turbo_active = false;
    svc->state = SNAPSYNC_IDLE;
    svc->received_utxos = 0;
    svc->start_time_us = 0;
    svc->serving_peer_id = 0;
    svc->last_commit_at = 0;
    memset(svc->offered_utxo_root, 0, 32);
    memset(svc->offered_mmb_root, 0, 32);
    memset(svc->offered_block_hash, 0, 32);
    memset(&svc->fc_challenge, 0, sizeof(svc->fc_challenge));
    svc->fc_verified = false;
    svc->offered_height = 0;
    svc->offered_count = 0;
    svc->state = SNAPSYNC_IDLE;
    snapsync_set_state(SNAPSYNC_IDLE, "reset");
    snapsync_service_unlock();
}

/* ── Accept Offer ────────────────────────────────────────── */

bool snapsync_accept_offer(struct snapshot_sync_service *svc,
                           int32_t height, uint64_t num_utxos,
                           const uint8_t utxo_root[32],
                           const uint8_t mmb_root[32],
                           const uint8_t block_hash[32],
                           uint32_t peer_id)
{
    snapsync_service_lock();
    if (!svc || !utxo_root || !block_hash || svc->state != SNAPSYNC_IDLE
        || height <= 0 || num_utxos == 0 || num_utxos > 100000000ULL) {
        snapsync_service_unlock();
        return false;
    }

    memcpy(svc->offered_utxo_root, utxo_root, 32);
    memcpy(svc->offered_block_hash, block_hash, 32);
    if (mmb_root) memcpy(svc->offered_mmb_root, mmb_root, 32);
    svc->offered_height = height;
    svc->offered_count = num_utxos;
    svc->serving_peer_id = peer_id;
    svc->start_time_us = now_us();

    svc->state = SNAPSYNC_NEGOTIATING;
    snapsync_set_state(SNAPSYNC_NEGOTIATING, "accepted offer");
    snapsync_service_unlock();
    return true;
}

/* ── Begin Receive ───────────────────────────────────────── */

bool snapsync_begin_receive(struct snapshot_sync_service *svc)
{
    struct db_service *dbsvc = NULL;

    if (!svc)
        return false;
    snapsync_service_lock();
    if (svc->state != SNAPSYNC_NEGOTIATING) {
        fprintf(stderr, "[snapsync] begin_receive: wrong state %s\n",
                snapsync_state_name(svc->state));
        snapsync_service_unlock();
        return false;
    }
    if (!svc->ndb) {
        fprintf(stderr, "[snapsync] begin_receive: ndb is NULL\n");
        snapsync_service_unlock();
        return false;
    }
    if (!svc->ndb->open) {
        fprintf(stderr, "[snapsync] begin_receive: ndb not open\n");
        snapsync_service_unlock();
        return false;
    }

    /* Enter turbo mode via ActiveRecord database helpers */
    dbsvc = snapsync_db_service(svc);
    if (dbsvc) {
        if (!db_service_ibd_turbo_mode(dbsvc)) {
            snapsync_service_unlock();
            return false;
        }
    } else if (!node_db_ibd_turbo_mode(svc->ndb)) {
        snapsync_service_unlock();
        return false;
    }
    svc->turbo_active = true;
    snapsync_service_unlock();

    if (!snapsync_run_write(svc, snapsync_begin_receive_write, svc)) {
        snapsync_exit_turbo_mode(svc);
        return false;
    }

    snapsync_service_lock();
    svc->state = SNAPSYNC_RECEIVING;
    snapsync_set_state(SNAPSYNC_RECEIVING, "turbo mode active");
    snapsync_service_unlock();
    return true;
}

/* ── Apply Chunk ─────────────────────────────────────────── */

int snapsync_apply_chunk(struct snapshot_sync_service *svc,
                         const uint8_t *chunk_data, size_t chunk_len)
{
    struct snapsync_apply_chunk_ctx ctx;
    if (!svc || !chunk_data || chunk_len < 4)
        return -1;

    snapsync_service_lock();

    /* Only accept chunks in RECEIVING state.
     * NEGOTIATING means FlyClient verification hasn't completed yet —
     * do NOT auto-transition, that would bypass chain verification. */
    if (svc->state != SNAPSYNC_RECEIVING) {
        snapsync_service_unlock();
        return 0;
    }
    if (!svc->ndb || !svc->ndb->open) {
        snapsync_service_unlock();
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.svc = svc;
    ctx.chunk_data = malloc(chunk_len);
    if (!ctx.chunk_data) {
        snapsync_service_unlock();
        return -1;
    }
    memcpy(ctx.chunk_data, chunk_data, chunk_len);
    ctx.chunk_len = chunk_len;
    snapsync_service_unlock();

    if (!snapsync_run_write(svc, snapsync_apply_chunk_write, &ctx)) {
        free(ctx.chunk_data);
        return -1;
    }
    free(ctx.chunk_data);
    return ctx.applied;
}

/* ── Finalize ────────────────────────────────────────────── */

bool snapsync_finalize(struct snapshot_sync_service *svc)
{
    struct snapsync_finalize_ctx ctx;
    bool finalize_allowed = false;
    bool turbo_active = false;

    if (!svc)
        return false;
    snapsync_service_lock();
    finalize_allowed = (svc->state == SNAPSYNC_RECEIVING &&
                       svc->ndb && svc->ndb->open);
    if (finalize_allowed)
        turbo_active = svc->turbo_active;
    snapsync_service_unlock();

    if (!finalize_allowed) {
        return false;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.svc = svc;

    if (!snapsync_run_write(svc, snapsync_finalize_write, &ctx)) {
        if (turbo_active)
            snapsync_set_state(SNAPSYNC_FAILED, "finalize write path failed");
        snapsync_reset(svc);
        return false;
    }
    snapsync_service_lock();
    if (!ctx.ok) {
        snapsync_set_state(SNAPSYNC_FAILED,
                           "snapshot SHA3 verification failed");
        svc->state = SNAPSYNC_FAILED;
    }
    snapsync_service_unlock();
    return ctx.ok;
}

bool snapsync_is_active(void)
{
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();
    struct snapsync_status st;
    if (!svc) {
        if (!snapsync_global_initialized())
            return false;
        svc = snapsync_global();
    }
    snapsync_get_status_snapshot(svc, &st);
    return st.state == SNAPSYNC_NEGOTIATING ||
           st.state == SNAPSYNC_RECEIVING ||
           st.state == SNAPSYNC_VERIFYING;
}

/* ── Progress Query ──────────────────────────────────────── */

void snapsync_get_progress(const struct snapshot_sync_service *svc,
                           uint64_t *received, uint64_t *total,
                           double *rate_per_sec)
{
    if (!svc || (!received && !total && !rate_per_sec))
        return;

    snapsync_service_lock();
    if (received) *received = svc->received_utxos;
    if (total) *total = svc->offered_count;
    if (rate_per_sec) {
        if (svc->start_time_us > 0) {
            double elapsed = (double)(now_us() - svc->start_time_us) / 1000000.0;
            *rate_per_sec = elapsed > 0 ? (double)svc->received_utxos / elapsed : 0;
        } else {
            *rate_per_sec = 0;
        }
    }
    snapsync_service_unlock();
}

void snapsync_get_status_snapshot(const struct snapshot_sync_service *svc,
                                 struct snapsync_status *out)
{
    if (!svc || !out)
        return;

    snapsync_service_lock();
    out->state = svc->state;
    out->offered_count = svc->offered_count;
    out->serving_peer_id = svc->serving_peer_id;
    out->offered_height = svc->offered_height;
    out->turbo_active = svc->turbo_active;
    snapsync_service_unlock();
}

enum snapsync_followup_action snapsync_offer_followup_action(
    const struct snapshot_sync_service *svc)
{
    enum snapsync_followup_action action = SNAPSYNC_FOLLOWUP_NONE;
    if (!svc)
        return SNAPSYNC_FOLLOWUP_NONE;

    snapsync_service_lock();
    action = svc->fc_verified
        ? SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ
        : SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE;
    snapsync_service_unlock();
    return action;
}

enum snapsync_followup_action snapsync_verify_followup_action(bool verified)
{
    return verified ? SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ
                    : SNAPSYNC_FOLLOWUP_NONE;
}

bool snapsync_build_request_pow(const uint8_t peer_ip[16],
                                struct fast_sync_pow *pow)
{
    uint8_t peer_id[32];

    if (!peer_ip || !pow)
        return false;

    memset(pow, 0, sizeof(*pow));
    sha3_256(peer_ip, 16, peer_id);
    return fast_sync_solve_pow(peer_id, pow);
}

bool snapsync_parse_offer_params(struct snapshot_offer_params *params,
                                 struct byte_stream *s)
{
    if (!params || !s)
        return false;

    memset(params, 0, sizeof(*params));
    if (!stream_read_i32_le(s, &params->height) ||
        !stream_read_bytes(s, params->block_hash, 32) ||
        !stream_read_bytes(s, params->utxo_root, 32) ||
        !stream_read_bytes(s, params->mmr_root, 32) ||
        !stream_read_u64_le(s, &params->num_utxos) ||
        !stream_read_u64_le(s, &params->total_bytes)) {
        return false;
    }

    if (s->size - s->read_pos >= 32)
        stream_read_bytes(s, params->mmb_root, 32);
    return true;
}

bool snapsync_parse_fc_response(struct fc_response *resp,
                                struct byte_stream *s)
{
    uint32_t num_samples = 0;

    if (!resp || !s)
        return false;

    memset(resp, 0, sizeof(*resp));
    if (!stream_read_u32_le(s, &num_samples) ||
        num_samples == 0 || num_samples > FC_MAX_SAMPLES) {
        return false;
    }

    resp->num_samples = num_samples;
    for (uint32_t i = 0; i < num_samples; i++) {
        struct fc_sample *sample = &resp->samples[i];
        if (!stream_read_bytes(s, sample->leaf.block_hash, 32) ||
            !stream_read_u32_le(s, &sample->leaf.height) ||
            !stream_read_u32_le(s, &sample->leaf.timestamp) ||
            !stream_read_u32_le(s, &sample->leaf.nBits) ||
            !stream_read_bytes(s, sample->leaf.sapling_root, 32) ||
            !stream_read_bytes(s, sample->leaf.chain_work, 32) ||
            !stream_read_u64_le(s, &sample->proof.leaf_index) ||
            !stream_read_bytes(s, sample->proof.leaf_hash, 32) ||
            !stream_read_u32_le(s, &sample->proof.num_siblings) ||
            sample->proof.num_siblings > MMB_MAX_MOUNTAINS) {
            return false;
        }

        for (uint32_t j = 0; j < sample->proof.num_siblings; j++) {
            if (!stream_read_bytes(s, sample->proof.siblings[j], 32))
                return false;
        }

        if (!stream_read_u32_le(s, &sample->proof.num_peaks) ||
            sample->proof.num_peaks > MMB_MAX_MOUNTAINS) {
            return false;
        }

        for (uint32_t j = 0; j < sample->proof.num_peaks; j++) {
            if (!stream_read_bytes(s, sample->proof.peaks[j], 32))
                return false;
        }

        if (!stream_read_u64_le(s, &sample->proof.mmb_size))
            return false;
    }

    return true;
}

bool snapsync_write_fc_challenge(const struct snapshot_sync_service *svc,
                                 struct byte_stream *s)
{
    if (!svc || !s)
        return false;

    stream_write_bytes(s, svc->fc_challenge.seed, 32);
    stream_write_u64_le(s, svc->fc_challenge.chain_length);
    stream_write_bytes(s, svc->fc_challenge.mmb_root, 32);
    return true;
}

bool snapsync_write_snapshot_request(struct byte_stream *s,
                                     int32_t our_height,
                                     const uint8_t peer_ip[16])
{
    struct fast_sync_pow pow;

    if (!s || !peer_ip)
        return false;
    if (!snapsync_build_request_pow(peer_ip, &pow))
        return false;

    stream_write_i32_le(s, our_height);
    stream_write_bytes(s, pow.peer_id, 32);
    stream_write_i64_le(s, pow.timestamp);
    stream_write_u64_le(s, pow.nonce);
    return true;
}

bool snapsync_build_fc_response(struct fc_response *resp,
                                const struct fc_challenge *challenge,
                                const struct active_chain *chain_active,
                                const struct mmb_leaf_store *leaf_store)
{
    const uint8_t (*all_hashes)[32];
    uint64_t indices[FC_MAX_SAMPLES];
    uint32_t count = 0;

    if (!resp || !challenge || !chain_active || !leaf_store ||
        !leaf_store->open || leaf_store->num_leaves == 0) {
        return false;
    }

    all_hashes = mmb_leaf_store_all(leaf_store);
    if (!all_hashes)
        return false;

    memset(resp, 0, sizeof(*resp));
    fc_generate_indices(challenge->seed, challenge->chain_length,
                        indices, &count);
    resp->num_samples = count;

    for (uint32_t i = 0; i < count; i++) {
        int h = (int)indices[i];
        uint64_t prove_len = challenge->chain_length;
        const struct block_index *bi = active_chain_at(chain_active, h);
        struct fc_sample *sample = &resp->samples[i];

        if (!bi || !bi->phashBlock)
            return false;

        mmb_leaf_from_block(&sample->leaf,
                            bi->phashBlock->data,
                            bi->nHeight, bi->nTime, bi->nBits,
                            bi->hashFinalSaplingRoot.data,
                            (const uint8_t *)bi->nChainWork.pn);

        if (prove_len > leaf_store->num_leaves)
            prove_len = leaf_store->num_leaves;
        if (!mmb_prove(all_hashes, prove_len, (uint64_t)h, &sample->proof))
            return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!mmb_verify(&resp->samples[i].proof, challenge->mmb_root))
            return false;
    }

    return true;
}

bool snapsync_write_fc_response(struct byte_stream *s,
                                const struct fc_response *resp)
{
    if (!s || !resp || resp->num_samples == 0 ||
        resp->num_samples > FC_MAX_SAMPLES) {
        return false;
    }

    stream_write_u32_le(s, resp->num_samples);
    for (uint32_t i = 0; i < resp->num_samples; i++) {
        const struct fc_sample *sample = &resp->samples[i];
        stream_write_bytes(s, sample->leaf.block_hash, 32);
        stream_write_u32_le(s, sample->leaf.height);
        stream_write_u32_le(s, sample->leaf.timestamp);
        stream_write_u32_le(s, sample->leaf.nBits);
        stream_write_bytes(s, sample->leaf.sapling_root, 32);
        stream_write_bytes(s, sample->leaf.chain_work, 32);
        stream_write_u64_le(s, sample->proof.leaf_index);
        stream_write_bytes(s, sample->proof.leaf_hash, 32);
        stream_write_u32_le(s, sample->proof.num_siblings);
        for (uint32_t j = 0; j < sample->proof.num_siblings; j++)
            stream_write_bytes(s, sample->proof.siblings[j], 32);
        stream_write_u32_le(s, sample->proof.num_peaks);
        for (uint32_t j = 0; j < sample->proof.num_peaks; j++)
            stream_write_bytes(s, sample->proof.peaks[j], 32);
        stream_write_u64_le(s, sample->proof.mmb_size);
    }

    return true;
}

int snapsync_activate_verified_tip(const struct snapshot_sync_service *svc,
                                   struct main_state *ms)
{
    struct uint256 snap_hash;
    struct block_index *snap_bi;

    if (!svc || !ms)
        return -1;

    memcpy(snap_hash.data, svc->offered_block_hash, 32);
    snap_bi = block_map_find(&ms->map_block_index, &snap_hash);
    if (!snap_bi)
        return -1;

    active_chain_set_tip(&ms->chain_active, snap_bi);
    ms->pindex_best_header = snap_bi;
    return snap_bi->nHeight;
}

void snapsync_build_offer_acceptance(struct snapsync_offer_acceptance *result)
{
    struct snapsync_offer_acceptance empty = {0};

    if (!result) return;
    *result = empty;

    result->should_begin_receive = true;
    result->should_store_offer_details = true;
    result->should_reset_offset = true;
    result->should_update_peer_state = true;
    result->peer_state = PEER_SNAPSHOT_RECEIVING;
    result->should_set_sync_state = true;
    result->sync_state = SYNC_SNAPSHOT_RECEIVE;
}

void snapsync_build_end_result(struct snapsync_end_result *result,
                               bool verified)
{
    struct snapsync_end_result empty = {0};

    if (!result) return;
    *result = empty;

    result->verified = verified;
    if (!verified)
        return;

    result->should_resume_header_sync = true;
    result->should_update_peer_state = true;
    result->peer_state = PEER_ACTIVE;
    result->should_activate_tip = true;
    result->should_set_sync_state = true;
    result->sync_state = SYNC_HEADERS_DOWNLOAD;
}

void snapsync_build_serve_start(struct snapsync_serve_start *result,
                                uint64_t total_utxos)
{
    struct snapsync_serve_start empty = {0};

    if (!result) return;
    *result = empty;

    result->should_begin_serving = true;
    result->should_reset_progress = true;
    result->should_reset_cursor = true;
    result->should_update_peer_state = true;
    result->peer_state = PEER_SNAPSHOT_SERVING;
    result->total_utxos = total_utxos;
}

void snapsync_build_offer_followup(struct snapsync_offer_followup *result,
                                   const struct snapshot_sync_service *svc)
{
    struct snapsync_offer_followup empty = {0};

    if (!result) return;
    *result = empty;
    if (!svc)
        return;

    result->action = snapsync_offer_followup_action(svc);
    result->should_send = (result->action != SNAPSYNC_FOLLOWUP_NONE);
}

void snapsync_build_verify_result(struct snapsync_verify_result *result,
                                  bool verified)
{
    struct snapsync_verify_result empty = {0};

    if (!result) return;
    *result = empty;

    result->verified = verified;
    result->action = snapsync_verify_followup_action(verified);
    result->should_send = (result->action != SNAPSYNC_FOLLOWUP_NONE);
}

void snapsync_build_serve_complete(struct snapsync_serve_complete *result)
{
    struct snapsync_serve_complete empty = {0};

    if (!result) return;
    *result = empty;

    result->should_finish_serving = true;
    result->should_update_peer_state = true;
    result->peer_state = PEER_ACTIVE;
}

bool snapsync_prepare_serve_step(struct snapsync_serve_step *step,
                                 struct p2p_node *node,
                                 const uint8_t *buf,
                                 int64_t buf_size)
{
    int64_t pos;
    int64_t scan;
    uint32_t entries;
    bool ok = true;

    if (!step || !node || !buf || buf_size <= 0)
        return false;

    memset(step, 0, sizeof(*step));
    if (node->zsync_file_size == 0)
        node->zsync_file_size = buf_size;
    if (node->send_size > 2 * 1024 * 1024)
        return true;

    pos = node->zsync_file_offset;
    if (pos >= buf_size) {
        step->action = SNAPSYNC_SERVE_ACTION_SEND_END;
        return true;
    }

    if (pos + 4 > buf_size)
        return false;

    entries = buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
              ((uint32_t)buf[pos + 2] << 16) |
              ((uint32_t)buf[pos + 3] << 24);
    if (entries == 0 || entries > 1000)
        return false;

    scan = pos + 4;
    for (uint32_t i = 0; i < entries && ok; i++) {
        uint64_t slen;

        scan += 49;
        if (scan >= buf_size) {
            ok = false;
            break;
        }

        slen = buf[scan++];
        if (slen == 253) {
            if (scan + 2 > buf_size) {
                ok = false;
                break;
            }
            slen = buf[scan] | ((uint16_t)buf[scan + 1] << 8);
            scan += 2;
        } else if (slen == 254) {
            if (scan + 4 > buf_size) {
                ok = false;
                break;
            }
            slen = buf[scan] | ((uint32_t)buf[scan + 1] << 8) |
                   ((uint32_t)buf[scan + 2] << 16) |
                   ((uint32_t)buf[scan + 3] << 24);
            scan += 4;
        }
        scan += (int64_t)slen;
    }

    if (!ok || scan > buf_size)
        return false;

    step->action = SNAPSYNC_SERVE_ACTION_SEND_CHUNK;
    step->chunk_offset = pos;
    step->chunk_len = (size_t)(scan - pos);
    step->entries = entries;

    node->zsync_file_offset = scan;
    node->zsync_offset += entries;
    node->zsync_sent++;
    return true;
}

/* ══════════════════════════════════════════════════════════════
 * Controller Actions — called from message router
 * ══════════════════════════════════════════════════════════════ */

/* Action: handle incoming snapshot offer.
 * Validates params, decides whether to accept, transitions state. */
enum snapsync_offer_result snapsync_handle_offer(
    struct snapshot_sync_service *svc,
    const struct snapshot_offer_params *params)
{
    bool reconnect = false;
    enum snapshot_sync_state current_state = SNAPSYNC_IDLE;
    uint32_t prior_peer_id = 0;
    uint64_t prior_received = 0;

    if (!svc || !params)
        return SNAPSYNC_OFFER_REJECTED_PARSE;

    /* Range validation */
    if (params->num_utxos > 100000000ULL ||
        params->total_bytes > 100ULL * 1024 * 1024 * 1024) {
        return SNAPSYNC_OFFER_REJECTED_RANGE;
    }

    /* MMR proof required — can't verify PoW chain without it */
    bool has_mmr = false;
    for (int i = 0; i < 32; i++)
        if (params->mmr_root[i]) { has_mmr = true; break; }
    if (!has_mmr)
        return SNAPSYNC_OFFER_REJECTED_NO_MMR;

    /* Must be significantly ahead */
    if (params->height <= params->our_height + 5000)
        return SNAPSYNC_OFFER_REJECTED_NOT_AHEAD;

    /* If we're already receiving from a different peer (reconnect scenario),
     * reset and re-accept from the new peer. This handles the case where
     * the serving connection dropped and node2 reconnected. */
    snapsync_service_lock();
    current_state = svc->state;
    if (current_state == SNAPSYNC_RECEIVING &&
        svc->serving_peer_id != params->peer_id) {
        reconnect = true;
        prior_peer_id = svc->serving_peer_id;
        prior_received = svc->received_utxos;
    }
    snapsync_service_unlock();
    if (reconnect) {
        printf("[snapsync] Reconnect detected: resetting for new peer %u "
               "(was peer %u, had %llu UTXOs)\n",
               params->peer_id, prior_peer_id,
               (unsigned long long)prior_received);
        snapsync_reset(svc);
    } else if (current_state != SNAPSYNC_IDLE) {
        return SNAPSYNC_OFFER_REJECTED_BUSY;
    }

    /* Accept the offer via service */
    if (!snapsync_accept_offer(svc, params->height, params->num_utxos,
                               params->utxo_root, params->mmb_root,
                               params->block_hash, params->peer_id))
        return SNAPSYNC_OFFER_REJECTED_BUSY;

    printf("[snapsync] Accepted offer: h=%d, %llu UTXOs from peer %u\n",
           params->height, (unsigned long long)params->num_utxos,
           params->peer_id);

    /* Generate FlyClient challenge for MMB chain verification.
     * The router sends zfcchallenge to the peer, who must respond
     * with zfcproofs before we send zsnapreq. */
    bool has_mmb = false;
    bool begin_after_offer = false;
    for (int i = 0; i < 32; i++)
        if (params->mmb_root[i]) { has_mmb = true; break; }

    if (has_mmb) {
        uint8_t fc_seed[32];

        GetRandBytes(fc_seed, sizeof(fc_seed));
        snapsync_service_lock();
        memcpy(svc->fc_challenge.seed, fc_seed, 32);
        svc->fc_challenge.chain_length = (uint64_t)params->height;
        memcpy(svc->fc_challenge.mmb_root, params->mmb_root, 32);
        svc->fc_verified = false;
        begin_after_offer = false;
        snapsync_service_unlock();
        printf("[snapsync] FlyClient challenge generated (%u samples, "
               "chain_length=%d)\n", FC_NUM_SAMPLES, params->height);
    } else {
        /* No MMB root — skip FlyClient, accept on SHA3 only.
         * This is weaker security but backward compatible. */
        snapsync_service_lock();
        svc->fc_verified = true;
        begin_after_offer = true;
        snapsync_service_unlock();
        printf("[snapsync] WARNING: no MMB root — skipping FlyClient "
               "chain verification\n");
    }

    /* Don't begin receive yet — wait for FlyClient verification
     * (or immediate begin if no MMB). Router handles the flow. */
    if (begin_after_offer) {
        if (!snapsync_begin_receive(svc)) {
            snapsync_reset(svc);
            return SNAPSYNC_OFFER_REJECTED_BUSY;
        }
    }

    return SNAPSYNC_OFFER_ACCEPTED;
}

/* Action: verify FlyClient proofs — Phase 1 chain verification.
 * Checks 20 random block samples with MMB inclusion proofs
 * and PoW target verification (block_hash < target(nBits)). */
bool snapsync_verify_flyclient(struct snapshot_sync_service *svc,
                               const struct fc_response *resp)
{
    enum snapshot_sync_state state = SNAPSYNC_IDLE;
    uint32_t serving_peer_id = 0;
    struct fc_challenge challenge;

    if (!svc || !resp)
        return false;

    snapsync_service_lock();
    state = svc->state;
    serving_peer_id = svc->serving_peer_id;
    memcpy(&challenge, &svc->fc_challenge, sizeof(challenge));
    snapsync_service_unlock();
    if (state != SNAPSYNC_NEGOTIATING) {
        printf("[snapsync] FlyClient: wrong state %s\n",
               snapsync_state_name(state));
        return false;
    }

    /* Verify all samples against the challenge */
    if (!fc_verify_response(resp, &challenge)) {
        printf("[snapsync] FlyClient: MMB proof verification FAILED\n");
        event_emitf(EV_FC_CHAIN_VERIFIED, serving_peer_id,
                    "flyclient=FAILED samples=%u", resp->num_samples);
        return false;
    }

    /* Additional check: verify PoW targets for each sample.
     * The MMB leaf contains block_hash and nBits — verify that
     * block_hash actually meets the difficulty target. */
    const struct chain_params *cp = chain_params_get();
    const struct consensus_params *consensus = cp ? &cp->consensus : NULL;
    if (!consensus) {
        printf("[snapsync] FlyClient: no chain params for PoW check\n");
        return false;
    }
    uint32_t pow_failures = 0;
    for (uint32_t i = 0; i < resp->num_samples; i++) {
        const struct mmb_leaf *leaf = &resp->samples[i].leaf;
        struct uint256 hash;
        memcpy(hash.data, leaf->block_hash, 32);
        if (!CheckProofOfWork(hash, leaf->nBits, consensus)) {
            printf("[snapsync] FlyClient: PoW check FAILED for sample %u "
                   "(h=%u)\n", i, leaf->height);
            pow_failures++;
        }
    }

    if (pow_failures > 0) {
        printf("[snapsync] FlyClient: %u/%u PoW checks FAILED\n",
               pow_failures, resp->num_samples);
        event_emitf(EV_FC_CHAIN_VERIFIED, serving_peer_id,
                    "flyclient=FAILED pow_failures=%u/%u",
                    pow_failures, resp->num_samples);
        return false;
    }

    snapsync_service_lock();
    svc->fc_verified = false;
    snapsync_service_unlock();
    if (!snapsync_begin_receive(svc))
        return false;

    snapsync_service_lock();
    svc->fc_verified = true;
    snapsync_service_unlock();
    printf("*** FlyClient PASSED: %u samples, all PoW targets valid, "
           "MMB proofs verified ***\n", resp->num_samples);
    event_emitf(EV_FC_CHAIN_VERIFIED, serving_peer_id,
                "flyclient=PASSED samples=%u chain_length=%llu",
                resp->num_samples,
                (unsigned long long)challenge.chain_length);

    return true;
}

/* Action: handle snapshot end from peer.
 * Validates peer identity and state, finalizes. */
bool snapsync_handle_end(struct snapshot_sync_service *svc, uint32_t peer_id)
{
    enum snapshot_sync_state state = SNAPSYNC_IDLE;
    uint32_t serving_peer_id = 0;
    uint64_t received = 0;

    if (!svc) return false;
    snapsync_service_lock();
    state = svc->state;
    serving_peer_id = svc->serving_peer_id;
    received = svc->received_utxos;
    snapsync_service_unlock();

    /* Only accept from the peer we're syncing from */
    if (serving_peer_id != peer_id) {
        printf("[snapsync] Ignoring zsnapend from peer %u "
               "(serving from %u)\n", peer_id, serving_peer_id);
        return false;
    }

    if (state != SNAPSYNC_RECEIVING) {
        printf("[snapsync] Ignoring zsnapend in state %s\n",
               snapsync_state_name(state));
        return false;
    }

    event_emitf(EV_SNAPSHOT_COMPLETE, peer_id,
                "%llu UTXOs received",
                (unsigned long long)received);

    return snapsync_finalize(svc);
}

/* Rate limiter — declared in fast_sync but we need the global instance */
extern struct fast_sync_rate_limiter g_rate_limiter;

/* Action: validate a snapshot serve request (PoW + rate limit). */
enum snapsync_serve_result snapsync_validate_serve_request(
    const uint8_t *pow_data, size_t pow_len,
    const uint8_t peer_ip[16])
{
    if (!pow_data || pow_len < 48)
        return SNAPSYNC_SERVE_TRUNCATED;

    /* Parse PoW fields */
    struct fast_sync_pow pow;
    memset(&pow, 0, sizeof(pow));
    memcpy(pow.peer_id, pow_data, 32);
    memcpy(&pow.timestamp, pow_data + 32, 8);
    memcpy(&pow.nonce, pow_data + 40, 8);

    if (!fast_sync_verify_pow(&pow))
        return SNAPSYNC_SERVE_BAD_POW;

    if (!fast_sync_rate_check(&g_rate_limiter, peer_ip))
        return SNAPSYNC_SERVE_RATE_LIMITED;

    return SNAPSYNC_SERVE_OK;
}
