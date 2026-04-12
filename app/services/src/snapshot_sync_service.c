/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Snapshot Sync Service — high-performance UTXO snapshot sync.
 *
 * Two-phase cryptographic verification:
 *   Phase 1: FlyClient — 50 random block samples with MMB proofs
 *            + PoW target checks (≥150-bit forgery security)
 *   Phase 2: SHA3-256 over all UTXOs in canonical order
 *
 * Uses ActiveRecord models, shared node_db connection with turbo
 * mode, batch COMMIT every 100K rows.
 *
 * State machine: IDLE → NEGOTIATING → RECEIVING → VERIFYING → COMPLETE */

#include "services/snapshot_sync_service.h"
#include "services/chain_state_repository.h"
#include "services/recovery_policy.h"
#include "models/db_txn.h"
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
#include "util/trace.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"
#include "validation/contextual_check_tx.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>

/* Global singleton */
static struct snapshot_sync_service g_snapsync_instance;
static bool g_snapsync_init_done = false;
static pthread_mutex_t g_snapsync_service_lock = PTHREAD_MUTEX_INITIALIZER;

/* Snapshot anchor: placeholder block_index at verified snapshot height.
 * Used by getheaders locator to resume header sync from snapshot height
 * instead of from the (much lower) locally-indexed chain tip. */
static struct block_index *g_snapshot_anchor = NULL;

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

/* Batch commit interval.  Smaller batches (25K) produce shorter WAL
 * checkpoints (~3-5s), reducing TCP backpressure pauses that trigger
 * false stall detections.  The tradeoff is slightly more total I/O,
 * but snapshot sync is I/O-bound anyway. */
#define SNAPSYNC_BATCH_COMMIT_ROWS 25000

static struct db_service *snapsync_db_service(
    const struct snapshot_sync_service *svc);
static bool snapsync_run_write(struct snapshot_sync_service *svc,
                               db_service_write_fn fn,
                               void *ctx);

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

static void snapsync_set_db_mode_flag(struct node_db *ndb, bool turbo_mode)
{
    if (!ndb)
        return;
    if (ndb->state_mutex_init)
        zcl_mutex_lock(&ndb->state_mutex);
    ndb->turbo_mode = turbo_mode;
    if (ndb->state_mutex_init)
        zcl_mutex_unlock(&ndb->state_mutex);
}

static bool snapsync_enter_receive_mode_write(struct node_db *ndb, void *ctx)
{
    bool *ok = ctx;

    if (!ndb || !ndb->open) {
        if (ok)
            *ok = false;
        LOG_FAIL("snapshot_sync", "enter_receive_mode: ndb null or not open");
    }

    /* Use synchronous=NORMAL (not OFF) for crash safety.  In WAL mode,
     * NORMAL only fsyncs at WAL checkpoint, not every write — nearly as
     * fast as OFF but the database survives a process crash.  With OFF,
     * a crash during receive leaves the DB in an indeterminate state. */
    if (!node_db_exec(ndb, "PRAGMA synchronous=NORMAL") ||
        !node_db_exec(ndb, "PRAGMA cache_size=-524288") ||
        !node_db_exec(ndb, "PRAGMA wal_autocheckpoint=0")) {
        if (ok)
            *ok = false;
        LOG_FAIL("snapshot_sync", "enter_receive_mode: PRAGMA setup failed");
    }
    sqlite3_busy_timeout(ndb->db, 10000);
    snapsync_set_db_mode_flag(ndb, true);
    printf("db: snapshot receive mode (synchronous=NORMAL, WAL deferred, 512MB cache)\n");
    if (ok)
        *ok = true;
    return true;
}

static bool snapsync_exit_receive_mode_write(struct node_db *ndb, void *ctx)
{
    bool *ok = ctx;

    if (!ndb || !ndb->open) {
        if (ok)
            *ok = false;
        LOG_FAIL("snapshot_sync", "exit_receive_mode: ndb null or not open");
    }

    if (!node_db_exec(ndb, "PRAGMA synchronous=NORMAL") ||
        !node_db_exec(ndb, "PRAGMA cache_size=-65536") ||
        !node_db_exec(ndb, "PRAGMA wal_autocheckpoint=1000")) {
        if (ok)
            *ok = false;
        LOG_FAIL("snapshot_sync", "exit_receive_mode: PRAGMA restore failed");
    }
    if (!node_db_wal_checkpoint(ndb)) {
        if (ok)
            *ok = false;
        LOG_FAIL("snapshot_sync", "exit_receive_mode: WAL checkpoint failed");
    }
    snapsync_set_db_mode_flag(ndb, false);
    printf("db: snapshot receive mode cleared (synchronous=NORMAL, indexes preserved)\n");
    if (ok)
        *ok = true;
    return true;
}

static bool snapsync_exit_turbo_mode(struct snapshot_sync_service *svc)
{
    bool turbo_active = false;
    bool ok = false;

    if (!svc || !svc->ndb)
        LOG_FAIL("snapshot_sync", "exit_turbo_mode: svc or ndb is NULL");

    snapsync_service_lock();
    turbo_active = svc->turbo_active;
    snapsync_service_unlock();
    if (!turbo_active)
        return true;

    ok = snapsync_run_write(svc, snapsync_exit_receive_mode_write, &ok) && ok;

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
        return NULL;  /* normal: db_service may not be wired yet */
    return db_service_node_db(dbsvc) == svc->ndb ? dbsvc : NULL;
}

static bool snapsync_run_write(struct snapshot_sync_service *svc,
                               db_service_write_fn fn,
                               void *ctx)
{
    struct db_service *dbsvc = snapsync_db_service(svc);

    if (!svc || !svc->ndb || !fn)
        LOG_FAIL("snapshot_sync", "run_write: null svc, ndb, or fn pointer");
    if (dbsvc)
        return db_service_run_write(dbsvc, fn, ctx);
    return fn(svc->ndb, ctx);
}

