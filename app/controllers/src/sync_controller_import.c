/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* sync_controller_import: parallel LevelDB → SQLite UTXO import.
 *
 * Reader thread feeds a ring buffer; N decoder threads deserialize
 * coins-format entries; a single writer thread bulk-inserts into the
 * utxos table. Split out of sync_controller.c. See
 * sync_controller_internal.h for cross-file glue. */

#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"
#include "services/recovery_policy.h"
#include "models/db_txn.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "chain/chain.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "wallet/sapling_keys.h"
#include "keys/key.h"
#include "core/hash.h"
#include "core/serialize.h"
#include "core/utiltime.h"
#include "script/standard.h"
#include "storage/disk_block_io.h"
#include "storage/dbwrapper.h"
#include "storage/coins_db.h"
#include "coins/undo.h"
#include "validation/chainstate.h"
#include "validation/txmempool.h"
#include "sapling/incremental_merkle_tree.h"
#include "sapling/sapling.h"
#include "sapling/note_encryption.h"
#include "support/cleanse.h"
#include "event/event.h"
#include "config/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

extern volatile sig_atomic_t g_shutdown_requested;


/* ── Parallel UTXO import: LevelDB → SQLite ────────────────────────
 *
 * Architecture: Reader → Ring Buffer → N Decoders → Queue → Writer
 *
 * Reader thread:  Sequential LevelDB iteration, copies raw key+value
 *                 into chunks. Single-threaded (LevelDB not thread-safe).
 * Decoder threads: N parallel workers deserialize coins format, classify
 *                  scripts, extract height. Pure CPU, no shared state.
 * Writer thread:   Single SQLite writer with direct bind/step, no
 *                  ActiveRecord overhead. journal_mode=OFF for max speed.
 *
 * Eliminates: double-write (INSERT+UPDATE), per-txid prepare/finalize,
 *             validation callbacks, 10KB tx_out stack allocations.
 * ─────────────────────────────────────────────────────────────────── */

/* Compact row for the pipeline — 128 bytes vs 10KB for tx_out+db_utxo */
struct utxo_row {
    uint8_t  txid[32];
    uint8_t  address_hash[20];
    uint8_t  script[80];       /* inline for scripts ≤80 bytes (99.9%) */
    uint8_t *script_overflow;  /* heap alloc for rare large scripts */
    int64_t  value;
    int32_t  height;
    uint32_t vout;
    uint16_t script_len;
    uint8_t  script_type;
    uint8_t  has_address;
    uint8_t  is_coinbase;
};

/* A chunk of raw LevelDB entries for decode workers */
#define IMPORT_CHUNK_ENTRIES 2048
/* Max outputs per chunk. Must be large enough to hold all outputs from
 * 2048 entries. Worst case: 2048 entries * 468 outputs = 958,464.
 * In practice ~5500 rows per chunk. Use 32768 for 4x safety margin. */
#define IMPORT_MAX_ROWS_PER_CHUNK 32768

struct raw_entry {
    uint8_t  txid[32];
    uint8_t *value;     /* heap copy of deobfuscated value */
    uint16_t value_len;
};

struct import_chunk {
    struct raw_entry entries[IMPORT_CHUNK_ENTRIES];
    int num_entries;
    struct utxo_row rows[IMPORT_MAX_ROWS_PER_CHUNK];
    int num_rows;
    _Atomic int state; /* 0=free, 1=filled, 2=decoded */
};

#define IMPORT_NUM_CHUNKS 64
/* Auto-detect decoder count: use all cores minus 2 (reader + writer).
 * Minimum 4, maximum 32. More decoders = faster LevelDB deserialization. */
#include <unistd.h>
static int import_num_decoders(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 6) return 4;
    if (n > 34) return 32;
    return (int)(n - 2);
}
#define IMPORT_NUM_DECODERS_MAX 32

static bool import_writer_bind_checked(sqlite3_stmt *stmt,
                                      const char *label,
                                      int rc,
                                      const struct node_db *ndb,
                                      int row_no)
{
    if (!stmt) LOG_FAIL("sync", "import_writer_bind: stmt is NULL for %s", label);
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "UTXO import writer: %s failed at row %d (rc=%d): %s\n",
                label, row_no, rc,
                ndb ? sqlite3_errmsg(ndb->db) : "db unavailable");
        return false;
    }
    return true;
}

static bool import_writer_step_checked(sqlite3_stmt *stmt,
                                      const struct node_db *ndb,
                                      int row_no)
{
    int step_rc = AR_STEP_ROW_READONLY(stmt);
    if (step_rc != SQLITE_DONE) {
        fprintf(stderr,
                "UTXO import writer: sqlite3_step failed at row %d (rc=%d): %s\n",
                row_no, step_rc,
                ndb ? sqlite3_errmsg(ndb->db) : "db unavailable");
        return false;
    }
    return true;
}

