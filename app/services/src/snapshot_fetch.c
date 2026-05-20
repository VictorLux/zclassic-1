/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * snapshot_fetch.c — Phase 2: chunk receive.
 *
 * Owns the receive-mode database PRAGMA tuning ("turbo mode"),
 * the staging table maintenance, the chunk wire-format parser,
 * and the chunk-application write path. Also handles the trigger
 * point for handing off to verify (snapsync_handle_end).
 *
 * Pure code-motion split from snapshot_sync_service.c. */

#include "services/snapshot_sync_service.h"
#include "models/database.h"
#include "models/db_txn.h"
#include "models/utxo.h"
#include "coins/utxo_commitment.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/trace.h"
#include "util/ar_step_readonly.h"

#include "snapshot_sync_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Staging helpers ─────────────────────────────────────── */

bool snapsync_discard_staging_internal(struct node_db *ndb, const char *reason)
{
    if (!ndb || !ndb->open)
        return false;
    bool ok = node_db_exec(ndb, "DELETE FROM " SNAPSYNC_STAGING_TABLE);
    ok &= node_db_exec(ndb,
                       "DELETE FROM node_state WHERE key LIKE 'snapshot_staging_%'");
    if (reason && *reason) {
        ok &= node_db_state_set(ndb, SNAPSYNC_STAGING_LAST_DISCARD_KEY,
                                reason, strlen(reason));
    }
    return ok;
}

bool snapsync_discard_staging_txn_internal(struct node_db *ndb,
                                           const char *label,
                                           const char *reason)
{
    if (!ndb || !ndb->open)
        return false;
    DB_TXN_SCOPE(txn, ndb, label ? label : "snapsync.discard_staging");
    if (!txn)
        return false;
    if (!snapsync_discard_staging_internal(ndb, reason))
        return false;
    return db_txn_commit(txn);
}

bool snapsync_set_staging_phase_internal(struct node_db *ndb, const char *phase)
{
    if (!ndb || !ndb->open || !phase || !*phase)
        return false;
    return node_db_state_set(ndb, SNAPSYNC_STAGING_PHASE_KEY,
                             phase, strlen(phase));
}

bool snapsync_discard_staging_write_internal(struct node_db *ndb, void *ctx)
{
    return snapsync_discard_staging_internal(ndb, (const char *)ctx);
}

static bool snapsync_insert_staging_raw(struct node_db *ndb,
                                        const struct db_utxo *u)
{
    sqlite3_stmt *s;

    if (!ndb || !ndb->stmt_snapshot_staging_insert || !u)
        return false;
    s = ndb->stmt_snapshot_staging_insert;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_blob(s, 1, u->txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, (int)u->vout);
    sqlite3_bind_int64(s, 3, u->value);
    sqlite3_bind_blob(s, 4, u->script, (int)u->script_len, SQLITE_STATIC);
    sqlite3_bind_int(s, 5, (int)u->script_type);
    if (u->has_address)
        sqlite3_bind_blob(s, 6, u->address_hash, 20, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 6);
    sqlite3_bind_int(s, 7, u->height);
    sqlite3_bind_int(s, 8, u->is_coinbase ? 1 : 0);
    return sqlite3_step(s) == SQLITE_DONE;  // raw-sql-ok:state-kv-write-caller-handles-rc
}

int64_t snapsync_staging_count_internal(struct node_db *ndb)
{
    sqlite3_stmt *st = NULL;
    int64_t result = -1;

    if (!ndb || !ndb->open || !ndb->db)
        LOG_ERR("snapshot_sync", "staging_count: ndb=%p open=%d db=%p",
                (void*)ndb, ndb ? ndb->open : 0,
                ndb ? (void*)ndb->db : NULL);
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(*) FROM " SNAPSYNC_STAGING_TABLE,
            -1, &st, NULL) != SQLITE_OK)
        LOG_ERR("snapshot_sync", "staging_count: prepare failed: %s",
                sqlite3_errmsg(ndb->db));
    if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
        result = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return result;
}

void snapsync_hash_staging_internal(struct node_db *ndb, uint8_t out[32],
                                    uint64_t *utxo_count)
{
    utxo_commitment_sha3_compute_table(ndb ? ndb->db : NULL,
                                       SNAPSYNC_STAGING_TABLE,
                                       out, utxo_count);
}

