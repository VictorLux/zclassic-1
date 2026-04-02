/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Snapshot Sync Service — high-performance UTXO snapshot sync.
 *
 * Service actions for P2P snapshot protocol:
 *   handle_offer     — validate + accept incoming snapshot offer
 *   handle_fc_proofs — verify FlyClient MMB proofs (PoW chain)
 *   handle_data      — apply UTXO chunk (batch commit every 100K)
 *   handle_end       — finalize + SHA3 verify + set chain tip
 *   validate_serve_request — check PoW + rate limit for serving
 *
 * Security: Two-phase verification
 *   Phase 1 (pre-download): FlyClient — 20 random block samples
 *     with MMB inclusion proofs + PoW target checks. Proves the
 *     peer's chain has valid cumulative work (2^-80 forgery prob).
 *   Phase 2 (post-download): SHA3-256 over all UTXOs in canonical
 *     order. Proves data integrity (hash matches offered root).
 *
 * Flow: offer → zfcchallenge → zfcproofs → zsnapreq → data → end
 *
 * Uses shared node_db connection in turbo mode with batch commits.
 * State machine driven, event observable, ActiveRecord validated. */

#ifndef ZCL_SNAPSHOT_SYNC_SERVICE_H
#define ZCL_SNAPSHOT_SYNC_SERVICE_H

#include "event/event.h"
#include "net/flyclient.h"
#include <stdint.h>
#include <stdbool.h>

struct byte_stream;
struct node_db;
struct fast_sync_pow;
struct active_chain;
struct mmb_leaf_store;
struct main_state;
struct p2p_node;

enum snapsync_followup_action {
    SNAPSYNC_FOLLOWUP_NONE = 0,
    SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE = 1,
    SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ = 2,
};

enum snapsync_serve_action {
    SNAPSYNC_SERVE_ACTION_NONE = 0,
    SNAPSYNC_SERVE_ACTION_SEND_CHUNK = 1,
    SNAPSYNC_SERVE_ACTION_SEND_END = 2,
};

struct snapsync_serve_step {
    enum snapsync_serve_action action;
    int64_t chunk_offset;
    size_t chunk_len;
    uint32_t entries;
};

struct snapsync_offer_acceptance {
    bool should_begin_receive;
    bool should_store_offer_details;
    bool should_reset_offset;
    bool should_update_peer_state;
    enum peer_state peer_state;
    bool should_set_sync_state;
    enum sync_state sync_state;
};

struct snapsync_end_result {
    bool verified;
    bool should_resume_header_sync;
    bool should_update_peer_state;
    enum peer_state peer_state;
    bool should_activate_tip;
    bool should_set_sync_state;
    enum sync_state sync_state;
};

struct snapsync_serve_start {
    bool should_begin_serving;
    bool should_reset_progress;
    bool should_reset_cursor;
    bool should_update_peer_state;
    enum peer_state peer_state;
    uint64_t total_utxos;
};

struct snapsync_offer_followup {
    enum snapsync_followup_action action;
    bool should_send;
};

struct snapsync_verify_result {
    bool verified;
    enum snapsync_followup_action action;
    bool should_send;
};

struct snapsync_serve_complete {
    bool should_finish_serving;
    bool should_update_peer_state;
    enum peer_state peer_state;
};

struct snapshot_sync_service {
    enum snapshot_sync_state state;

    /* Offer details */
    uint8_t  offered_utxo_root[32];
    uint8_t  offered_mmb_root[32];
    uint8_t  offered_block_hash[32];
    int32_t  offered_height;
    uint64_t offered_count;

    /* FlyClient challenge (generated on offer accept) */
    struct fc_challenge fc_challenge;
    bool     fc_verified;     /* true after FlyClient proofs pass */

    /* Progress */
    uint64_t received_utxos;
    int64_t  start_time_us;
    uint32_t serving_peer_id;

    /* Database — shared connection, turbo mode during receive */
    struct node_db *ndb;
    bool     turbo_active;
    uint64_t last_commit_at;  /* received_utxos at last COMMIT */
};

/* ── Lifecycle ─────────────────────────────────────────────────── */

/* Initialize (called once at boot) */
void snapsync_init(struct snapshot_sync_service *svc, struct node_db *ndb);

/* Reset service back to IDLE (after complete or failed) */
void snapsync_reset(struct snapshot_sync_service *svc);

/* Global singleton — initialized lazily on first snapshot offer */
struct snapshot_sync_service *snapsync_global(void);
bool snapsync_global_initialized(void);
void snapsync_global_ensure_init(struct node_db *ndb);

/* ── Controller Actions (called from message router) ───────────── */

/* Result codes for controller actions */
enum snapsync_offer_result {
    SNAPSYNC_OFFER_ACCEPTED,      /* offer accepted, send zsnapreq */
    SNAPSYNC_OFFER_REJECTED_RANGE,     /* num_utxos or total_bytes out of range */
    SNAPSYNC_OFFER_REJECTED_NO_MMR,    /* no MMR root — can't verify chain */
    SNAPSYNC_OFFER_REJECTED_NOT_AHEAD, /* peer not far enough ahead */
    SNAPSYNC_OFFER_REJECTED_BUSY,      /* already receiving a snapshot */
    SNAPSYNC_OFFER_REJECTED_PARSE,     /* malformed wire data */
};