static bool snapsync_begin_receive_write(struct node_db *ndb, void *ctx)
{
    struct snapshot_sync_service *svc = ctx;
    struct node_db_status status = {0};

    if (!svc || !ndb || !ndb->open)
        LOG_FAIL("snapshot_sync", "begin_receive_write: svc=%p ndb=%p open=%d",
                 (void*)svc, (void*)ndb, ndb ? ndb->open : 0);

    node_db_get_status(ndb, &status);
    if (status.tx_open) {
        if (!node_db_sync_flush(ndb)) {
            fprintf(stderr, "[snapsync] begin_receive: failed to flush stale transaction\n");
            return false;
        }
        node_db_get_status(ndb, &status);
        if (status.tx_open) {
            if (!node_db_commit(ndb)) {
                fprintf(stderr, "[snapsync] begin_receive: failed to close stale transaction\n");
                return false;
            }
        }
    }
    /* ── Scoped destructive transaction ───────────────────────────
     * This is THE call site from the 2026-04-10 incident. A bad boot
     * path offered a synthetic anchor, triggered snapsync_begin_receive,
     * and the raw primitive below wiped 1.3M UTXOs. Two things now
     * guard it: (1) recovery_policy caps the wipe size so an operator
     * must explicitly opt in, and (2) db_txn's RAII scope rolls back
     * automatically on any early return so a crash or unexpected
     * failure mid-sequence cannot leave a half-wiped UTXO table. */
    {
        DB_TXN_SCOPE(txn, ndb, "snapsync.begin_receive");
        if (!txn)
            LOG_FAIL("snapshot_sync", "begin_receive_write: failed to open db_txn scope");

        struct recovery_policy rp;
        policy_load_from_env(&rp);
        int64_t existing = node_db_utxo_count(ndb);
        if (existing < 0) existing = 0;
        enum policy_decision pd = policy_check_utxo_wipe(
            &rp, existing, "snapsync.begin_receive");
        if (pd != POLICY_ALLOW) {
            fprintf(stderr,
                    "[snapsync] begin_receive: recovery_policy refused wipe "
                    "(code=%s rows=%lld) — snapshot rejected\n",
                    policy_decision_name(pd), (long long)existing);
            return false;  /* scope auto-rollback */
        }

        if (!node_db_wipe_utxos(ndb))
            LOG_FAIL("snapshot_sync", "begin_receive_write: node_db_wipe_utxos failed");

        if (!db_txn_commit(txn))
            LOG_FAIL("snapshot_sync", "begin_receive_write: db_txn_commit failed after wipe");
    }

    /* Reopen a plain transaction for the chunk receive loop. The
     * chunk writer batches node_db_commit/node_db_begin pairs every
     * SNAPSYNC_BATCH_COMMIT_ROWS rows and relies on having an open
     * transaction at entry. */
    if (!node_db_begin(ndb))
        LOG_FAIL("snapshot_sync", "begin_receive_write: node_db_begin failed for chunk loop");

    svc->received_utxos = 0;
    svc->last_commit_at = 0;
    return true;
}