/* ── Receive-mode DB tuning ──────────────────────────────── */

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

bool snapsync_exit_turbo_mode_internal(struct snapshot_sync_service *svc)
{
    bool turbo_active = false;
    bool ok = false;

    if (!svc || !svc->ndb)
        LOG_FAIL("snapshot_sync", "exit_turbo_mode: svc or ndb is NULL");

    snapsync_service_lock_internal();
    turbo_active = svc->turbo_active;
    snapsync_service_unlock_internal();
    if (!turbo_active)
        return true;

    ok = snapsync_run_write_internal(svc, snapsync_exit_receive_mode_write, &ok) && ok;

    snapsync_service_lock_internal();
    svc->turbo_active = false;
    snapsync_service_unlock_internal();

    return ok;
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
        if (!node_db_sync_flush(ndb))
            LOG_FAIL("snapshot_sync", "begin_receive: failed to flush stale transaction");
        node_db_get_status(ndb, &status);
        if (status.tx_open) {
            if (!node_db_commit(ndb))
                LOG_FAIL("snapshot_sync", "begin_receive: failed to close stale transaction");
        }
    }
    /* Reset the isolated staging namespace only. Active utxos remain
     * untouched until snapshot_verify passes and atomic_activate promotes
     * the staged rows in one transaction. */
    {
        DB_TXN_SCOPE(txn, ndb, "snapsync.begin_receive");
        if (!txn)
            LOG_FAIL("snapshot_sync", "begin_receive_write: failed to open db_txn scope");

        if (!snapsync_discard_staging_internal(ndb, "begin_receive"))
            LOG_FAIL("snapshot_sync", "begin_receive_write: staging cleanup failed");
        if (!snapsync_set_staging_phase_internal(ndb, SNAPSYNC_PHASE_CHUNK_RECEIVE))
            LOG_FAIL("snapshot_sync", "begin_receive_write: failed to set staging phase");

        if (!db_txn_commit(txn))
            LOG_FAIL("snapshot_sync", "begin_receive_write: db_txn_commit failed after staging reset");
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

bool snapsync_rollback_receive_write_internal(struct node_db *ndb, void *ctx)
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

        if (!snapsync_insert_staging_raw(svc->ndb, &u))
            LOG_ERR("snapshot_sync", "apply_chunk: staging insert failed at entry %u vout=%u", i, u.vout);
        applied++;
    }

    snapsync_service_lock_internal();
    if (svc->state != SNAPSYNC_RECEIVING) {
        snapsync_service_unlock_internal();
        return 0;
    }

    svc->received_utxos += (uint64_t)applied;
    svc->last_progress_time_us = snapsync_now_us_internal();
    received = svc->received_utxos;
    offered_count = svc->offered_count;
    serving_peer_id = svc->serving_peer_id;
    if (received - svc->last_commit_at >= SNAPSYNC_BATCH_COMMIT_ROWS) {
        commit_batch = true;
        svc->last_commit_at = received;
    }
    snapsync_service_unlock_internal();

    if (commit_batch) {
        if (!node_db_commit(svc->ndb) || !node_db_begin(svc->ndb))
            LOG_ERR("snapshot_sync", "apply_chunk: batch commit/begin failed at %llu UTXOs", (unsigned long long)received);

        double elapsed_s = (double)(snapsync_now_us_internal() - svc->start_time_us) / 1000000.0;
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

    snapsync_service_lock_internal();
    if (svc->state != SNAPSYNC_NEGOTIATING) {
        event_emitf(EV_SNAPSYNC_VERIFIED, svc->serving_peer_id,
                    "snapshot=FAILED reason=begin_receive_wrong_state state=%s",
                    snapsync_state_name(svc->state));
        snapsync_service_unlock_internal();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "wrong_state");
        trace_end(ss_span);
        return false;
    }
    if (!svc->ndb) {
        event_emitf(EV_SNAPSYNC_VERIFIED, svc->serving_peer_id,
                    "snapshot=FAILED reason=begin_receive_no_db");
        snapsync_service_unlock_internal();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_end(ss_span);
        return false;
    }
    if (!svc->ndb->open) {
        event_emitf(EV_SNAPSYNC_VERIFIED, svc->serving_peer_id,
                    "snapshot=FAILED reason=begin_receive_db_closed");
        snapsync_service_unlock_internal();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_end(ss_span);
        return false;
    }

    /* Snapshot receive is a live-node path after startup. Keep schema stable
     * and only relax bulk-write pragmas here instead of dropping indexes. */
    bool receive_mode_ok = false;
    if (!snapsync_run_write_internal(svc, snapsync_enter_receive_mode_write,
                                     &receive_mode_ok) ||
        !receive_mode_ok) {
        snapsync_service_unlock_internal();
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "enter_receive_mode");
        trace_end(ss_span);
        return false;
    }
    svc->turbo_active = true;
    snapsync_service_unlock_internal();

    if (!snapsync_run_write_internal(svc, snapsync_begin_receive_write, svc)) {
        snapsync_exit_turbo_mode_internal(svc);
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "begin_receive_write");
        trace_end(ss_span);
        return false;
    }
    if (!sync_set_state(SYNC_SNAPSHOT_RECEIVE, "snapshot receive started")) {
        snapsync_run_write_internal(svc, snapsync_rollback_receive_write_internal, NULL);
        snapsync_reset(svc);
        trace_set_status(ss_span, TRACE_STATUS_ERROR);
        trace_attr_str(ss_span, "error", "sync_set_state");
        trace_end(ss_span);
        return false;
    }

    snapsync_service_lock_internal();
    svc->state = SNAPSYNC_RECEIVING;
    svc->last_progress_time_us = snapsync_now_us_internal();  /* start stall timer */
    svc->last_progress_utxos = 0;
    snapsync_set_state(SNAPSYNC_RECEIVING, "receive mode active");
    snapsync_service_unlock_internal();
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

    snapsync_service_lock_internal();

    /* Only accept chunks in RECEIVING state.
     * NEGOTIATING means FlyClient verification hasn't completed yet —
     * do NOT auto-transition, that would bypass chain verification. */
    if (svc->state != SNAPSYNC_RECEIVING) {
        snapsync_service_unlock_internal();
        return 0;
    }
    if (!svc->ndb || !svc->ndb->open) {
        snapsync_service_unlock_internal();
        LOG_ERR("snapshot_sync", "apply_chunk: ndb null or not open during RECEIVING");
    }
    restore_turbo = svc->turbo_active;

    memset(&ctx, 0, sizeof(ctx));
    ctx.svc = svc;
    ctx.chunk_data = zcl_malloc(chunk_len, "snapsync chunk copy");
    if (!ctx.chunk_data) {
        snapsync_service_unlock_internal();
        LOG_ERR("snapshot_sync", "apply_chunk: malloc(%zu) failed for chunk copy", chunk_len);
    }
    memcpy(ctx.chunk_data, chunk_data, chunk_len);
    ctx.chunk_len = chunk_len;
    snapsync_service_unlock_internal();

    if (!snapsync_run_write_internal(svc, snapsync_apply_chunk_write, &ctx)) {
        free(ctx.chunk_data);
        if (restore_turbo)
            snapsync_run_write_internal(svc, snapsync_rollback_receive_write_internal, NULL);
        snapsync_service_lock_internal();
        svc->state = SNAPSYNC_FAILED;
        snapsync_set_state(SNAPSYNC_FAILED, "snapshot chunk apply failed");
        snapsync_service_unlock_internal();
        if (restore_turbo && !snapsync_exit_turbo_mode_internal(svc))
            event_emitf(EV_SNAPSYNC_VERIFIED, 0,
                        "snapshot=FAILED reason=turbo_exit_failed path=chunk_apply");
        LOG_ERR("snapsync", "chunk apply failed");
    }
    free(ctx.chunk_data);
    return ctx.applied;
}

/* Action: handle snapshot end from peer.
 * Validates peer identity and state, finalizes. */
bool snapsync_handle_end(struct snapshot_sync_service *svc, uint32_t peer_id)
{
    enum snapshot_sync_state state = SNAPSYNC_IDLE;
    uint32_t serving_peer_id = 0;
    uint64_t received = 0;

    if (!svc) LOG_FAIL("snapshot_sync", "handle_end: svc is NULL");
    snapsync_service_lock_internal();
    state = svc->state;
    serving_peer_id = svc->serving_peer_id;
    received = svc->received_utxos;
    snapsync_service_unlock_internal();

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