struct import_context {
    struct import_chunk chunks[IMPORT_NUM_CHUNKS];
    _Atomic int total_txids;
    _Atomic int total_rows;
    _Atomic int decode_failures;
    _Atomic int skipped_outputs;
    _Atomic bool cancel_requested;
    _Atomic bool reader_done;
    _Atomic bool decoders_done;
    _Atomic int chunks_produced;  /* total chunks filled by reader */
    _Atomic int chunks_consumed;  /* total chunks written by writer */
    /* LevelDB reader state (single-threaded) */
    struct coins_view_db *cvdb;
    /* Writer state */
    struct node_db *ndb;
    int write_next; /* next chunk index to write (in-order) */
};

struct import_job {
    struct import_context *ctx;
    pthread_t decoders[IMPORT_NUM_DECODERS_MAX];
    int num_decoders;
    int decoder_threads_started;
    pthread_t writer_thread;
    bool writer_thread_started;
};

static void *import_decoder_thread(void *arg);
static void *import_writer_thread(void *arg);
static void import_job_join_decoders(struct import_job *job);

static bool import_ctx_should_stop(const struct import_context *ctx)
{
    return (ctx && atomic_load(&ctx->cancel_requested)) || g_shutdown_requested;
}

static void import_ctx_request_stop(struct import_context *ctx)
{
    if (!ctx)
        return;
    atomic_store(&ctx->cancel_requested, true);
}

static void import_chunk_reset(struct import_chunk *chunk)
{
    if (!chunk)
        return;
    for (int ei = 0; ei < chunk->num_entries; ei++)
        free(chunk->entries[ei].value);
    for (int ri = 0; ri < chunk->num_rows; ri++)
        free(chunk->rows[ri].script_overflow);
    chunk->num_entries = 0;
    chunk->num_rows = 0;
    atomic_store(&chunk->state, 0);
}

static void import_context_release_chunks(struct import_context *ctx)
{
    if (!ctx)
        return;
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        import_chunk_reset(&ctx->chunks[i]);
}

static void import_job_init(struct import_job *job,
                            struct import_context *ctx,
                            int num_decoders)
{
    if (!job)
        return;
    memset(job, 0, sizeof(*job));
    job->ctx = ctx;
    job->num_decoders = num_decoders;
}

static bool import_job_start_decoders(struct import_job *job)
{
    if (!job || !job->ctx)
        LOG_FAIL("sync", "import_start_decoders: invalid args (job=%p)", (void *)job);

    for (int i = 0; i < job->num_decoders; i++) {
        int rc = thread_registry_spawn_ex("zcl_utxo_dec",
                                           import_decoder_thread, job->ctx,
                                           &job->decoders[i]);
        if (rc != 0) {
            fprintf(stderr,
                    "UTXO import: thread_registry_spawn_ex decoder[%d] failed: %d\n",
                    i, rc);
            import_ctx_request_stop(job->ctx);
            import_job_join_decoders(job);
            return false;
        }
        job->decoder_threads_started++;
    }
    return job->decoder_threads_started > 0;
}

static bool import_job_start_writer(struct import_job *job)
{
    if (!job || !job->ctx)
        LOG_FAIL("sync", "import_start_writer: invalid args (job=%p)", (void *)job);
    if (thread_registry_spawn_ex("zcl_utxo_wr",
                                  import_writer_thread, job->ctx,
                                  &job->writer_thread) != 0) {
        fprintf(stderr, "UTXO import: FATAL — writer thread failed to start\n");
        import_ctx_request_stop(job->ctx);
        return false;
    }
    job->writer_thread_started = true;
    return true;
}

static void import_job_join_decoders(struct import_job *job)
{
    if (!job)
        return;
    for (int i = 0; i < job->decoder_threads_started; i++)
        pthread_join(job->decoders[i], NULL);
    job->decoder_threads_started = 0;
}

static void import_job_join_writer(struct import_job *job)
{
    if (!job || !job->writer_thread_started)
        return;
    pthread_join(job->writer_thread, NULL);
    job->writer_thread_started = false;
}

static bool import_job_start(struct import_job *job)
{
    if (!import_job_start_decoders(job))
        LOG_FAIL("sync", "import_job_start: decoders failed to start");
    if (!import_job_start_writer(job)) {
        if (job && job->ctx)
            atomic_store(&job->ctx->reader_done, true);
        import_job_join_decoders(job);
        LOG_FAIL("sync", "import_job_start: writer failed to start");
    }
    return true;
}

