/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast P2P sync protocol for zclassic23 nodes.
 *
 * When two zclassic23 nodes connect, they can use this protocol
 * instead of legacy block-by-block sync. The protocol:
 *
 * 1. SNAPSHOT_OFFER: "I have a UTXO snapshot at height H with root R"
 * 2. SNAPSHOT_REQUEST: "Send me the snapshot"
 * 3. SNAPSHOT_DATA: Chunked UTXO set transfer
 * 4. SNAPSHOT_VERIFY: Recipient verifies Merkle root matches
 * 5. DELTA_SYNC: Sync remaining blocks from snapshot height to tip
 *
 * This reduces initial sync from hours to minutes.
 *
 * Detection: zclassic23 nodes advertise service bit NODE_ZCL23 (1<<10)
 * in the version message. If both peers have it, fast sync activates. */

#ifndef ZCL_NET_FAST_SYNC_H
#define ZCL_NET_FAST_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Service bit for zclassic23 extended protocol */
#define NODE_ZCL23 (1 << 10)

/* Fast sync message types (command names for P2P) */
#define MSG_SNAPSHOT_OFFER   "zsnapshot"
#define MSG_SNAPSHOT_REQ     "zsnapreq"
#define MSG_SNAPSHOT_DATA    "zsnapdata"
#define MSG_SNAPSHOT_END     "zsnapend"

/* BitTorrent-style parallel chunk sync messages */
#define MSG_CHUNK_REQ        "zchunkreq"   /* request chunk by index */
#define MSG_CHUNK_DATA       "zchunkdata"  /* chunk response with index + hash */
#define MSG_MANIFEST         "zmanifest"   /* manifest with chunk count + merkle root */

/* Default UTXOs per chunk for parallel sync */
#define SYNC_CHUNK_SIZE 500

/* ── Rate limiting + PoW defense ─────────────────────────── */

/* Difficulty for snapshot request PoW (number of leading zero bits).
 * 20 bits ≈ ~1M hashes ≈ ~0.5s on modern CPU. Prevents spam. */
#define FAST_SYNC_POW_BITS 20

/* Max snapshot chunks per IP per hour */
#define FAST_SYNC_MAX_CHUNKS_PER_HOUR 5000

/* PoW challenge: client must find nonce such that
 * SHA256(peer_id || timestamp || nonce) has FAST_SYNC_POW_BITS leading zeros.
 * This is included in the zsnapreq message. */
struct fast_sync_pow {
    uint8_t  peer_id[32];   /* SHA256 of requester's IP or node ID */
    int64_t  timestamp;     /* unix timestamp (must be within 5 min) */
    uint64_t nonce;         /* PoW solution */
};

/* Verify a PoW proof. Returns true if valid. */
bool fast_sync_verify_pow(const struct fast_sync_pow *pow);

/* Solve a PoW challenge (blocking, ~0.5s). */
bool fast_sync_solve_pow(const uint8_t peer_id[32], struct fast_sync_pow *pow);

/* Rate limiter state (per node, tracks IPs) */
struct fast_sync_rate_limiter {
    struct {
        uint8_t ip[16];
        int64_t window_start;
        uint32_t chunks_sent;
    } entries[256];
    size_t num_entries;
};

/* Check if an IP is rate-limited. Returns true if OK to serve. */
bool fast_sync_rate_check(struct fast_sync_rate_limiter *rl,
                           const uint8_t ip[16]);

/* UTXO snapshot chunk: batch of UTXOs for transfer.
 * Max 1000 entries for legacy full-snapshot transfer.
 * Parallel sync uses SYNC_CHUNK_SIZE (500) entries per chunk. */
struct utxo_chunk {
    uint32_t num_entries;
    uint32_t chunk_index;     /* index within manifest (for parallel sync) */
    struct {
        uint8_t  txid[32];
        uint32_t vout;
        int64_t  value;
        uint8_t  script[128]; /* most scripts < 128 bytes */
        uint16_t script_len;
        int32_t  height;
    } entries[1000]; /* 1000 max; parallel sync uses 500 */
};