/* Parsed snapshot offer params (wire → struct by router) */
struct snapshot_offer_params {
    int32_t  height;
    uint8_t  block_hash[32];
    uint8_t  utxo_root[32];
    uint8_t  mmr_root[32];
    uint8_t  mmb_root[32];
    uint64_t num_utxos;
    uint64_t total_bytes;
    uint32_t peer_id;
    int32_t  our_height;  /* receiver's current chain height */
};

/* Action: handle incoming snapshot offer.
 * Validates offer, transitions IDLE → NEGOTIATING.
 * Generates FlyClient challenge (svc->fc_challenge).
 * Router should send zfcchallenge BEFORE zsnapreq.
 * Returns result code for the router to act on. */
enum snapsync_offer_result snapsync_handle_offer(
    struct snapshot_sync_service *svc,
    const struct snapshot_offer_params *params);
bool snapsync_parse_offer_params(struct snapshot_offer_params *params,
                                 struct byte_stream *s);
bool snapsync_parse_fc_response(struct fc_response *resp,
                                struct byte_stream *s);
bool snapsync_write_fc_challenge(const struct snapshot_sync_service *svc,
                                 struct byte_stream *s);
bool snapsync_write_snapshot_request(struct byte_stream *s,
                                     int32_t our_height,
                                     const uint8_t peer_ip[16]);
bool snapsync_build_fc_response(struct fc_response *resp,
                                const struct fc_challenge *challenge,
                                const struct active_chain *chain_active,
                                const struct mmb_leaf_store *leaf_store);
bool snapsync_write_fc_response(struct byte_stream *s,
                                const struct fc_response *resp);
int snapsync_activate_verified_tip(const struct snapshot_sync_service *svc,
                                   struct main_state *ms);
void snapsync_build_offer_acceptance(struct snapsync_offer_acceptance *result);
void snapsync_build_end_result(struct snapsync_end_result *result,
                               bool verified);
void snapsync_build_serve_start(struct snapsync_serve_start *result,
                                uint64_t total_utxos);
void snapsync_build_offer_followup(struct snapsync_offer_followup *result,
                                   const struct snapshot_sync_service *svc);
void snapsync_build_verify_result(struct snapsync_verify_result *result,
                                  bool verified);
void snapsync_build_serve_complete(struct snapsync_serve_complete *result);
bool snapsync_prepare_serve_step(struct snapsync_serve_step *step,
                                 struct p2p_node *node,
                                 const uint8_t *buf,
                                 int64_t buf_size);

/* Action: verify FlyClient proofs from peer.
 * Checks 20 random block samples with MMB inclusion proofs
 * and PoW target verification. If ALL pass, sets fc_verified=true.
 * Router should only send zsnapreq after this returns true. */
bool snapsync_verify_flyclient(struct snapshot_sync_service *svc,
                               const struct fc_response *resp);

/* Action: apply a chunk of UTXO data from wire.
 * Auto-transitions to RECEIVING on first chunk.
 * Returns count of UTXOs applied, or -1 on error. */
int snapsync_apply_chunk(struct snapshot_sync_service *svc,
                         const uint8_t *chunk_data, size_t chunk_len);

/* Action: handle snapshot end — finalize + SHA3 verify.
 * Only accepts from the serving peer.
 * Returns true if snapshot verified successfully. */
bool snapsync_handle_end(struct snapshot_sync_service *svc,
                         uint32_t peer_id);

/* Result codes for serve request validation */
enum snapsync_serve_result {
    SNAPSYNC_SERVE_OK,            /* serve the snapshot */
    SNAPSYNC_SERVE_BAD_POW,       /* invalid or missing PoW */
    SNAPSYNC_SERVE_RATE_LIMITED,   /* too many requests from this IP */
    SNAPSYNC_SERVE_NOT_READY,     /* snapshot not prebuilt yet */
    SNAPSYNC_SERVE_TRUNCATED,     /* malformed request */
};

/* Action: validate a snapshot serve request.
 * Checks PoW, rate limits. Returns result code. */
enum snapsync_serve_result snapsync_validate_serve_request(
    const uint8_t *pow_data, size_t pow_len,
    const uint8_t peer_ip[16]);

/* ── Low-level (used internally / by handle_offer) ─────────────── */

bool snapsync_accept_offer(struct snapshot_sync_service *svc,
                           int32_t height, uint64_t num_utxos,
                           const uint8_t utxo_root[32],
                           const uint8_t mmb_root[32],
                           const uint8_t block_hash[32],
                           uint32_t peer_id);

bool snapsync_begin_receive(struct snapshot_sync_service *svc);

bool snapsync_finalize(struct snapshot_sync_service *svc);

/* Query progress */
void snapsync_get_progress(const struct snapshot_sync_service *svc,
                           uint64_t *received, uint64_t *total,
                           double *rate_per_sec);
enum snapsync_followup_action snapsync_offer_followup_action(
    const struct snapshot_sync_service *svc);
enum snapsync_followup_action snapsync_verify_followup_action(
    bool verified);
bool snapsync_build_request_pow(const uint8_t peer_ip[16],
                                struct fast_sync_pow *pow);

/* True if snapshot sync is in any active state (not IDLE/COMPLETE/FAILED).
 * Use this to suppress block/header processing during snapshot sync. */
static inline bool snapsync_is_active(void)
{
    if (!snapsync_global_initialized()) return false;
    enum snapshot_sync_state st = snapsync_global()->state;
    return st == SNAPSYNC_NEGOTIATING || st == SNAPSYNC_RECEIVING ||
           st == SNAPSYNC_VERIFYING;
}

#endif