/* Decode a single raw coins entry into utxo_row structs.
 * Returns number of rows produced. Pure function, no shared state. */
static int decode_coins_entry(const struct raw_entry *raw,
                              struct utxo_row *out, int max_rows)
{
    struct byte_stream s;
    stream_init_from_data(&s, raw->value, raw->value_len);

    uint64_t nVersion = 0;
    if (!stream_read_varint(&s, &nVersion)) { stream_free(&s); return 0; }

    uint64_t nCode = 0;
    if (!stream_read_varint(&s, &nCode)) { stream_free(&s); return 0; }

    bool is_coinbase = (nCode & 1) != 0;
    bool vout0_present = (nCode & 2) != 0;
    bool vout1_present = (nCode & 4) != 0;
    unsigned int nMaskCode = (unsigned int)(nCode / 8) +
        ((vout0_present || vout1_present) ? 0 : 1);

    if (nMaskCode > 10000) { stream_free(&s); return 0; }

    /* Build availability vector (max 4096 vouts per tx, largest seen: 468) */
    size_t num_avail = 2;
    bool avail[4096];
    memset(avail, 0, sizeof(avail));
    avail[0] = vout0_present;
    avail[1] = vout1_present;

    unsigned int mask_remaining = nMaskCode;
    while (mask_remaining > 0) {
        unsigned char ch = 0;
        if (!stream_read_bytes(&s, &ch, 1)) break;
        for (unsigned int p = 0; p < 8 && num_avail < 4096; p++)
            avail[num_avail++] = (ch & (1 << p)) != 0;
        if (ch != 0) mask_remaining--;
    }

    /* Deserialize each available output.
     * We do inline parsing instead of compressed_txout_deserialize to avoid
     * secp256k1 point decompression (unnecessary for index) and to ensure
     * every output is captured — zero tolerance for data loss. */
    int nrows = 0;
    for (size_t vi = 0; vi < num_avail && nrows < max_rows; vi++) {
        if (!avail[vi]) continue;

        /* Read compressed amount (varint) */
        uint64_t comp_amount = 0;
        if (!stream_read_varint(&s, &comp_amount)) break;
        int64_t value = (int64_t)decompress_amount(comp_amount);

        /* Read script type/size (varint) */
        uint64_t nSize = 0;
        if (!stream_read_varint(&s, &nSize)) break;

        /* Determine raw script data size in the stream */
        size_t raw_script_len = 0;
        if (nSize == 0 || nSize == 1) raw_script_len = 20;      /* P2PKH / P2SH hash */
        else if (nSize >= 2 && nSize <= 5) raw_script_len = 32;  /* compressed pubkey */
        else raw_script_len = (size_t)(nSize - 6);               /* raw script */

        /* Read raw script data */
        uint8_t raw_script[10240];
        if (raw_script_len > sizeof(raw_script)) raw_script_len = sizeof(raw_script);
        if (!stream_read_bytes(&s, raw_script, raw_script_len)) break;

        /* Reconstruct full script for classification */
        struct utxo_row *r = &out[nrows];
        memcpy(r->txid, raw->txid, 32);
        r->vout = (uint32_t)vi;
        r->value = value;
        r->is_coinbase = is_coinbase;
        r->height = 0;
        r->script_overflow = NULL;
        r->has_address = 0;
        r->script_type = 0; /* OTHER */

        if (nSize == 0) {
            /* P2PKH: OP_DUP OP_HASH160 <20 bytes> OP_EQUALVERIFY OP_CHECKSIG */
            r->script_len = 25;
            r->script[0] = 0x76; r->script[1] = 0xa9; r->script[2] = 0x14;
            memcpy(r->script + 3, raw_script, 20);
            r->script[23] = 0x88; r->script[24] = 0xac;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 1; /* P2PKH */
        } else if (nSize == 1) {
            /* P2SH: OP_HASH160 <20 bytes> OP_EQUAL */
            r->script_len = 23;
            r->script[0] = 0xa9; r->script[1] = 0x14;
            memcpy(r->script + 2, raw_script, 20);
            r->script[22] = 0x87;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 2; /* P2SH */
        } else if (nSize >= 2 && nSize <= 5) {
            /* Compressed/uncompressed pubkey → P2PK script.
             * Store the raw 33-byte compressed pubkey directly as script.
             * We skip secp256k1 decompression — not needed for indexing. */
            uint8_t prefix = (nSize == 2 || nSize == 4) ? 0x02 : 0x03;
            r->script_len = 35; /* 1(push33) + 33(pubkey) + 1(OP_CHECKSIG) */
            r->script[0] = 0x21; /* push 33 bytes */
            r->script[1] = prefix;
            memcpy(r->script + 2, raw_script, 32);
            r->script[34] = 0xac; /* OP_CHECKSIG */
        } else {
            /* Raw script */
            uint16_t slen = (uint16_t)raw_script_len;
            r->script_len = slen;
            if (slen <= sizeof(r->script)) {
                memcpy(r->script, raw_script, slen);
            } else {
                r->script_overflow = zcl_malloc(slen, "script overflow");
                if (r->script_overflow) {
                    memcpy(r->script_overflow, raw_script, slen);
                } else {
                    /* malloc failed — cap to inline buffer */
                    r->script_len = (uint16_t)sizeof(r->script);
                    memcpy(r->script, raw_script, sizeof(r->script));
                }
            }
            /* Classify raw script */
            const uint8_t *sc = r->script_overflow ? r->script_overflow : r->script;
            if (slen == 25 && sc[0]==0x76 && sc[1]==0xa9 && sc[2]==0x14 &&
                sc[23]==0x88 && sc[24]==0xac) {
                memcpy(r->address_hash, sc + 3, 20);
                r->has_address = 1;
                r->script_type = 1;
            } else if (slen == 23 && sc[0]==0xa9 && sc[1]==0x14 && sc[22]==0x87) {
                memcpy(r->address_hash, sc + 2, 20);
                r->has_address = 1;
                r->script_type = 2;
            } else if (slen > 0 && sc[0] == 0x6a) {
                r->script_type = 3;
            }
        }
        nrows++;
    }

