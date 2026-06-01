/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_validator — default header validator for the
 * validate_headers Job. Reconstructs the block header from the in-memory or
 * persisted block index and checks PoW target plus Equihash solution. No
 * cursor logic lives here. */

#include "validate_headers_internal.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "config/runtime.h"
#include "core/uint256.h"
#include "models/block.h"
#include "primitives/block.h"
#include "storage/block_index_db.h"
#include "validation/check_block.h"

#include <stdio.h>
#include <string.h>

extern struct block_tree_db *g_active_block_tree;

/* node.db handle for the SQLite solution fallback. Defaults to the live
 * runtime handle (NULL outside a booted node, e.g. unit tests); tests
 * inject a fixture handle via validate_headers_validator_set_node_db().
 * The bytes here are validated IDENTICALLY to the in-memory/persisted
 * paths — this is purely a different *source* for the same nSolution. */
static struct node_db *g_fallback_node_db = NULL;
static bool            g_fallback_node_db_set = false;

void validate_headers_validator_set_node_db(struct node_db *ndb);
void validate_headers_validator_set_node_db(struct node_db *ndb)
{
    g_fallback_node_db     = ndb;
    g_fallback_node_db_set = true;
}

static struct node_db *fallback_node_db(void)
{
    if (g_fallback_node_db_set)
        return g_fallback_node_db;
    return app_runtime_node_db();
}

/* Source the Equihash solution from the node.db blocks.solution BLOB when
 * the in-memory and persisted block-index records have evicted it. Builds
 * the SAME struct block_header the index paths build, differing only in
 * where nSolution came from. Returns false (with a distinct reason) when
 * node.db has no real solution either — NEVER a synthesized/empty one, so
 * the caller keeps failing rather than passing without a verified PoW. */
static bool header_from_node_db_solution(const struct block_index *bi,
                                         struct block_header *out,
                                         char *out_reason,
                                         size_t out_reason_size)
{
    struct node_db *ndb = fallback_node_db();
    if (!ndb) {
        /* No node.db reachable (e.g. unbooted). The 675,755 rows whose
         * node.db solution is ALSO empty land here too via the loader's
         * false return below. */
        snprintf(out_reason, out_reason_size,
                 "no-header-solution-backfill-required");
        return false;
    }

    unsigned char sol[MAX_SOLUTION_SIZE];
    size_t sol_len = 0;
    if (!db_block_load_solution_by_height(ndb, bi->nHeight,
                                          sol, &sol_len, sizeof(sol))
        || sol_len == 0) {
        /* OWNER-GATED BACKFILL: ~675K of 3.13M node.db rows have an empty
         * solution column. They cannot be validated until a scoped
         * zclassicd LevelDB solution import is run (stop/import/restart) —
         * do NOT attempt that here. Until then this header has NO real,
         * verified solution and MUST fail; never a false pass. */
        snprintf(out_reason, out_reason_size,
                 "no-header-solution-backfill-required");
        return false;
    }
    if (sol_len > sizeof(out->nSolution)) {
        snprintf(out_reason, out_reason_size, "solution-too-large");
        return false;
    }
    if (bi->nHeight > 0 && (!bi->pprev || !bi->pprev->phashBlock)) {
        snprintf(out_reason, out_reason_size, "missing-parent-header");
        return false;
    }

    block_header_init(out);
    out->nVersion = bi->nVersion;
    if (bi->pprev && bi->pprev->phashBlock)
        out->hashPrevBlock = *bi->pprev->phashBlock;
    else
        memset(out->hashPrevBlock.data, 0, sizeof(out->hashPrevBlock.data));
    out->hashMerkleRoot = bi->hashMerkleRoot;
    out->hashFinalSaplingRoot = bi->hashFinalSaplingRoot;
    out->nTime = bi->nTime;
    out->nBits = bi->nBits;
    out->nNonce = bi->nNonce;
    memcpy(out->nSolution, sol, sol_len);
    out->nSolutionSize = sol_len;
    return true;
}

/* ── Default validator: PoW target + Equihash ────────────────────── */

static bool header_from_block_index(const struct block_index *bi,
                                    struct block_header *out,
                                    char *out_reason,
                                    size_t out_reason_size)
{
    if (!bi || !out) {
        snprintf(out_reason, out_reason_size, "null-block-index");
        return false;
    }
    if (!bi->nSolution || bi->nSolutionSize == 0) {
        snprintf(out_reason, out_reason_size, "no-header-solution");
        return false;
    }
    if (bi->nSolutionSize > sizeof(out->nSolution)) {
        snprintf(out_reason, out_reason_size, "solution-too-large");
        return false;
    }
    if (bi->nHeight > 0 && (!bi->pprev || !bi->pprev->phashBlock)) {
        snprintf(out_reason, out_reason_size, "missing-parent-header");
        return false;
    }

    block_header_init(out);
    out->nVersion = bi->nVersion;
    if (bi->pprev && bi->pprev->phashBlock)
        out->hashPrevBlock = *bi->pprev->phashBlock;
    else
        memset(out->hashPrevBlock.data, 0, sizeof(out->hashPrevBlock.data));
    out->hashMerkleRoot = bi->hashMerkleRoot;
    out->hashFinalSaplingRoot = bi->hashFinalSaplingRoot;
    out->nTime = bi->nTime;
    out->nBits = bi->nBits;
    out->nNonce = bi->nNonce;
    memcpy(out->nSolution, bi->nSolution, bi->nSolutionSize);
    out->nSolutionSize = bi->nSolutionSize;
    return true;
}

