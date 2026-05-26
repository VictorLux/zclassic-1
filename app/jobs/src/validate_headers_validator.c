/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_validator — default header validator for the
 * validate_headers Job. Extracted verbatim from validate_headers_stage.c
 * (pure refactor, behaviour byte-identical). Reconstructs the block
 * header from the in-memory or persisted block index and checks PoW
 * target + Equihash solution. No cursor logic lives here. */

#include "validate_headers_internal.h"

#include "chain/chainparams.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "storage/block_index_db.h"
#include "validation/check_block.h"

#include <stdio.h>
#include <string.h>

extern struct block_tree_db *g_active_block_tree;

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

    return false;
}