    /* Read height varint (comes after all outputs) and stamp all rows.
     * Sanity-check: height must be ≤ 10M (no chain is taller). */
    uint64_t height = 0;
    if (stream_read_varint(&s, &height) && height <= 10000000) {
        for (int i = 0; i < nrows; i++)
            out[i].height = (int32_t)height;
    }

    stream_free(&s);
    return nrows;
}

/* Decoder worker thread — picks filled chunks, decodes, marks decoded */
static void *import_decoder_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;

    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a filled chunk to decode */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int expected = 1; /* filled */
            if (atomic_compare_exchange_strong(&ctx->chunks[i].state,
                                              &expected, -1)) {
                atomic_thread_fence(memory_order_acquire);
                chunk = &ctx->chunks[i];
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->reader_done)) {
                /* Check once more for any remaining chunks */
                bool found = false;
                for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                    if (atomic_load(&ctx->chunks[i].state) == 1) {
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }
            /* Yield briefly — spin is fine on 32 cores */
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Decode all entries in this chunk */
        chunk->num_rows = 0;
        int skipped_in_chunk = 0;
        for (int i = 0; i < chunk->num_entries; i++) {
            if (import_ctx_should_stop(ctx))
                break;
            int space = IMPORT_MAX_ROWS_PER_CHUNK - chunk->num_rows;
            if (space <= 0) { skipped_in_chunk += chunk->num_entries - i; break; }
            int n = decode_coins_entry(&chunk->entries[i],
                                       &chunk->rows[chunk->num_rows],
                                       space);
            if (n == 0) {
                atomic_fetch_add(&ctx->decode_failures, 1);
            }
            chunk->num_rows += n;
        }
        if (skipped_in_chunk > 0) {
            atomic_fetch_add(&ctx->skipped_outputs, skipped_in_chunk);
            fprintf(stderr, "UTXO import: chunk overflow! %d entries skipped "
                    "(rows=%d, max=%d)\n", skipped_in_chunk,
                    chunk->num_rows, IMPORT_MAX_ROWS_PER_CHUNK);
        }

        if (import_ctx_should_stop(ctx))
            import_chunk_reset(chunk);
        else
            atomic_store(&chunk->state, 2); /* decoded */
    }
    return NULL;
}

