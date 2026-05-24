/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * accept_block_header — header-only acceptance into the block index.
 *
 * Extracted from process_block_core.c (WS-6 phase 1, file-level split).
 * Pure code motion; function body is byte-identical to its prior site. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "validation/process_block.h"
#include "validation/main_logic.h"
#include "validation/check_block.h"
#include "chain/pow.h"
#include "event/event.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include "process_block_internal.h"

/* Avoid a lib/validation -> app/services include while the C-2 cutover
 * guard still needs the stage mode and parity record check.
 */
typedef enum {
    HEADER_ADMIT_MODE_SHADOW = 0,
    HEADER_ADMIT_MODE_AUTHORITATIVE
} header_admit_mode_t;

extern header_admit_mode_t header_admit_get_mode(void);
extern uint64_t header_admit_stage_cursor(void);
extern bool header_admit_stage_has_record(int32_t height,
                                          const struct uint256 *hash);

typedef enum {
    VALIDATE_HEADERS_MODE_SHADOW = 0,
    VALIDATE_HEADERS_MODE_AUTHORITATIVE
} validate_headers_mode_t;

extern validate_headers_mode_t validate_headers_get_mode(void);
extern uint64_t validate_headers_stage_cursor(void);
extern bool validate_headers_stage_has_pass_record(int32_t height,
                                                   const struct uint256 *hash);

static bool fast_forward_cursor_allows(uint64_t cursor, int32_t height)
{
    sqlite3 *db = progress_store_db();
    if (!db)
        return false;

    int32_t legacy_tip = -1;
    size_t got = 0;
    bool found = false;
    if (!progress_meta_get(db, "legacy_attach_tip_height",
                           &legacy_tip, sizeof(legacy_tip),
                           &got, &found) ||
        !found || got != sizeof(legacy_tip) || legacy_tip < 0)
        return false;

    uint64_t imported_boundary = (uint64_t)legacy_tip + 1u;
    return cursor == imported_boundary &&
           height >= 0 &&
           (uint64_t)height >= imported_boundary;
}

static bool validate_headers_authoritative_guard(
    struct validation_state *state,
    int32_t height,
    const struct uint256 *hash)
{
    if (validate_headers_get_mode() !=
        VALIDATE_HEADERS_MODE_AUTHORITATIVE)
        return true;

    if (fast_forward_cursor_allows(validate_headers_stage_cursor(), height))
        return true;

    if (validate_headers_stage_has_pass_record(height, hash))
        return true;

    char hex[65];
    uint256_get_hex(hash, hex);
    event_emitf(EV_CUTOVER_GUARD_DIVERGED, 0,
                "stage=validate_headers height=%d hash=%s "
                "reason=legacy_expected_valid_header_missing_stage_pass",
                height, hex);
    return validation_state_invalid(state, false, 0,
                                    "validate-headers-cutover-diverged",
                                    NULL);
}

bool accept_block_header(const struct block_header *header,
                         struct validation_state *state,
                         struct main_state *ms,
                         const struct chain_params *params,
                         struct block_index **ppindex)
{
    struct uint256 hash;
    block_header_get_hash(header, &hash);

    struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
    if (pindex) {
        if (ppindex)
            *ppindex = pindex;
        if (uint256_cmp(&hash, &params->consensus.hashGenesisBlock) != 0) {
            struct block_index *header_prev = block_map_find(
                &ms->map_block_index, &header->hashPrevBlock);
            if (!header_prev) {
                return validation_state_invalid(state, false, 0,
                                                "bad-prevblk", NULL);
            }
            int expected_height = header_prev->nHeight + 1;
            int active_h = active_chain_height(&ms->chain_active);
            struct block_index *active_tip =
                active_chain_tip(&ms->chain_active);
            if (active_h > 0 && pindex == active_tip) {
                expected_height = active_h;
                if (header_prev->nHeight != active_h - 1) {
                    header_prev->nHeight = active_h - 1;
                    struct arith_uint256 proof = GetBlockProof(header_prev);
                    if (header_prev->pprev) {
                        arith_uint256_add(&header_prev->nChainWork,
                                          &header_prev->pprev->nChainWork,
                                          &proof);
                    }
                }
            } else if (active_tip && header_prev == active_tip) {
                expected_height = active_h + 1;
            }
            if (pindex->pprev != header_prev ||
                pindex->nHeight != expected_height) {
                pindex->pprev = header_prev;
                pindex->nHeight = expected_height;
                block_index_build_skip(pindex);
                struct arith_uint256 proof = GetBlockProof(pindex);
                arith_uint256_add(&pindex->nChainWork,
                                  &header_prev->nChainWork, &proof);
            }
        }
        /* Fix scrambled heights from LDB import.  After snapshot sync,
         * block_map entries above the coins tip may have nHeight=0 or
         * wrong values because the flat-file/LDB import didn't walk
         * pprev chains for blocks it couldn't fully validate.  Walk UP
         * the pprev chain to find the first correct ancestor, then
         * propagate heights DOWN — same algorithm as boot_index.c.
         * Without this, the getheaders loop stalls forever because
         * pindex_best_header never advances past the wrong height. */
        if (pindex->pprev &&
            pindex->nHeight != pindex->pprev->nHeight + 1) {
            /* Walk up to find first correct ancestor */
            struct block_index *stack[2048];
            int depth = 0;
            struct block_index *cur = pindex;
            /* monotonicity guard. A corrupt pprev cycle
             * would otherwise hold this thread until depth==2048, but
             * also poison every block we push on the stack. Bail clean. */
            while (cur->pprev &&
                   cur->pprev->nHeight < cur->nHeight &&
                   cur->nHeight != cur->pprev->nHeight + 1 &&
                   depth < 2048) {
                stack[depth++] = cur;
                cur = cur->pprev;
            }
            /* Fix cur if needed */
            if (cur->pprev && cur->nHeight != cur->pprev->nHeight + 1) {
                cur->nHeight = cur->pprev->nHeight + 1;
                struct arith_uint256 proof = GetBlockProof(cur);
                arith_uint256_add(&cur->nChainWork,
                                  &cur->pprev->nChainWork, &proof);
            }
            /* Propagate down the stack */
            for (int i = depth - 1; i >= 0; i--) {
                struct block_index *fix = stack[i];
                if (fix->pprev) {
                    fix->nHeight = fix->pprev->nHeight + 1;
                    struct arith_uint256 proof = GetBlockProof(fix);
                    arith_uint256_add(&fix->nChainWork,
                                      &fix->pprev->nChainWork, &proof);
                }
            }
        }
        if ((pindex->nStatus & BLOCK_FAILED_MASK) == BLOCK_FAILED_CHILD)
            pindex->nStatus &= ~BLOCK_FAILED_CHILD;
        /* Re-arriving headers must promote nStatus from
         * BLOCK_VALID_HEADER to BLOCK_VALID_TREE. Without this, blocks
         * stored to block_map by body-pull / direct-import (which
         * leave status at BLOCK_VALID_HEADER + BLOCK_HAVE_DATA) stay
         * invisible to find_most_work_chain forever — that filter
         * requires VALID_TREE. The new-pindex path below does this
         * promotion correctly; the existing-pindex path was silently
         * skipping it, leaving the node wedged with on-disk bodies the
         * chain refuses to connect. pprev_valid here means we've
         * already gone through the "Fix scrambled heights" pass above,
         * so ancestry is linked. */
        if (!validate_headers_authoritative_guard(state, pindex->nHeight,
                                                  &hash))
            return false;

        if ((pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE &&
            !(pindex->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                              BLOCK_VALID_TREE;
        }
        return true;
    }

    if (header_admit_get_mode() == HEADER_ADMIT_MODE_AUTHORITATIVE) {
        struct block_index *pindex_prev = NULL;
        if (uint256_cmp(&hash, &params->consensus.hashGenesisBlock) != 0) {
            pindex_prev = block_map_find(&ms->map_block_index,
                                          &header->hashPrevBlock);
            if (!pindex_prev) {
                return validation_state_invalid(state, false, 0,
                                                "bad-prevblk", NULL);
            }
            if (pindex_prev->nStatus & BLOCK_FAILED_MASK) {
                return validation_state_invalid(state, false, REJECT_INVALID,
                                                "bad-prevblk", NULL);
            }
        }

        int expected_height = pindex_prev ? pindex_prev->nHeight + 1 : 0;
        if (fast_forward_cursor_allows(header_admit_stage_cursor(),
                                       expected_height))
            goto legacy_header_checks;

        if (!header_admit_stage_has_record(expected_height, &hash)) {
            char hex[65];
            uint256_get_hex(&hash, hex);
            event_emitf(EV_CUTOVER_GUARD_DIVERGED, 0,
                        "stage=header_admit height=%d hash=%s "
                        "reason=legacy_expected_admit_missing_stage_record",
                        expected_height, hex);
            return validation_state_invalid(state, false, 0,
                                            "header-admit-cutover-diverged",
                                            NULL);
        }

        char hex[65];
        uint256_get_hex(&hash, hex);
        event_emitf(EV_CUTOVER_GUARD_DIVERGED, 0,
                    "stage=header_admit height=%d hash=%s "
                    "reason=stage_record_without_block_index",
                    expected_height, hex);
        return validation_state_invalid(state, false, 0,
                                        "header-admit-cutover-diverged",
                                        NULL);
    }

legacy_header_checks:
    /* Get prev block index */
    struct block_index *pindex_prev = NULL;
    if (uint256_cmp(&hash, &params->consensus.hashGenesisBlock) != 0) {
        pindex_prev = block_map_find(&ms->map_block_index,
                                      &header->hashPrevBlock);
        if (!pindex_prev) {
            /* Parent not in our block index — this is an orphan block.
             * Normal during sync (blocks arrive before headers).
             * DoS=0: don't penalize the peer for out-of-order delivery. */
            return validation_state_invalid(state, false, 0,
                                            "bad-prevblk", NULL);
        }
        if (pindex_prev->nStatus & BLOCK_FAILED_MASK) {
            /* Don't ban peer — parent may have been marked failed by a
             * prior validation bug (e.g. turnstile false positive).
             * The block is invalid from our perspective, but the peer
             * isn't misbehaving. DoS=0 rejects without penalty. */
            return validation_state_invalid(state, false, REJECT_INVALID,
                                            "bad-prevblk", NULL);
        }
    }

    bool validate_headers_authoritative =
        validate_headers_get_mode() == VALIDATE_HEADERS_MODE_AUTHORITATIVE;
    if (validate_headers_authoritative) {
        int32_t expected_height = pindex_prev ? pindex_prev->nHeight + 1 : 0;
        if (!validate_headers_authoritative_guard(state, expected_height,
                                                  &hash))
            return false;
    } else if (!check_block_header(header, state, params, true)) {
        LOG_FAIL("validation", "check_block_header failed for accepted header");
    }

    /* Fix pindex_prev height if scrambled (same logic as the already-known
     * path above).  After snapshot sync + LDB import, block_map entries
     * may have nHeight=0 or wrong values because pprev chains weren't
     * fully resolved.  Without this fix, contextual_check_block_header
     * applies rules for the WRONG height (e.g. pre-Sapling equihash size
     * check at computed height 2 for a block really at height 2M+). */
    if (pindex_prev && pindex_prev->pprev &&
        pindex_prev->nHeight != pindex_prev->pprev->nHeight + 1) {
        struct block_index *stack[2048];
        int depth = 0;
        struct block_index *cur = pindex_prev;
        /* monotonicity guard (see same site at L1575). */
        while (cur->pprev &&
               cur->pprev->nHeight < cur->nHeight &&
               cur->nHeight != cur->pprev->nHeight + 1 &&
               depth < 2048) {
            stack[depth++] = cur;
            cur = cur->pprev;
        }
        if (cur->pprev && cur->nHeight != cur->pprev->nHeight + 1) {
            cur->nHeight = cur->pprev->nHeight + 1;
            struct arith_uint256 proof = GetBlockProof(cur);
            arith_uint256_add(&cur->nChainWork,
                              &cur->pprev->nChainWork, &proof);
        }
        for (int i = depth - 1; i >= 0; i--) {
            struct block_index *fix = stack[i];
            if (fix->pprev) {
                fix->nHeight = fix->pprev->nHeight + 1;
                struct arith_uint256 proof = GetBlockProof(fix);
                arith_uint256_add(&fix->nChainWork,
                                  &fix->pprev->nChainWork, &proof);
            }
        }
    }

    /* Skip contextual header check during IBD when the block index has
     * scrambled heights from snapshot/LDB import, OR when the post-
     * FlyClient-snapshot tail leaves the PoW averaging window unable to
     * walk back contiguously. In either case, the contextual
     * check would spuriously fail; full validation happens later in
     * connect_block(). This mirrors Bitcoin Core's header-first sync
     * model, where header structure is checked before block connection. */
    if (pindex_prev &&
        !process_block_should_skip_contextual_header(ms, pindex_prev,
                                                     &params->consensus) &&
        !contextual_check_block_header(header, state, params, pindex_prev,
                                        ms->fCheckpointsEnabled))
        LOG_FAIL("validation", "contextual_check_block_header failed for header at height %d",
                 pindex_prev->nHeight + 1);

    pindex = add_to_block_index(ms, header);
    if (!pindex)
        return validation_state_error(state, "add-to-block-index-failed");

    if ((pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE)
        pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) |
                           BLOCK_VALID_TREE;

    if (ppindex)
        *ppindex = pindex;

    return true;
}
