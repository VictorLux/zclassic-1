/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * File Transfer Controller — SHA3-verified, quantum-secure file service.
 *
 * Splits blockchain state (block files, UTXO snapshots) into ~50MB chunks.
 * Each chunk is SHA3-256 hashed. The manifest (ordered list of chunk hashes)
 * SHA3-hashes to a root that can be verified against on-chain data.
 *
 * REST API:
 *   GET /api/files/manifest     → JSON manifest (chunk hashes, sizes)
 *   GET /api/files/:sha3hash    → raw chunk bytes
 *
 * RPC:
 *   getfilemanifest             → JSON manifest
 *   getfilechunk "sha3hash"     → hex-encoded chunk (or error)
 *
 * P2P messages (encrypted with SHA3 stream cipher):
 *   zfilelist   → manifest exchange on ZCL23 handshake
 *   zfilereq    → request chunk by SHA3 hash
 *   zfiledata   → chunk data response */

#ifndef ZCL_FILE_CONTROLLER_H
#define ZCL_FILE_CONTROLLER_H

#include "rpc/server.h"
#include <stdint.h>
#include <stdbool.h>

#define FILE_CHUNK_SIZE (50 * 1024 * 1024)  /* 50 MB per chunk */
#define FILE_MAX_CHUNKS 1024                 /* ~50 GB max total */

struct file_chunk {
    uint8_t  sha3[32];        /* SHA3-256 hash of chunk data */
    uint64_t offset;          /* byte offset in source file */
    uint32_t size;            /* actual size (last chunk may be smaller) */
    uint8_t  file_index;      /* which source file (0=blk0000.dat, etc.) */
};

struct file_manifest {
    struct file_chunk chunks[FILE_MAX_CHUNKS];
    uint32_t          num_chunks;
    uint8_t           root_hash[32]; /* SHA3-256 of all chunk hashes */
    uint8_t           mmr_root[32];  /* MMR root at chain_height (trust anchor) */
    int32_t           chain_height;  /* height when manifest was built */
    uint64_t          total_bytes;   /* total data size */
};

/* Build manifest from block files in datadir. */
bool file_manifest_build(struct file_manifest *fm, const char *datadir);

/* Find a chunk by its SHA3 hash. Returns NULL if not found. */
const struct file_chunk *file_manifest_find(const struct file_manifest *fm,
                                             const uint8_t sha3[32]);

/* Read chunk data from disk. Caller must free(*out). */
bool file_chunk_read(const struct file_chunk *chunk, const char *datadir,
                     uint8_t **out, uint32_t *out_size);

/* Register RPC commands. */
void register_file_rpc_commands(struct rpc_table *t);

/* Set state for the file controller. */
void file_controller_init(const char *datadir);

/* Get the cached manifest (built in background). */
const struct file_manifest *file_controller_get_manifest(void);

/* Export public consensus tables from node.db to consensus_snapshot.db.
 * SECURITY: excludes wallet_keys, wallet_utxos, wallet_sapling_keys,
 * wallet_sapling_notes, node_state — only public blockchain data.
 * Returns true on success. Safe to call from background thread. */
bool file_export_consensus_snapshot(const char *datadir);

#endif