/* Writer thread — consumes decoded chunks, inserts into SQLite */
static void *import_writer_thread(void *arg)
{
    struct import_context *ctx = (struct import_context *)arg;
    if (!ctx) LOG_NULL("sync", "import_writer_thread: ctx is NULL");
    struct node_db *ndb = ctx->ndb;
    if (!ndb || !ndb->open || !ndb->stmt_utxo_insert) {
        fprintf(stderr, "UTXO import writer: invalid node_db statement/db state\n");
        import_ctx_request_stop(ctx);
        return NULL;
    }
    sqlite3_stmt *ins = ndb->stmt_utxo_insert;
    int total_rows = 0;
    int next_chunk = 0;

    if (!node_db_begin(ndb)) {
        fprintf(stderr, "UTXO import writer: BEGIN failed\n");
        import_ctx_request_stop(ctx);
    }

    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Look for any decoded chunk to write */
        struct import_chunk *chunk = NULL;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int idx = (next_chunk + i) % IMPORT_NUM_CHUNKS;
            int expected = 2; /* decoded */
            if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
                                              &expected, -1)) {
                chunk = &ctx->chunks[idx];
                next_chunk = (idx + 1) % IMPORT_NUM_CHUNKS;
                break;
            }
        }
        if (!chunk) {
            if (atomic_load(&ctx->decoders_done)) {
                /* Decoders are done. Use definitive chunk count to
                 * know when we're truly finished — no race possible. */
                int produced = atomic_load(&ctx->chunks_produced);
                int consumed = atomic_load(&ctx->chunks_consumed);
                if (consumed >= produced) break;
                /* Still have chunks to consume — scan harder */
                atomic_thread_fence(memory_order_seq_cst);
            }
            struct timespec ts = {0, 100000}; /* 100μs */
            nanosleep(&ts, NULL);
            continue;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Insert all rows from this chunk */
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            if (import_ctx_should_stop(ctx))
                break;
            struct utxo_row *r = &chunk->rows[ri];
            const uint8_t *sc = r->script_overflow ?
                                r->script_overflow : r->script;
            bool row_ok = true;

            row_ok &= import_writer_bind_checked(ins, "sqlite3_reset",
                                                sqlite3_reset(ins), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(txid)",
                                                sqlite3_bind_blob(ins, 1, r->txid, 32, SQLITE_STATIC), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(vout)",
                                                sqlite3_bind_int(ins, 2, (int)r->vout), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int64(value)",
                                                sqlite3_bind_int64(ins, 3, r->value), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(script)",
                                                sqlite3_bind_blob(ins, 4, sc, (int)r->script_len, SQLITE_STATIC), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(script_type)",
                                                sqlite3_bind_int(ins, 5, r->script_type), ndb,
                                                total_rows);
            if (r->has_address)
                row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_blob(address_hash)",
                                                    sqlite3_bind_blob(ins, 6, r->address_hash, 20, SQLITE_STATIC), ndb,
                                                    total_rows);
            else
                row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_null(address_hash)",
                                                    sqlite3_bind_null(ins, 6), ndb,
                                                    total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(height)",
                                                sqlite3_bind_int(ins, 7, r->height), ndb,
                                                total_rows);
            row_ok &= import_writer_bind_checked(ins, "sqlite3_bind_int(is_coinbase)",
                                                sqlite3_bind_int(ins, 8, r->is_coinbase), ndb,
                                                total_rows);
            row_ok &= import_writer_step_checked(ins, ndb, total_rows);
            if (!row_ok) {
                import_ctx_request_stop(ctx);
                break;
            }
            total_rows++;
        }

        if (import_ctx_should_stop(ctx)) {
            import_chunk_reset(chunk);
            break;
        }

        /* Commit every ~100K rows */
        if (total_rows % 100000 < chunk->num_rows) {
            if (!node_db_commit(ndb)) {
                fprintf(stderr, "UTXO import writer: COMMIT failed\n");
                if (!node_db_rollback(ndb))
                    fprintf(stderr, "UTXO import writer: ROLLBACK failed after commit failure\n");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
            sync_job_import_progress(total_rows);
            printf("UTXO import: %d rows written...\n", total_rows);
            fflush(stdout);
            if (!node_db_begin(ndb)) {
                fprintf(stderr, "UTXO import writer: BEGIN restart failed\n");
                if (!node_db_rollback(ndb))
                    fprintf(stderr, "UTXO import writer: rollback after BEGIN restart failure failed\n");
                import_ctx_request_stop(ctx);
                import_chunk_reset(chunk);
                break;
            }
        }

        /* Free buffers and release chunk */
        for (int ei = 0; ei < chunk->num_entries; ei++) {
            free(chunk->entries[ei].value);
            chunk->entries[ei].value = NULL;
        }
        for (int ri = 0; ri < chunk->num_rows; ri++) {
            free(chunk->rows[ri].script_overflow);
            chunk->rows[ri].script_overflow = NULL;
        }
        chunk->num_entries = 0;
        chunk->num_rows = 0;
        atomic_fetch_add(&ctx->chunks_consumed, 1);
        atomic_store(&chunk->state, 0); /* free for reuse */
    }

    if (!import_ctx_should_stop(ctx)) {
        if (!node_db_commit(ndb))
            fprintf(stderr, "UTXO import writer: final COMMIT failed\n");
    } else {
        if (!node_db_rollback(ndb))
            fprintf(stderr, "UTXO import writer: rollback requested by stop flag failed\n");
    }
    sync_job_import_progress(total_rows);
    atomic_store(&ctx->total_rows, total_rows);
    return NULL;
}