static bool snapsync_rollback_receive_write(struct node_db *ndb, void *ctx)
{
    struct node_db_status status = {0};

    (void)ctx;
    if (!ndb || !ndb->open)
        LOG_FAIL("snapshot_sync", "rollback_receive_write: ndb null or not open");
    node_db_get_status(ndb, &status);
    if (!status.tx_open)
        return true;
    return node_db_rollback(ndb);
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
        LOG_ERR("snapshot_sync", "apply_chunk: invalid args svc=%p chunk=%p len=%zu",
                (void*)svc, (void*)chunk_data, chunk_len);

    entries = chunk_data[0] | ((uint32_t)chunk_data[1] << 8) |
              ((uint32_t)chunk_data[2] << 16) | ((uint32_t)chunk_data[3] << 24);
    if (entries == 0 || entries > 1000)
        LOG_ERR("snapshot_sync", "apply_chunk: bad entry count %u", entries);

    for (uint32_t i = 0; i < entries; i++) {
        if (pos + 48 > chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: truncated entry %u at pos %zu (need 48, have %zu)", i, pos, chunk_len);

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

        if (pos >= chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: truncated at coinbase flag, entry %u pos %zu", i, pos);
        u.is_coinbase = (chunk_data[pos++] != 0);

        if (pos >= chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: truncated at script varint, entry %u pos %zu", i, pos);
        uint64_t slen = chunk_data[pos++];
        if (slen == 253) {
            if (pos + 2 > chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: truncated at 2-byte varint, entry %u pos %zu", i, pos);
            slen = chunk_data[pos] | (chunk_data[pos+1] << 8);
            pos += 2;
        } else if (slen == 254) {
            if (pos + 4 > chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: truncated at 4-byte varint, entry %u pos %zu", i, pos);
            slen = chunk_data[pos] | ((uint32_t)chunk_data[pos+1] << 8) |
                   ((uint32_t)chunk_data[pos+2] << 16) | ((uint32_t)chunk_data[pos+3] << 24);
            pos += 4;
        }
        if (pos + slen > chunk_len) LOG_ERR("snapshot_sync", "apply_chunk: script overflows chunk, entry %u pos %zu slen %llu", i, pos, (unsigned long long)slen);

        u.script = (uint8_t *)(chunk_data + pos);
        u.script_len = (size_t)slen;
        pos += slen;

        u.script_type = utxo_classify_script(u.script, u.script_len,
                                             u.address_hash, &u.has_address);

        if (!db_utxo_insert_raw(svc->ndb, &u))
            LOG_ERR("snapshot_sync", "apply_chunk: db_utxo_insert_raw failed at entry %u vout=%u", i, u.vout);
        applied++;
    }

    snapsync_service_lock();
    if (svc->state != SNAPSYNC_RECEIVING) {
        snapsync_service_unlock();
        return 0;
    }

    svc->received_utxos += (uint64_t)applied;
    svc->last_progress_time_us = now_us();
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
            LOG_ERR("snapshot_sync", "apply_chunk: batch commit/begin failed at %llu UTXOs", (unsigned long long)received);

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
        LOG_FAIL("snapshot_sync", "apply_chunk_write: null context apply=%p", (void*)apply);
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
    struct node_db_status db_status = {0};
    bool fc_verified;
    bool sha3_ok;
    double elapsed_s;

    if (!finalize || !finalize->svc || !ndb || !ndb->open)
        LOG_FAIL("snapshot_sync", "finalize_write: null args finalize=%p ndb=%p", (void*)finalize, (void*)ndb);
    svc = finalize->svc;

    node_db_get_status(ndb, &db_status);
    if (db_status.tx_open && !node_db_commit(ndb))
        LOG_FAIL("snapshot_sync", "finalize_write: failed to commit open transaction");

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

        /* ── Scoped commitment update ──────────────────────────
         * The coins_best_block / snapshot MMB / MMR height writes
         * must land as a single atomic unit: a partially-written
         * set of these three rows would point coins_best_block at
         * a height whose MMB root never landed, making the next
         * boot think UTXO state is authoritative without a valid
         * FlyClient anchor. DB_TXN_SCOPE auto-rolls-back any
         * partial writes on early return or db_txn_commit
         * failure. */
        {
            DB_TXN_SCOPE(txn, ndb, "snapsync.finalize");
            if (!txn)
                LOG_FAIL("snapshot_sync", "finalize_write: failed to open db_txn for commitment update");

            if (!node_db_state_set(ndb, "coins_best_block",
                                   svc->offered_block_hash, 32))
                LOG_FAIL("snapshot_sync", "finalize_write: failed to set coins_best_block");
            for (int i = 0; i < 32; i++) {
                if (svc->offered_mmb_root[i]) {
                    has_mmb = true;
                    break;
                }
            }
            if (has_mmb) {
                if (!node_db_state_set(ndb, "snapshot_mmb_root",
                                       svc->offered_mmb_root, 32) ||
                    !node_db_state_set(ndb, "snapshot_mmr_height",
                                       &svc->offered_height, 4))
                    LOG_FAIL("snapshot_sync", "finalize_write: failed to set snapshot_mmb_root/mmr_height");
            }

            if (!db_txn_commit(txn))
                LOG_FAIL("snapshot_sync", "finalize_write: db_txn_commit failed for commitment update");
        }
        if (!snapsync_exit_turbo_mode(svc))
            LOG_FAIL("snapshot_sync", "finalize_write: exit_turbo_mode failed after SHA3 pass");
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

        /* ── Recovery policy gate ──────────────────────────────────
         * The rows about to be wiped are the partial receive buffer
         * — data we just loaded from a peer that failed SHA3. Amount
         * is received_utxos (what this receive session wrote). If
         * the policy refuses we leave the bad data in place and log
         * loudly: the state machine is already SNAPSYNC_FAILED so
         * nothing will serve it, and an operator can raise the cap
         * to force cleanup. */
        int64_t partial = (int64_t)svc->received_utxos;
        struct recovery_policy rp;
        policy_load_from_env(&rp);
        enum policy_decision pd = policy_check_utxo_wipe(
            &rp, partial, "snapsync.finalize_sha3_fail");
        if (pd != POLICY_ALLOW) {
            fprintf(stderr,
                    "snapshot finalize: recovery_policy refused cleanup wipe "
                    "(code=%s rows=%lld) — corrupt partial state retained "
                    "until operator raises cap\n",
                    policy_decision_name(pd), (long long)partial);
        } else {
            /* Cleanup wipe runs inside its own scoped transaction:
             * if the DELETE cannot complete cleanly we'd rather
             * leave the partial UTXOs in place (SNAPSYNC_FAILED
             * already blocks them from being served) than hand
             * back control with a half-wiped table. */
            DB_TXN_SCOPE(txn, ndb, "snapsync.finalize_sha3_fail");
            if (!txn) {
                fprintf(stderr,
                        "snapshot finalize: failed to open db_txn for cleanup wipe\n");
            } else if (!node_db_wipe_utxos(ndb)) {
                fprintf(stderr,
                        "snapshot finalize: failed to wipe UTXOs after SHA3 mismatch\n");
                /* leave scope → auto-rollback */
            } else if (!db_txn_commit(txn)) {
                fprintf(stderr,
                        "snapshot finalize: commit of cleanup wipe failed\n");
            }
        }
        if (!snapsync_exit_turbo_mode(svc))
            fprintf(stderr,
                    "snapshot finalize: failed to restore normal mode after SHA3 mismatch\n");
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

/* ── Peer Blacklist ──────────────────────────────────────── */

bool snapsync_is_peer_blacklisted(const struct snapshot_sync_service *svc,
                                  uint32_t peer_id)
{
    if (!svc || peer_id == 0)
        return false;

    int64_t now = now_us();
    int64_t expiry_us = SNAPSYNC_BLACKLIST_SECS * 1000000LL;

    for (int i = 0; i < svc->blacklist_count; i++) {
        if (svc->blacklist[i].peer_id == peer_id &&
            (now - svc->blacklist[i].blacklisted_at_us) < expiry_us)
            return true;
    }
    return false;
}

void snapsync_blacklist_peer(struct snapshot_sync_service *svc,
                             uint32_t peer_id)
{
    if (!svc || peer_id == 0)
        return;

    int64_t now = now_us();
    int64_t expiry_us = SNAPSYNC_BLACKLIST_SECS * 1000000LL;

    /* Check if already blacklisted — refresh timestamp */
    for (int i = 0; i < svc->blacklist_count; i++) {
        if (svc->blacklist[i].peer_id == peer_id) {
            svc->blacklist[i].blacklisted_at_us = now;
            return;
        }
    }

    /* Evict expired entries first */
    for (int i = 0; i < svc->blacklist_count; ) {
        if ((now - svc->blacklist[i].blacklisted_at_us) >= expiry_us) {
            svc->blacklist[i] = svc->blacklist[--svc->blacklist_count];
        } else {
            i++;
        }
    }

    /* Add new entry */
    if (svc->blacklist_count < SNAPSYNC_MAX_BLACKLIST) {
        svc->blacklist[svc->blacklist_count].peer_id = peer_id;
        svc->blacklist[svc->blacklist_count].blacklisted_at_us = now;
        svc->blacklist_count++;
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
    bool rollback_ok = true;

    if (!svc) {
        return;
    }
    snapsync_service_lock();
    bool turbo_active = svc->turbo_active;
    snapsync_service_unlock();
    rollback_ok = snapsync_run_write(svc, snapsync_rollback_receive_write, NULL);
    if (!rollback_ok) {
        struct node_db_status status = {0};
        if (svc->ndb)
            node_db_get_status(svc->ndb, &status);
        if (status.tx_open) {
            snapsync_service_lock();
            snapsync_set_state(SNAPSYNC_FAILED, "receive rollback failed");
            snapsync_service_unlock();
        }
    }
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
    svc->last_progress_time_us = 0;
    svc->last_progress_utxos = 0;
    /* Clear snapshot anchor — its pprev=NULL blocks backward chain
     * walks in header_sync_service after snapshot completes. */
    if (g_snapshot_anchor) {
        free(g_snapshot_anchor);
        g_snapshot_anchor = NULL;
    }
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

    /* Skip P2P snapshot if we already have a real UTXO set.
     *
     * IMPORTANT: Do NOT skip just because coins_best_block is non-null.
     * A partial block connect from genesis (e.g. h=587) sets
     * coins_best_block but produces very few UTXOs — not a real set.
     * Only skip if the UTXO count indicates a genuine snapshot import
     * (100K+ UTXOs from file sync or a previous P2P snapshot). */
    if (svc->ndb && svc->ndb->open) {
        int64_t utxo_count = 0;
        sqlite3_stmt *sc = NULL;
        if (sqlite3_prepare_v2(svc->ndb->db,
                "SELECT COUNT(*) FROM utxos", -1, &sc, NULL) == SQLITE_OK
            && sc) {
            if (sqlite3_step(sc) == SQLITE_ROW)
                utxo_count = sqlite3_column_int64(sc, 0);
            sqlite3_finalize(sc);
        }
        if (utxo_count > 100000) {
            printf("[snapsync] Skipping P2P snapshot — %lld UTXOs already "
                   "imported\n", (long long)utxo_count);
            snapsync_service_unlock();
            return false;
        }
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
    struct trace_span *ss_span = trace_start("snapsync.begin_receive");

    if (!svc) {
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "null_svc");
        trace_end(ss_span);
        return false;
    }
    trace_attr_int(ss_span, "snap_height", svc->offered_height);

    snapsync_service_lock();
    if (svc->state != SNAPSYNC_NEGOTIATING) {
        fprintf(stderr, "[snapsync] begin_receive: wrong state %s\n",
                snapsync_state_name(svc->state));
        snapsync_service_unlock();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "wrong_state");
        trace_end(ss_span);
        return false;
    }
    if (!svc->ndb) {
        fprintf(stderr, "[snapsync] begin_receive: ndb is NULL\n");
        snapsync_service_unlock();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_end(ss_span);
        return false;
    }
    if (!svc->ndb->open) {
        fprintf(stderr, "[snapsync] begin_receive: ndb not open\n");
        snapsync_service_unlock();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_end(ss_span);
        return false;
    }

    /* Snapshot receive is a live-node path after startup. Keep schema stable
     * and only relax bulk-write pragmas here instead of dropping indexes. */
    bool receive_mode_ok = false;
    if (!snapsync_run_write(svc, snapsync_enter_receive_mode_write,
                            &receive_mode_ok) ||
        !receive_mode_ok) {
        snapsync_service_unlock();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "enter_receive_mode");
        trace_end(ss_span);
        return false;
    }
    svc->turbo_active = true;
    snapsync_service_unlock();

    if (!snapsync_run_write(svc, snapsync_begin_receive_write, svc)) {
        snapsync_exit_turbo_mode(svc);
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "begin_receive_write");
        trace_end(ss_span);
        return false;
    }
    if (!sync_set_state(SYNC_SNAPSHOT_RECEIVE, "snapshot receive started")) {
        snapsync_run_write(svc, snapsync_rollback_receive_write, NULL);
        snapsync_reset(svc);
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "sync_set_state");
        trace_end(ss_span);
        return false;
    }

    snapsync_service_lock();
    svc->state = SNAPSYNC_RECEIVING;
    svc->last_progress_time_us = now_us();  /* start stall timer */
    svc->last_progress_utxos = 0;
    snapsync_set_state(SNAPSYNC_RECEIVING, "receive mode active");
    snapsync_service_unlock();
    trace_end(ss_span);
    return true;
}

/* ── Apply Chunk ─────────────────────────────────────────── */

int snapsync_apply_chunk(struct snapshot_sync_service *svc,
                         const uint8_t *chunk_data, size_t chunk_len)
{
    struct snapsync_apply_chunk_ctx ctx;
    bool restore_turbo = false;
    if (!svc || !chunk_data || chunk_len < 4)
        LOG_ERR("snapshot_sync", "apply_chunk: invalid args svc=%p chunk=%p len=%zu",
                (void*)svc, (void*)chunk_data, chunk_len);

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
        LOG_ERR("snapshot_sync", "apply_chunk: ndb null or not open during RECEIVING");
    }
    restore_turbo = svc->turbo_active;

    memset(&ctx, 0, sizeof(ctx));
    ctx.svc = svc;
    ctx.chunk_data = zcl_malloc(chunk_len, "snapsync chunk copy");
    if (!ctx.chunk_data) {
        snapsync_service_unlock();
        LOG_ERR("snapshot_sync", "apply_chunk: malloc(%zu) failed for chunk copy", chunk_len);
    }
    memcpy(ctx.chunk_data, chunk_data, chunk_len);
    ctx.chunk_len = chunk_len;
    snapsync_service_unlock();

    if (!snapsync_run_write(svc, snapsync_apply_chunk_write, &ctx)) {
        free(ctx.chunk_data);
        if (restore_turbo)
            snapsync_run_write(svc, snapsync_rollback_receive_write, NULL);
        snapsync_service_lock();
        svc->state = SNAPSYNC_FAILED;
        snapsync_set_state(SNAPSYNC_FAILED, "snapshot chunk apply failed");
        snapsync_service_unlock();
        if (restore_turbo && !snapsync_exit_turbo_mode(svc))
            fprintf(stderr, "snapshot apply: failed to restore normal mode\n");
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
    bool keep_failed_state = false;

    if (!svc)
        LOG_FAIL("snapshot_sync", "finalize: svc is NULL");
    snapsync_service_lock();
    finalize_allowed = (svc->state == SNAPSYNC_RECEIVING &&
                       svc->ndb && svc->ndb->open);
    if (finalize_allowed)
        turbo_active = svc->turbo_active;
    snapsync_service_unlock();

    if (!finalize_allowed) {
        LOG_FAIL("snapshot_sync", "finalize: not allowed (state != RECEIVING or ndb not open)");
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.svc = svc;

    if (!snapsync_run_write(svc, snapsync_finalize_write, &ctx)) {
        snapsync_service_lock();
        keep_failed_state = (svc->state == SNAPSYNC_FAILED);
        if (!keep_failed_state)
            svc->state = SNAPSYNC_FAILED;
        snapsync_service_unlock();

        if (!keep_failed_state) {
            snapsync_set_state(SNAPSYNC_FAILED, "finalize write path failed");
            if (!snapsync_exit_turbo_mode(svc))
                fprintf(stderr, "snapshot finalize: failed to restore normal mode\n");
        } else if (turbo_active) {
            if (!snapsync_exit_turbo_mode(svc))
                fprintf(stderr, "snapshot finalize: failed to restore normal mode\n");
        }
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

bool snapsync_awaiting_utxos(void)
{
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();
    if (!svc) {
        if (!snapsync_global_initialized()) {
            /* Service not yet initialized.  Check UTXO count directly
             * via the global node_db.  If UTXOs are low, we're likely
             * awaiting a P2P snapshot — do NOT connect blocks. */
            struct node_db *ndb = app_runtime_node_db();
            if (ndb && ndb->open) {
                int64_t utxo_count = 0;
                sqlite3_stmt *sc = NULL;
                if (sqlite3_prepare_v2(ndb->db,
                        "SELECT COUNT(*) FROM utxos", -1, &sc, NULL)
                    == SQLITE_OK && sc) {
                    if (sqlite3_step(sc) == SQLITE_ROW)
                        utxo_count = sqlite3_column_int64(sc, 0);
                    sqlite3_finalize(sc);
                }
                if (utxo_count < 100000)
                    return true;
            }
            return false;
        }
        svc = snapsync_global();
    }
    if (!svc || !svc->ndb || !svc->ndb->open)
        return false;

    /* If snapshot sync already completed or failed with recovery, not waiting */
    struct snapsync_status st;
    snapsync_get_status_snapshot(svc, &st);
    if (st.state == SNAPSYNC_COMPLETE)
        return false;

    /* Check coins_best_block — if set to a meaningful height, UTXOs exist */
    uint8_t cb_buf[32] = {0};
    size_t cb_len = 0;
    if (node_db_state_get(svc->ndb, "coins_best_block",
                          cb_buf, sizeof(cb_buf), &cb_len) && cb_len == 32) {
        bool all_zero = true;
        for (int i = 0; i < 32; i++)
            if (cb_buf[i]) { all_zero = false; break; }
        if (!all_zero) {
            /* coins_best_block is set.  Check that it points to a block
             * at a meaningful height (not just h=587 from a failed partial
             * connect).  If the height is below the snapshot offer height
             * and below a safety threshold, we're still waiting. */
            int64_t utxo_count = 0;
            sqlite3_stmt *sc = NULL;
            if (sqlite3_prepare_v2(svc->ndb->db,
                    "SELECT COUNT(*) FROM utxos", -1, &sc, NULL) == SQLITE_OK
                && sc) {
                if (sqlite3_step(sc) == SQLITE_ROW)
                    utxo_count = sqlite3_column_int64(sc, 0);
                sqlite3_finalize(sc);
            }
            /* A real snapshot import produces 1M+ UTXOs.  A partial
             * block connect from genesis produces very few. */
            if (utxo_count > 100000)
                return false;  /* real UTXO set exists */
        }
    }

    /* If we get here, UTXO count is low (<100K) — check if snapshot
     * sync is still a possibility (not yet completed or permanently failed
     * with no hope of recovery). */
    if (st.state != SNAPSYNC_FAILED)
        return true;  /* still waiting for snapshot */

    return false;
}

bool snapsync_check_stall(void)
{
    struct snapshot_sync_service *svc = app_runtime_snapshot_sync();
    if (!svc) {
        if (!snapsync_global_initialized())
            return false;
        svc = snapsync_global();
    }

    snapsync_service_lock();
    if (svc->state != SNAPSYNC_RECEIVING || svc->last_progress_time_us == 0) {
        snapsync_service_unlock();
        return false;
    }

    uint64_t received = svc->received_utxos;
    uint64_t offered = svc->offered_count;
    uint32_t peer_id = svc->serving_peer_id;

    /* If progress has been made since last check, update timer */
    if (received > svc->last_progress_utxos) {
        svc->last_progress_time_us = now_us();
        svc->last_progress_utxos = received;
        snapsync_service_unlock();
        return false;
    }

    int64_t elapsed_us = now_us() - svc->last_progress_time_us;
    int64_t timeout_us = SNAPSYNC_STALL_TIMEOUT_SECS * 1000000LL;
    if (elapsed_us < timeout_us) {
        snapsync_service_unlock();
        return false;
    }

    snapsync_service_unlock();

    fprintf(stderr, "[snapsync] STALL DETECTED: no chunk for %llds "
            "(%llu/%llu UTXOs from peer %u) — blacklisting peer, resetting\n",
            (long long)(elapsed_us / 1000000),
            (unsigned long long)received,
            (unsigned long long)offered, peer_id);

    /* Blacklist the stalling peer before reset (reset clears serving_peer_id) */
    snapsync_blacklist_peer(svc, peer_id);

    snapsync_reset(svc);

    /* Wipe partial UTXOs so the next offer isn't rejected.
     * Committed batches (every 100K) survive the rollback in reset.
     *
     * Same gating as the SHA3-fail path — the rows here are the
     * partial receive buffer from the stalling peer. Amount is
     * received_utxos (at the time of stall). If the policy refuses,
     * the partial data stays and the operator can raise the cap. */
    if (svc->ndb && svc->ndb->open) {
        int64_t partial = (int64_t)received;
        struct recovery_policy rp;
        policy_load_from_env(&rp);
        enum policy_decision pd = policy_check_utxo_wipe(
            &rp, partial, "snapsync.stall_cleanup");
        if (pd != POLICY_ALLOW) {
            fprintf(stderr,
                    "[snapsync] stall cleanup: recovery_policy refused wipe "
                    "(code=%s rows=%lld) — partial data retained\n",
                    policy_decision_name(pd), (long long)partial);
        } else {
            node_db_wipe_utxos(svc->ndb);
            printf("[snapsync] Wiped partial UTXOs after stall reset\n");
        }
    }

    /* Reset sync state so the node can accept a new snapshot offer.
     * SYNC_SNAPSHOT_RECEIVE → SYNC_HEADERS_DOWNLOAD allows re-entry
     * into the snapshot path when the next peer offers. */
    if (sync_get_state() == SYNC_SNAPSHOT_RECEIVE)
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "snapshot stall reset");

    return true;
}

struct block_index *snapsync_get_anchor(void) { return g_snapshot_anchor; }
void snapsync_set_anchor(struct block_index *anchor) { g_snapshot_anchor = anchor; }

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
        LOG_FAIL("snapshot_sync", "build_request_pow: peer_ip=%p pow=%p", (void*)peer_ip, (void*)pow);

    memset(pow, 0, sizeof(*pow));
    sha3_256(peer_ip, 16, peer_id);
    return fast_sync_solve_pow(peer_id, pow);
}

bool snapsync_parse_offer_params(struct snapshot_offer_params *params,
                                 struct byte_stream *s)
{
    if (!params || !s)
        LOG_FAIL("snapshot_sync", "parse_offer_params: params=%p stream=%p", (void*)params, (void*)s);

    memset(params, 0, sizeof(*params));
    if (!stream_read_i32_le(s, &params->height) ||
        !stream_read_bytes(s, params->block_hash, 32) ||
        !stream_read_bytes(s, params->utxo_root, 32) ||
        !stream_read_bytes(s, params->mmr_root, 32) ||
        !stream_read_u64_le(s, &params->num_utxos) ||
        !stream_read_u64_le(s, &params->total_bytes)) {
        LOG_FAIL("snapshot_sync", "parse_offer_params: truncated stream at pos %zu/%zu",
                 s->read_pos, s->size);
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
        LOG_FAIL("snapshot_sync", "parse_fc_response: resp=%p stream=%p", (void*)resp, (void*)s);

    memset(resp, 0, sizeof(*resp));
    if (!stream_read_u32_le(s, &num_samples) ||
        num_samples == 0 || num_samples > FC_MAX_SAMPLES) {
        LOG_FAIL("snapshot_sync", "parse_fc_response: bad num_samples=%u (max=%d)",
                 num_samples, FC_MAX_SAMPLES);
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
            LOG_FAIL("snapshot_sync", "parse_fc_response: truncated sample %u at pos %zu/%zu",
                     i, s->read_pos, s->size);
        }

        for (uint32_t j = 0; j < sample->proof.num_siblings; j++) {
            if (!stream_read_bytes(s, sample->proof.siblings[j], 32))
                LOG_FAIL("snapshot_sync", "parse_fc_response: truncated sibling %u/%u in sample %u",
                         j, sample->proof.num_siblings, i);
        }

        if (!stream_read_u32_le(s, &sample->proof.num_peaks) ||
            sample->proof.num_peaks > MMB_MAX_MOUNTAINS) {
            LOG_FAIL("snapshot_sync", "parse_fc_response: bad num_peaks=%u in sample %u",
                     sample->proof.num_peaks, i);
        }

        for (uint32_t j = 0; j < sample->proof.num_peaks; j++) {
            if (!stream_read_bytes(s, sample->proof.peaks[j], 32))
                LOG_FAIL("snapshot_sync", "parse_fc_response: truncated peak %u/%u in sample %u",
                         j, sample->proof.num_peaks, i);
        }

        if (!stream_read_u64_le(s, &sample->proof.mmb_size))
            LOG_FAIL("snapshot_sync", "parse_fc_response: truncated mmb_size in sample %u", i);
    }

    return true;
}

bool snapsync_write_fc_challenge(const struct snapshot_sync_service *svc,
                                 struct byte_stream *s)
{
    if (!svc || !s)
        LOG_FAIL("snapshot_sync", "write_fc_challenge: svc=%p stream=%p", (void*)svc, (void*)s);

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
        LOG_FAIL("snapshot_sync", "write_snapshot_request: s=%p peer_ip=%p", (void*)s, (void*)peer_ip);
    if (!snapsync_build_request_pow(peer_ip, &pow))
        LOG_FAIL("snapshot_sync", "write_snapshot_request: build_request_pow failed");

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
        LOG_FAIL("snapshot_sync", "build_fc_response: invalid args resp=%p challenge=%p chain=%p leaves=%llu",
                 (void*)resp, (void*)challenge, (void*)chain_active,
                 leaf_store ? (unsigned long long)leaf_store->num_leaves : 0ULL);
    }

    all_hashes = mmb_leaf_store_all(leaf_store);
    if (!all_hashes)
        LOG_FAIL("snapshot_sync", "build_fc_response: mmb_leaf_store_all returned NULL");

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
            LOG_FAIL("snapshot_sync", "build_fc_response: no block_index at height %d for sample %u", h, i);

        mmb_leaf_from_block(&sample->leaf,
                            bi->phashBlock->data,
                            bi->nHeight, bi->nTime, bi->nBits,
                            bi->hashFinalSaplingRoot.data,
                            (const uint8_t *)bi->nChainWork.pn);

        if (prove_len > leaf_store->num_leaves)
            prove_len = leaf_store->num_leaves;
        if (!mmb_prove(all_hashes, prove_len, (uint64_t)h, &sample->proof))
            LOG_FAIL("snapshot_sync", "build_fc_response: mmb_prove failed for height %d sample %u", h, i);
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!mmb_verify(&resp->samples[i].proof, challenge->mmb_root))
            LOG_FAIL("snapshot_sync", "build_fc_response: mmb_verify failed for sample %u", i);
    }

    return true;
}

bool snapsync_write_fc_response(struct byte_stream *s,
                                const struct fc_response *resp)
{
    if (!s || !resp || resp->num_samples == 0 ||
        resp->num_samples > FC_MAX_SAMPLES) {
        LOG_FAIL("snapshot_sync", "write_fc_response: invalid args s=%p resp=%p samples=%u",
                 (void*)s, (void*)resp, resp ? resp->num_samples : 0);
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

/* Single-writer tip commit for snapshot activation. Routes through
 * the chain_state_repository so block_map, active_chain, coins_tip,
 * and pindex_best_header move together — the exact failure mode that
 * caused the 2026-04-10 UTXO wipe was a snapshot path updating these
 * out of order. Falls back to raw setters when the csr singleton was
 * never wired (unit tests that call this function without boot). */
static bool snapsync_commit_tip(struct main_state *ms,
                                 struct block_index *new_tip,
                                 const char *reason)
{
    if (!new_tip || !new_tip->phashBlock) LOG_FAIL("snapshot_sync", "commit_tip: new_tip=%p phashBlock=%p", (void*)new_tip, new_tip ? (void*)new_tip->phashBlock : NULL);

    struct chain_state_commit commit = {
        .new_tip             = new_tip,
        .new_coins_best      = *new_tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip   = true,
        /* Snapshot sync legitimately installs a tip at a different
         * height (a synthetic anchor on the fast-sync path). Bypass
         * the orphan-rows guard; Phase 2 recovery_policy will gate
         * this kind of move explicitly. */
        .allow_rollback      = true,
        .wallet_scan_height  = -1,
        .reason              = reason,
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK) return true;

    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        /* Test harness path: singleton was never wired. Fall back to
         * the raw setters so existing unit tests still exercise the
         * snapshot activation logic end-to-end. */
        active_chain_set_tip(&ms->chain_active, new_tip);
        ms->pindex_best_header = new_tip;
        return true;
    }

    fprintf(stderr,
            "snapsync: csr rejected tip commit (%s) reason=%s h=%d\n",
            csr_result_name(rc), reason, new_tip->nHeight);
    return false;
}

int snapsync_activate_verified_tip(const struct snapshot_sync_service *svc,
                                   struct main_state *ms)
{
    struct uint256 snap_hash;
    struct block_index *snap_bi;

    if (!svc || !ms)
        LOG_ERR("snapshot_sync", "activate_verified_tip: svc=%p ms=%p", (void*)svc, (void*)ms);

    memcpy(snap_hash.data, svc->offered_block_hash, 32);
    snap_bi = block_map_find(&ms->map_block_index, &snap_hash);
    if (!snap_bi) {
        /* Snapshot block hash not in local block index — expected for fresh
         * nodes that received a UTXO snapshot via fast sync. FlyClient has
         * verified the chain of work (≥150-bit security) and SHA3 has verified
         * the UTXO set integrity, so we trust this block hash.
         *
         * Insert a placeholder block_index at the snapshot height. This
         * serves as the anchor for getheaders locator — header sync will
         * resume from this height instead of from the local chain tip.
         *
         * We do NOT set this as the active chain tip because active_chain
         * requires a full pprev chain. Instead, we store it as a snapshot
         * anchor that push_getheaders uses as its locator starting point. */
        snap_bi = zcl_calloc(1, sizeof(struct block_index), "snapsync anchor block_index");
        if (!snap_bi)
            LOG_ERR("snapshot_sync", "activate_verified_tip: calloc block_index failed (%zu bytes)", sizeof(struct block_index));
        block_index_init(snap_bi);
        snap_bi->nHeight = svc->offered_height;
        snap_bi->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;

        /* Set nChainTx non-zero so find_most_work_chain considers blocks
         * building on this anchor. The UTXO set at this height is
         * cryptographically verified — we don't need actual block data. */
        snap_bi->nChainTx = 1;

        /* Set chain work higher than any entry in the block index.
         * The file-synced block_index.bin has real chain_work values —
         * if our fake chain_work is too low, find_most_work_chain picks
         * a locally-indexed block instead of the snapshot chain. */
        {
            struct arith_uint256 max_work;
            arith_uint256_set_u64(&max_work, 0);
            size_t iter = 0;
            struct block_index *bi;
            while (block_map_next(&ms->map_block_index, &iter, NULL, &bi)) {
                if (bi && arith_uint256_compare(&bi->nChainWork, &max_work) > 0)
                    max_work = bi->nChainWork;
            }
            /* Snapshot chain_work = max existing + generous margin */
            struct arith_uint256 margin;
            arith_uint256_set_u64(&margin, (uint64_t)svc->offered_height * 4096ULL);
            arith_uint256_add(&snap_bi->nChainWork, &max_work, &margin);
        }

        block_map_insert(&ms->map_block_index, &snap_hash, snap_bi);
        snap_bi->phashBlock = block_map_find_hash(&ms->map_block_index,
                                                   &snap_hash);
        g_snapshot_anchor = snap_bi;

        /* Set assume-valid to snapshot height — all blocks at or below
         * this height skip expensive script/proof verification since the
         * UTXO set at this point is cryptographically verified. */
        g_assume_valid_height = svc->offered_height;

        /* Set the active chain tip to the anchor. This allows
         * activate_best_chain to find a fork point and connect delta
         * blocks from snapshot+1 to tip. The pprev=NULL means the
         * chain array below anchor height will be NULL, but we only
         * need to connect blocks ABOVE the anchor. */
        snapsync_commit_tip(ms, snap_bi,
                            "snapshot.apply_synthetic_anchor");

        printf("[snapshot] Anchor at height %d set as chain tip "
               "(FlyClient+SHA3 verified)\n", svc->offered_height);
        return svc->offered_height;
    }

    snapsync_commit_tip(ms, snap_bi, "snapshot.apply_anchor");
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
    result->should_set_sync_state = false;
    result->sync_state = SYNC_IDLE;
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
        LOG_FAIL("snapshot_sync", "prepare_serve_step: invalid args step=%p node=%p buf=%p size=%lld",
                 (void*)step, (void*)node, (void*)buf, (long long)buf_size);

    memset(step, 0, sizeof(*step));
    if (node->zsync_file_size == 0)
        node->zsync_file_size = buf_size;
    /* Allow up to 8MB of send buffer during snapshot serving.
     * The previous 2MB limit caused stalls: the receiver's SQLite writes
     * slow TCP drainage, the 2MB fills in ~50 chunks, and the sender's
     * message loop moves on to other peers before returning to pump more.
     * 8MB gives ~200 chunks of headroom. */
    if (node->send_size > 8 * 1024 * 1024)
        return true;

    pos = node->zsync_file_offset;
    if (pos >= buf_size) {
        step->action = SNAPSYNC_SERVE_ACTION_SEND_END;
        return true;
    }

    if (pos + 4 > buf_size)
        LOG_FAIL("snapshot_sync", "prepare_serve_step: pos %lld + 4 > buf_size %lld", (long long)pos, (long long)buf_size);

    entries = buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
              ((uint32_t)buf[pos + 2] << 16) |
              ((uint32_t)buf[pos + 3] << 24);
    if (entries == 0 || entries > 1000)
        LOG_FAIL("snapshot_sync", "prepare_serve_step: bad entry count %u at pos %lld", entries, (long long)pos);

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
        LOG_FAIL("snapshot_sync", "prepare_serve_step: scan overflow scan=%lld buf_size=%lld entries=%u",
                 (long long)scan, (long long)buf_size, entries);

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

    /* Reject peers that previously stalled during snapshot transfer */
    if (snapsync_is_peer_blacklisted(svc, params->peer_id))
        return SNAPSYNC_OFFER_REJECTED_BLACKLISTED;

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
        LOG_FAIL("snapshot_sync", "verify_flyclient: svc=%p resp=%p", (void*)svc, (void*)resp);

    snapsync_service_lock();
    state = svc->state;
    serving_peer_id = svc->serving_peer_id;
    memcpy(&challenge, &svc->fc_challenge, sizeof(challenge));
    snapsync_service_unlock();
    if (state != SNAPSYNC_NEGOTIATING) {
        printf("[snapsync] FlyClient: wrong state %s\n",
               snapsync_state_name(state));
        return false;  /* already logged */
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
        LOG_FAIL("snapshot_sync", "verify_flyclient: begin_receive failed after FlyClient pass");

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

    if (!svc) LOG_FAIL("snapshot_sync", "handle_end: svc is NULL");
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