/* Snapshot offer message */
struct snapshot_offer {
    int32_t  height;         /* snapshot height */
    uint8_t  block_hash[32]; /* block hash at height */
    uint8_t  utxo_root[32]; /* Merkle root of UTXO set */
    uint64_t num_utxos;      /* total UTXO count */
    uint64_t total_bytes;    /* estimated transfer size */
};

/* Check if a peer supports zclassic23 fast sync */
static inline bool peer_supports_fast_sync(uint64_t services)
{
    return (services & NODE_ZCL23) != 0;
}

/* Build a snapshot offer from current chain state */
bool fast_sync_build_offer(const char *datadir,
                            struct snapshot_offer *offer);

/* Serve a snapshot to a requesting peer (chunked) */
typedef bool (*chunk_callback)(const struct utxo_chunk *chunk,
                                void *ctx);
bool fast_sync_serve_snapshot(const char *datadir,
                               int from_height,
                               chunk_callback cb, void *ctx);

/* Receive and apply a snapshot */
bool fast_sync_apply_chunk(const char *datadir,
                            const struct utxo_chunk *chunk);

/* Compute UTXO set Merkle root for verification */
bool fast_sync_compute_utxo_root(const char *datadir,
                                  uint8_t root_out[32]);

/* Internal: compute from open db handle */
struct sqlite3;
void fast_sync_compute_utxo_root_db(struct sqlite3 *db,
                                     uint8_t root_out[32]);

/* ── BitTorrent-style parallel chunk sync ────────────────── */

/* Manifest: describes the UTXO snapshot as a set of verified chunks.
 * Each chunk contains chunk_size UTXOs (last chunk may have fewer).
 * A Merkle tree of chunk hashes enables independent verification. */
struct sync_manifest {
    int32_t  height;
    uint8_t  block_hash[32];
    uint64_t num_utxos;
    uint32_t num_chunks;
    uint32_t chunk_size;      /* UTXOs per chunk (default 500) */
    uint8_t  merkle_root[32]; /* root of chunk hash Merkle tree */
    uint8_t  (*chunk_hashes)[32]; /* array of num_chunks hashes (heap) */
};

/* Build manifest from current UTXO set.
 * Allocates chunk_hashes array; caller must call sync_manifest_free(). */
bool fast_sync_build_manifest(const char *datadir,
                               struct sync_manifest *out);

/* Build manifest from an open database handle. */
bool fast_sync_build_manifest_db(struct sqlite3 *db,
                                  struct sync_manifest *out);

/* Free heap memory inside a manifest. */
void sync_manifest_free(struct sync_manifest *m);

/* Compute SHA-256 hash of a single chunk's contents. */
void fast_sync_chunk_hash(const struct utxo_chunk *chunk,
                           uint8_t hash_out[32]);

/* Verify a chunk against its expected hash. */
bool fast_sync_verify_chunk(const struct utxo_chunk *chunk,
                             const uint8_t expected_hash[32]);

/* Serve a specific chunk by index from the UTXO database. */
bool fast_sync_serve_chunk(const char *datadir, uint32_t chunk_index,
                            struct utxo_chunk *out);

/* Serve a chunk from an open database handle. */
bool fast_sync_serve_chunk_db(struct sqlite3 *db, uint32_t chunk_index,
                               uint32_t chunk_size,
                               struct utxo_chunk *out);

/* Compute Merkle root from an array of leaf hashes.
 * Returns root in root_out. Handles power-of-two padding. */
void fast_sync_merkle_root(const uint8_t (*hashes)[32],
                            uint32_t count,
                            uint8_t root_out[32]);

/* Verify a chunk against the Merkle root using a proof path. */
bool fast_sync_verify_chunk_proof(uint32_t chunk_index,
                                   const uint8_t chunk_hash[32],
                                   const uint8_t (*proof)[32],
                                   uint32_t proof_len,
                                   const uint8_t merkle_root[32]);

/* Build a Merkle proof for a given chunk index. Allocates proof array.
 * Caller must free(*proof_out). Returns proof length. */
uint32_t fast_sync_build_proof(const uint8_t (*hashes)[32],
                                uint32_t count,
                                uint32_t chunk_index,
                                uint8_t (**proof_out)[32]);

#endif