int node_db_sync_import_utxos(struct node_db *ndb,
                               struct coins_view_db *cvdb)
{
    struct import_job job;
    struct sync_db_turbo_scope turbo_mode = {0};
    bool restore_ok = true;

    if (!ndb || !ndb->open || !cvdb)
        LOG_ERR("sync", "import_utxos: invalid args (ndb=%p, cvdb=%p)", (void *)ndb, (void *)cvdb);
    sync_job_import_begin();

    int num_decoders = import_num_decoders();
    printf("UTXO import: parallel pipeline (%d decoders, %d chunks, %ld cores)...\n",
           num_decoders, IMPORT_NUM_CHUNKS, sysconf(_SC_NPROCESSORS_ONLN));
    fflush(stdout);

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* ── SQLite turbo mode — delegate to node_db layer ────────────── */
    if (!sync_db_turbo_scope_begin(&turbo_mode, ndb, true)) {
        fprintf(stderr, "UTXO import: failed to enter turbo mode\n");
        sync_job_import_finish(0);
        return -1; // raw-return-ok:logged-above
    }

    /* ── Recovery policy gate + scoped wipe ────────────────────────
     * The wipe below is a reimport prelude, not a reorg rollback — in
     * normal operation the table is empty or nearly empty. Historically
     * this call site is *not* the one that caused 2026-04-10, but it
     * shares a primitive with the paths that did, so we gate it the
     * same way: ask the policy, refuse if over cap, abort cleanly.
     * The cap is deliberately generous here (reimport is legitimate)
     * but an operator can still raise ZCL_MAX_UTXO_WIPE_ROWS if a
     * partial import is being resumed.
     *
     * The DELETE + initial state are wrapped in a DB_TXN_SCOPE so a
     * mid-wipe crash or early-return rolls back cleanly: the pipeline
     * below starts from a fully-wiped-and-committed table, not a
     * half-deleted one. */
    /* Skip the wipe if the table is already empty — the caller
     * (utxo_recovery_import_ldb) already wiped at service line 168.
     * This was the third redundant wipe that destroyed data. */
    int64_t existing = node_db_utxo_count(ndb);
    if (existing < 0) existing = 0;

    if (existing > 0) {
        struct recovery_policy rp;
        policy_load_from_env(&rp);
        enum policy_decision pd = policy_check_utxo_wipe(
            &rp, existing, "sync_controller.import_utxos_reimport");
        if (pd != POLICY_ALLOW) {
            fprintf(stderr,
                    "UTXO import: recovery_policy refused wipe (code=%s, rows=%lld)\n",
                    policy_decision_name(pd), (long long)existing);
            if (!sync_db_turbo_scope_end(&turbo_mode))
                fprintf(stderr, "UTXO import: failed to restore normal mode after policy refusal\n");
            sync_job_import_finish(0);
            return -1; // raw-return-ok:logged-above
        }

        {
            DB_TXN_SCOPE(txn, ndb, "sync_controller.import_utxos_reimport");
            if (!txn) {
                fprintf(stderr, "UTXO import: failed to open db_txn for wipe\n");
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after db_txn failure\n");
                sync_job_import_finish(0);
                return -1; // raw-return-ok:logged-above
            }
            if (!node_db_wipe_utxos(ndb)) {
                fprintf(stderr, "UTXO import: failed to wipe utxos table\n");
                /* leave scope → auto-rollback */
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after wipe failure\n");
                sync_job_import_finish(0);
                return -1; // raw-return-ok:logged-above
            }
            if (!db_txn_commit(txn)) {
                fprintf(stderr, "UTXO import: commit of wipe failed\n");
                if (!sync_db_turbo_scope_end(&turbo_mode))
                    fprintf(stderr, "UTXO import: failed to restore normal mode after commit failure\n");
                sync_job_import_finish(0);
                return -1; // raw-return-ok:logged-above
            }
        }
    }

    /* ── Initialize pipeline context ───────────────────────────────── */
    struct import_context *ctx = zcl_calloc(1, sizeof(struct import_context), "import context");
    if (!ctx) {
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after alloc failure\n");
        sync_job_import_finish(0);
        return -1; // raw-return-ok:logged-above
    }
    ctx->cvdb = cvdb;
    ctx->ndb = ndb;
    atomic_store(&ctx->cancel_requested, false);
    atomic_store(&ctx->reader_done, false);
    atomic_store(&ctx->decoders_done, false);
    for (int i = 0; i < IMPORT_NUM_CHUNKS; i++)
        atomic_store(&ctx->chunks[i].state, 0);
    import_job_init(&job, ctx, num_decoders);