static bool header_from_disk_block_index(const struct disk_block_index *dbi,
                                         struct block_header *out,
                                         char *out_reason,
                                         size_t out_reason_size)
{
    if (!dbi || !out) {
        snprintf(out_reason, out_reason_size, "null-disk-block-index");
        return false;
    }
    if (dbi->nSolutionSize == 0) {
        snprintf(out_reason, out_reason_size, "no-header-solution");
        return false;
    }
    if (dbi->nSolutionSize > sizeof(out->nSolution)) {
        snprintf(out_reason, out_reason_size, "solution-too-large");
        return false;
    }

    block_header_init(out);
    out->nVersion = dbi->nVersion;
    out->hashPrevBlock = dbi->hashPrev;
    out->hashMerkleRoot = dbi->hashMerkleRoot;
    out->hashFinalSaplingRoot = dbi->hashFinalSaplingRoot;
    out->nTime = dbi->nTime;
    out->nBits = dbi->nBits;
    out->nNonce = dbi->nNonce;
    memcpy(out->nSolution, dbi->nSolution, dbi->nSolutionSize);
    out->nSolutionSize = dbi->nSolutionSize;
    return true;
}

static bool header_from_persisted_block_index(const struct block_index *bi,
                                              struct block_header *out,
                                              char *out_reason,
                                              size_t out_reason_size)
{
    if (!g_active_block_tree || !bi || !bi->phashBlock) {
        snprintf(out_reason, out_reason_size, "no-header-solution");
        return false;
    }

    struct disk_block_index dbi;
    if (!block_tree_db_read_block_index(g_active_block_tree,
                                        bi->phashBlock, &dbi)) {
        snprintf(out_reason, out_reason_size, "no-header-solution");
        return false;
    }
    return header_from_disk_block_index(&dbi, out,
                                        out_reason, out_reason_size);
}

static bool validate_header_fields(const struct block_header *header,
                                   const struct chain_params *cp,
                                   char *out_reason,
                                   size_t out_reason_size)
{
    if (!header || !cp) {
        snprintf(out_reason, out_reason_size, "missing-header-context");
        return false;
    }

    struct uint256 hash;
    block_header_get_hash(header, &hash);
    if (!CheckProofOfWork(hash, header->nBits, &cp->consensus)) {
        snprintf(out_reason, out_reason_size, "high-hash");
        return false;
    }
    if (!check_equihash_solution(header, cp)) {
        snprintf(out_reason, out_reason_size, "invalid-solution");
        return false;
    }
    return true;
}

bool validate_headers_default_validator(const struct block_index *bi,
                                        const char *datadir,
                                        char *out_reason,
                                        size_t out_reason_size,
                                        void *user)
{
    (void)user;
    (void)datadir;
    if (!bi || !bi->phashBlock) {
        snprintf(out_reason, out_reason_size, "null-block-index");
        return false;
    }

    /* (1) version. */
    if (bi->nVersion < MIN_BLOCK_VERSION) {
        snprintf(out_reason, out_reason_size, "version-too-low");
        return false;
    }

    /* (2) PoW target. Cheap — no disk. */
    const struct chain_params *cp = chain_params_get();
    if (!cp) {
        snprintf(out_reason, out_reason_size, "no-chain-params");
        return false;
    }

    /* The block index stores full header fields, including nonce and
     * Equihash solution, for headers admitted through normal P2P/RPC
     * paths. Validate from that header snapshot first. */
    struct block_header index_header;
    if (header_from_block_index(bi, &index_header,
                                out_reason, out_reason_size)) {
        return validate_header_fields(&index_header, cp,
                                      out_reason, out_reason_size);
    }

    /* Restart/load paths deliberately keep the Equihash solution out
     * of the hot in-memory index to save RAM. The persisted block-index
     * record still owns it, so load that compact header record instead
     * of making header validation depend on readable block bodies. */
    if (header_from_persisted_block_index(bi, &index_header,
                                          out_reason, out_reason_size)) {
        return validate_header_fields(&index_header, cp,
                                      out_reason, out_reason_size);
    }

    /* Final source: the node.db blocks.solution BLOB. A cold-imported /
     * near-empty LevelDB leaves both index paths solution-less even though
     * the real, network-anchored Equihash solution lives in SQLite. We
     * validate it IDENTICALLY (same validate_header_fields → CheckProofOfWork
     * + check_equihash_solution). On a node.db that ALSO lacks the solution
     * this still FAILS with "no-header-solution-backfill-required" — never a
     * false pass. */
    if (header_from_node_db_solution(bi, &index_header,
                                     out_reason, out_reason_size)) {
        return validate_header_fields(&index_header, cp,
                                      out_reason, out_reason_size);
    }

    return false;
}