    /* ── Start decoder + writer threads ────────────────────────────── */
    if (!import_job_start(&job)) {
        fprintf(stderr, "UTXO import: FATAL — worker pipeline failed to start\n");
        import_context_release_chunks(ctx);
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after worker startup failure\n");
        free(ctx);
        sync_job_import_finish(0);
        return -1; // raw-return-ok:logged-above
    }

    /* ── Reader (main thread): iterate LevelDB, fill chunks ────────── */
    /* Take a LevelDB snapshot so the iterator sees a consistent,
     * frozen view even if zclassicd is writing concurrently.
     * This prevents the random UTXO gaps caused by non-atomic reads. */
    db_wrapper_snapshot_begin(&cvdb->db);

    struct db_iterator it;
    db_iter_init(&it, &cvdb->db);
    char seek_key[33];
    seek_key[0] = 'c';
    memset(seek_key + 1, 0, 32);
    db_iter_seek(&it, seek_key, 33);

    int chunk_idx = 0;
    int total_entries = 0;
    int skipped_entries = 0;

    while (db_iter_valid(&it)) {
        if (import_ctx_should_stop(ctx))
            break;
        /* Find a free chunk */
        struct import_chunk *chunk = NULL;
        while (!chunk) {
            if (import_ctx_should_stop(ctx))
                goto reader_done;
            for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
                int idx = (chunk_idx + i) % IMPORT_NUM_CHUNKS;
                int expected = 0;
                if (atomic_compare_exchange_strong(&ctx->chunks[idx].state,
                                                  &expected, -1)) {
                    chunk = &ctx->chunks[idx];
                    chunk_idx = (idx + 1) % IMPORT_NUM_CHUNKS;
                    break;
                }
            }
            if (!chunk) {
                struct timespec ts = {0, 50000}; /* 50μs */
                nanosleep(&ts, NULL);
            }
        }

        /* Fill chunk with raw entries from LevelDB */
        chunk->num_entries = 0;
        chunk->num_rows = 0;

        while (chunk->num_entries < IMPORT_CHUNK_ENTRIES &&
               db_iter_valid(&it)) {
            if (import_ctx_should_stop(ctx))
                goto reader_done;
            size_t key_len;
            const char *key_data = db_iter_key(&it, &key_len);
            if (key_len < 1 || key_data[0] != 'c') goto reader_done;
            if (key_len < 33) { db_iter_next(&it); continue; }

            struct raw_entry *e = &chunk->entries[chunk->num_entries];
            memcpy(e->txid, key_data + 1, 32);

            size_t val_len;
            const char *val_data = db_iter_value(&it, &val_len);
            if (val_len > 65535) val_len = 65535;
            e->value = zcl_malloc(val_len, "import entry value");
            if (e->value) {
                memcpy(e->value, val_data, val_len);
                /* db_iter_value() already deobfuscates values using the
                 * obfuscation key (dbwrapper.c:370-372). Do NOT XOR
                 * again here — that would undo the deobfuscation. */
                e->value_len = (uint16_t)val_len;
                chunk->num_entries++;
                total_entries++;
            } else {
                fprintf(stderr, "WARNING: malloc failed for chunk entry value (%zu bytes), skipping entry\n", val_len);
                skipped_entries++;
            }
            db_iter_next(&it);
        }

        if (chunk->num_entries > 0) {
            atomic_fetch_add(&ctx->chunks_produced, 1);
            atomic_thread_fence(memory_order_release);
            atomic_store(&chunk->state, 1); /* filled → decoders */
        } else {
            atomic_store(&chunk->state, 0); /* empty, release */
        }
    }
reader_done:
    /* Check for iterator errors — checksum failures can cause early
     * termination, silently dropping remaining entries. */
    {
        extern void db_iter_check_error(struct db_iterator *it);
        db_iter_check_error(&it);
    }
    db_iter_free(&it);
    db_wrapper_snapshot_end(&cvdb->db);
    atomic_store(&ctx->reader_done, true);

    printf("UTXO import: read %d txids from LevelDB\n", total_entries);
    fflush(stdout);
    fflush(stdout);

    /* ── Wait for decoders ────────────────────────────────────────── */
    import_job_join_decoders(&job);

    /* ── Wait for ALL chunks to be consumed by writer ──────────── */
    /* After decoders finish, remaining chunks are in state 2 (decoded).
     * We MUST wait for the writer to consume them ALL before signaling
     * decoders_done. Otherwise the writer sees decoders_done=true, does
     * a quick scan, misses state=2 chunks due to timing, and exits
     * early — dropping the last ~219 txids / ~520 UTXOs. */
    for (;;) {
        if (import_ctx_should_stop(ctx))
            break;
        bool any_pending = false;
        for (int i = 0; i < IMPORT_NUM_CHUNKS; i++) {
            int s = atomic_load_explicit(&ctx->chunks[i].state,
                                          memory_order_acquire);
            if (s == 1 || s == 2) { any_pending = true; break; }
        }
        if (!any_pending) break;
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    atomic_store(&ctx->decoders_done, true);

    /* ── Wait for writer ───────────────────────────────────────────── */
    import_job_join_writer(&job);
    int total_rows = atomic_load(&ctx->total_rows);
    sync_job_import_progress(total_rows);
    int decode_fail = atomic_load(&ctx->decode_failures);
    int skip_out = atomic_load(&ctx->skipped_outputs);
    if (decode_fail > 0 || skip_out > 0)
        printf("UTXO import: %d decode failures, %d skipped outputs\n",
               decode_fail, skip_out);

    /* Validation: verify all txids made it to SQLite */
    {
        sqlite3_stmt *cnt = NULL;
        sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(DISTINCT txid), COUNT(*) FROM utxos",
            -1, &cnt, NULL);
        if (cnt && AR_STEP_ROW_READONLY(cnt) == SQLITE_ROW) {
            int64_t sql_txids = sqlite3_column_int64(cnt, 0);
            int64_t sql_rows = sqlite3_column_int64(cnt, 1);
            if (sql_rows != total_rows) {
                /* Row count mismatch = real data loss — pipeline bug */
                fprintf(stderr, "UTXO IMPORT ERROR: wrote %d rows but "
                        "SQLite has %lld rows — data loss!\n",
                        total_rows, (long long)sql_rows);
            } else if (sql_txids < total_entries) {
                /* Fewer distinct txids is expected: fully-pruned CCoins
                 * (all outputs spent) produce zero rows per txid.
                 * These exist in LevelDB as tombstones until compaction. */
                int pruned = total_entries - (int)sql_txids;
                printf("UTXO import: %d/%d LevelDB entries were "
                       "fully-pruned (all outputs spent)\n",
                       pruned, total_entries);
            }
        }
        sqlite3_finalize(cnt);
    }
    fflush(stdout);

    struct timespec ts_write;
    clock_gettime(CLOCK_MONOTONIC, &ts_write);
    double pipe_ms = (ts_write.tv_sec - ts_start.tv_sec) * 1000.0 +
                     (ts_write.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import: %d rows written in %.0fms\n", total_rows, pipe_ms);
    fflush(stdout);

    if (import_ctx_should_stop(ctx)) {
        fprintf(stderr, "UTXO import: aborted%s\n",
                g_shutdown_requested ? " on shutdown" : "");
        if (!sync_db_turbo_scope_end(&turbo_mode))
            fprintf(stderr, "UTXO import: failed to restore normal mode after abort\n");
        restore_ok = false;
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return -1; // raw-return-ok:logged-above
    }

    /* ── Rebuild all indexes for power-node queries ────────────────── */
    printf("UTXO import: building indexes for fast queries...\n");
    fflush(stdout);

    struct timespec ts_idx;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx);

    /* Rebuild indexes and restore safe pragmas */
    if (!sync_db_turbo_scope_end(&turbo_mode)) {
        restore_ok = false;
        fprintf(stderr, "UTXO import: failed to restore normal mode\n");
    }
    if (!restore_ok) {
        import_context_release_chunks(ctx);
        free(ctx);
        sync_job_import_finish(total_rows);
        return -1; // raw-return-ok:logged-above
    }

    struct timespec ts_idx_done;
    clock_gettime(CLOCK_MONOTONIC, &ts_idx_done);
    double idx_ms = (ts_idx_done.tv_sec - ts_idx.tv_sec) * 1000.0 +
                    (ts_idx_done.tv_nsec - ts_idx.tv_nsec) / 1e6;
    printf("UTXO import: indexes built in %.0fms\n", idx_ms);

    double total_ms = (ts_idx_done.tv_sec - ts_start.tv_sec) * 1000.0 +
                      (ts_idx_done.tv_nsec - ts_start.tv_nsec) / 1e6;
    printf("UTXO import complete: %d outputs from %d txids in %.1fs "
           "(pipeline %.0fms + index %.0fms)\n",
           total_rows, total_entries, total_ms / 1000.0,
           pipe_ms, idx_ms);
    fflush(stdout);

    import_context_release_chunks(ctx);
    free(ctx);
    sync_job_import_finish(total_rows);
    return total_rows;
}
