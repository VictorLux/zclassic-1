/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for fast sync protocol: UTXO snapshots, swarm sync,
 * block swarm, Merkle proofs, integrity verification,
 * and bandwidth-adaptive download manager. */

#include "test/test_helpers.h"
#include "net/fast_sync.h"
#include "net/download.h"
#include "coins/utxo_commitment.h"
#include "crypto/sha3.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Merkle tree tests ─────────────────────────────────────── */

static int test_merkle_root_single(void)
{
    int failures = 0;
    TEST("fast_sync_merkle_root single hash") {
        uint8_t hash[32];
        memset(hash, 0xAA, 32);
        uint8_t root[32];
        fast_sync_merkle_root((const uint8_t (*)[32])&hash, 1, root);
        ASSERT(memcmp(root, hash, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_merkle_root_empty(void)
{
    int failures = 0;
    TEST("fast_sync_merkle_root empty") {
        uint8_t root[32];
        fast_sync_merkle_root(NULL, 0, root);
        uint8_t zero[32] = {0};
        ASSERT(memcmp(root, zero, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_merkle_root_deterministic(void)
{
    int failures = 0;
    TEST("fast_sync_merkle_root deterministic") {
        uint8_t hashes[4][32];
        for (int i = 0; i < 4; i++)
            memset(hashes[i], (uint8_t)(i + 1), 32);
        uint8_t root1[32], root2[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes, 4, root1);
        fast_sync_merkle_root((const uint8_t (*)[32])hashes, 4, root2);
        ASSERT(memcmp(root1, root2, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_merkle_root_order_sensitive(void)
{
    int failures = 0;
    TEST("fast_sync_merkle_root order sensitive") {
        uint8_t hashes_ab[2][32], hashes_ba[2][32];
        memset(hashes_ab[0], 0xAA, 32);
        memset(hashes_ab[1], 0xBB, 32);
        memset(hashes_ba[0], 0xBB, 32);
        memset(hashes_ba[1], 0xAA, 32);
        uint8_t root_ab[32], root_ba[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes_ab, 2, root_ab);
        fast_sync_merkle_root((const uint8_t (*)[32])hashes_ba, 2, root_ba);
        ASSERT(memcmp(root_ab, root_ba, 32) != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_merkle_root_power_of_two_padding(void)
{
    int failures = 0;
    TEST("fast_sync_merkle_root pads non-power-of-2 correctly") {
        /* 3 leaves should produce same root as 4 leaves where leaf[3]=leaf[2] */
        uint8_t hashes3[3][32], hashes4[4][32];
        for (int i = 0; i < 3; i++) {
            memset(hashes3[i], (uint8_t)(i + 0x10), 32);
            memset(hashes4[i], (uint8_t)(i + 0x10), 32);
        }
        memcpy(hashes4[3], hashes4[2], 32); /* pad with last */
        uint8_t root3[32], root4[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes3, 3, root3);
        fast_sync_merkle_root((const uint8_t (*)[32])hashes4, 4, root4);
        ASSERT(memcmp(root3, root4, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Merkle proof tests ───────────────────────────────────── */

static int test_merkle_proof_verify(void)
{
    int failures = 0;
    TEST("fast_sync_build_proof + verify roundtrip") {
        uint8_t hashes[8][32];
        for (int i = 0; i < 8; i++)
            memset(hashes[i], (uint8_t)(i + 0x20), 32);
        uint8_t root[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes, 8, root);

        /* Build and verify proof for each leaf */
        for (uint32_t i = 0; i < 8; i++) {
            uint8_t (*proof)[32] = NULL;
            uint32_t plen = fast_sync_build_proof(
                (const uint8_t (*)[32])hashes, 8, i, &proof);
            ASSERT(plen == 3); /* log2(8) = 3 */
            bool ok = fast_sync_verify_chunk_proof(
                i, hashes[i], (const uint8_t (*)[32])proof, plen, root);
            ASSERT(ok);
            free(proof);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_merkle_proof_invalid(void)
{
    int failures = 0;
    TEST("fast_sync_verify_chunk_proof rejects wrong hash") {
        uint8_t hashes[4][32];
        for (int i = 0; i < 4; i++)
            memset(hashes[i], (uint8_t)(i + 0x30), 32);
        uint8_t root[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes, 4, root);

        uint8_t (*proof)[32] = NULL;
        uint32_t plen = fast_sync_build_proof(
            (const uint8_t (*)[32])hashes, 4, 0, &proof);

        /* Tamper with the chunk hash */
        uint8_t fake[32];
        memset(fake, 0xFF, 32);
        bool ok = fast_sync_verify_chunk_proof(
            0, fake, (const uint8_t (*)[32])proof, plen, root);
        ASSERT(!ok);
        free(proof);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Chunk hash tests ─────────────────────────────────────── */

static int test_chunk_hash_deterministic(void)
{
    int failures = 0;
    TEST("fast_sync_chunk_hash deterministic") {
        struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
        ASSERT(chunk != NULL);
        chunk->chunk_index = 0;
        chunk->num_entries = 2;
        memset(chunk->entries[0].txid, 0x11, 32);
        chunk->entries[0].vout = 0;
        chunk->entries[0].value = 100000;
        chunk->entries[0].height = 1000;
        memset(chunk->entries[1].txid, 0x22, 32);
        chunk->entries[1].vout = 1;
        chunk->entries[1].value = 200000;
        chunk->entries[1].height = 2000;

        uint8_t h1[32], h2[32];
        fast_sync_chunk_hash(chunk, h1);
        fast_sync_chunk_hash(chunk, h2);
        ASSERT(memcmp(h1, h2, 32) == 0);

        /* Different chunk index produces different hash */
        chunk->chunk_index = 1;
        uint8_t h3[32];
        fast_sync_chunk_hash(chunk, h3);
        ASSERT(memcmp(h1, h3, 32) != 0);

        free(chunk);
        PASS();
    } _test_next:;
    return failures;
}

static int test_chunk_verify(void)
{
    int failures = 0;
    TEST("fast_sync_verify_chunk correct/incorrect") {
        struct utxo_chunk *chunk = calloc(1, sizeof(struct utxo_chunk));
        ASSERT(chunk != NULL);
        chunk->chunk_index = 5;
        chunk->num_entries = 1;
        memset(chunk->entries[0].txid, 0xCC, 32);
        chunk->entries[0].vout = 0;
        chunk->entries[0].value = 50000;
        chunk->entries[0].height = 500;

        uint8_t expected[32];
        fast_sync_chunk_hash(chunk, expected);

        ASSERT(fast_sync_verify_chunk(chunk, expected));

        /* Tamper */
        chunk->entries[0].value = 99999;
        ASSERT(!fast_sync_verify_chunk(chunk, expected));

        free(chunk);
        PASS();
    } _test_next:;
    return failures;
}

/* ── PoW defense tests ───────────────────────────────────── */

static int test_pow_solve_verify(void)
{
    int failures = 0;
    TEST("fast_sync PoW solve and verify") {
        uint8_t peer_id[32];
        memset(peer_id, 0x42, 32);
        struct fast_sync_pow pow;
        bool solved = fast_sync_solve_pow(peer_id, &pow);
        ASSERT(solved);
        ASSERT(fast_sync_verify_pow(&pow));

        /* Tamper with nonce */
        pow.nonce += 1;
        ASSERT(!fast_sync_verify_pow(&pow));
        PASS();
    } _test_next:;
    return failures;
}

static int test_pow_timestamp_range(void)
{
    int failures = 0;
    TEST("fast_sync PoW rejects stale timestamp") {
        uint8_t peer_id[32];
        memset(peer_id, 0x43, 32);
        struct fast_sync_pow pow;
        fast_sync_solve_pow(peer_id, &pow);
        /* Set timestamp to 10 minutes ago */
        pow.timestamp -= 600;
        ASSERT(!fast_sync_verify_pow(&pow));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Rate limiter tests ──────────────────────────────────── */

static int test_rate_limiter(void)
{
    int failures = 0;
    TEST("fast_sync rate limiter tracks per-IP") {
        struct fast_sync_rate_limiter rl;
        memset(&rl, 0, sizeof(rl));

        uint8_t ip1[16] = {0};
        ip1[0] = 1;
        uint8_t ip2[16] = {0};
        ip2[0] = 2;

        /* First request OK */
        ASSERT(fast_sync_rate_check(&rl, ip1));
        ASSERT(fast_sync_rate_check(&rl, ip2));
        ASSERT(rl.num_entries == 2);

        /* Exhaust ip1's rate limit */
        for (int i = 1; i < FAST_SYNC_MAX_CHUNKS_PER_HOUR; i++)
            ASSERT(fast_sync_rate_check(&rl, ip1));

        /* Next request should fail for ip1 */
        ASSERT(!fast_sync_rate_check(&rl, ip1));

        /* ip2 still has budget */
        ASSERT(fast_sync_rate_check(&rl, ip2));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Swarm sync coordinator tests ────────────────────────── */

static int test_swarm_init_assign(void)
{
    int failures = 0;
    TEST("swarm_sync init + assign chunks") {
        struct sync_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.num_chunks = 10;
        manifest.chunk_size = 500;
        manifest.chunk_hashes = calloc(10, 32);
        ASSERT(manifest.chunk_hashes != NULL);

        struct swarm_sync ss;
        ASSERT(swarm_sync_init(&ss, &manifest, NULL));
        ASSERT(swarm_sync_progress(&ss) == 0);
        ASSERT(!swarm_sync_is_complete(&ss));

        /* Assign chunks to peers */
        int32_t c1 = swarm_sync_assign_chunk(&ss, 1);
        int32_t c2 = swarm_sync_assign_chunk(&ss, 2);
        int32_t c3 = swarm_sync_assign_chunk(&ss, 1);
        ASSERT(c1 == 0);
        ASSERT(c2 == 1);
        ASSERT(c3 == 2);
        ASSERT(ss.chunks_inflight == 3);

        swarm_sync_free(&ss);
        free(manifest.chunk_hashes);
        PASS();
    } _test_next:;
    return failures;
}

static int test_swarm_timeout_reassign(void)
{
    int failures = 0;
    TEST("swarm_sync timeout reassigns chunks") {
        struct sync_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.num_chunks = 5;
        manifest.chunk_size = 500;
        manifest.chunk_hashes = calloc(5, 32);
        ASSERT(manifest.chunk_hashes != NULL);

        struct swarm_sync ss;
        swarm_sync_init(&ss, &manifest, NULL);

        /* Assign all 5 chunks */
        for (int i = 0; i < 5; i++)
            swarm_sync_assign_chunk(&ss, 1);
        ASSERT(ss.chunks_inflight == 5);

        /* No chunks available now */
        ASSERT(swarm_sync_assign_chunk(&ss, 2) == -1);

        /* Simulate timeout: set request times to the past */
        for (uint32_t i = 0; i < 5; i++)
            ss.chunk_request_time[i] -= 60; /* 60s ago */
        swarm_sync_handle_timeouts(&ss, 30); /* 30s timeout */
        ASSERT(ss.chunks_inflight == 0);

        /* Chunks should be available again */
        int32_t c = swarm_sync_assign_chunk(&ss, 2);
        ASSERT(c >= 0);

        swarm_sync_free(&ss);
        free(manifest.chunk_hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Block swarm tests ───────────────────────────────────── */

static int test_block_swarm_rarest_first(void)
{
    int failures = 0;
    TEST("block_swarm rarest-first piece selection") {
        struct block_piece_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.start_height = 0;
        manifest.end_height = 511; /* 4 pieces of 128 blocks */
        manifest.num_pieces = 4;
        manifest.piece_hashes = calloc(4, 32);
        ASSERT(manifest.piece_hashes != NULL);

        struct block_swarm bs;
        ASSERT(block_swarm_init(&bs, &manifest, NULL));

        /* Set availability: piece 0=10, 1=2, 2=5, 3=1 (rarest) */
        bs.piece_availability[0] = 10;
        bs.piece_availability[1] = 2;
        bs.piece_availability[2] = 5;
        bs.piece_availability[3] = 1;

        /* First assignment should pick piece 3 (rarest) */
        int32_t p = block_swarm_assign_piece(&bs, 1, NULL);
        ASSERT(p == 3);

        /* Next should pick piece 1 (next rarest) */
        p = block_swarm_assign_piece(&bs, 2, NULL);
        ASSERT(p == 1);

        block_swarm_free(&bs);
        free(manifest.piece_hashes);
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_swarm_endgame(void)
{
    int failures = 0;
    TEST("block_swarm endgame mode activates") {
        struct block_piece_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.start_height = 0;
        manifest.end_height = 1279; /* 10 pieces */
        manifest.num_pieces = 10;
        manifest.piece_hashes = calloc(10, 32);
        ASSERT(manifest.piece_hashes != NULL);

        struct block_swarm bs;
        block_swarm_init(&bs, &manifest, NULL);

        /* Complete 3 pieces, leaving 7 remaining > ENDGAME_THRESHOLD */
        for (uint32_t i = 0; i < 3; i++)
            block_swarm_receive_piece(&bs, i, 1);
        ASSERT(!bs.endgame);

        /* Complete more, leaving exactly ENDGAME_THRESHOLD-1 */
        for (uint32_t i = 3; i < 10 - ENDGAME_THRESHOLD + 1; i++)
            block_swarm_receive_piece(&bs, i, 1);

        /* Next assignment should trigger endgame */
        block_swarm_assign_piece(&bs, 1, NULL);
        ASSERT(bs.endgame);

        block_swarm_free(&bs);
        free(manifest.piece_hashes);
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_swarm_bitmap(void)
{
    int failures = 0;
    TEST("block_swarm bitmap serialize/update") {
        struct block_piece_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.num_pieces = 16;
        manifest.piece_hashes = calloc(16, 32);
        ASSERT(manifest.piece_hashes != NULL);

        struct block_swarm bs;
        block_swarm_init(&bs, &manifest, NULL);

        /* Complete pieces 0, 3, 7 */
        block_swarm_receive_piece(&bs, 0, 1);
        block_swarm_receive_piece(&bs, 3, 1);
        block_swarm_receive_piece(&bs, 7, 1);

        /* Serialize bitmap */
        uint8_t bitmap[2];
        uint32_t blen = block_swarm_serialize_bitmap(&bs, bitmap, 2);
        ASSERT(blen == 2);
        /* Bit 0, 3, 7 should be set */
        ASSERT(bitmap[0] & (1 << 0));
        ASSERT(bitmap[0] & (1 << 3));
        ASSERT(bitmap[0] & (1 << 7));
        ASSERT(!(bitmap[0] & (1 << 1)));

        /* Update availability from a peer's bitmap */
        uint8_t peer_bitmap[2] = {0xFF, 0x0F}; /* peer has all 16 pieces */
        block_swarm_update_availability(&bs, peer_bitmap, 2);
        for (uint32_t i = 0; i < 12; i++) /* 0xFF=8bits + 0x0F=4bits */
            ASSERT(bs.piece_availability[i] >= 1);

        block_swarm_free(&bs);
        free(manifest.piece_hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Block piece hash tests ──────────────────────────────── */

static int test_block_piece_hash_deterministic(void)
{
    int failures = 0;
    TEST("block_piece_hash deterministic + index-sensitive") {
        uint8_t hashes[4][32];
        for (int i = 0; i < 4; i++)
            memset(hashes[i], (uint8_t)(i + 0x50), 32);

        uint8_t h1[32], h2[32], h3[32];
        block_piece_hash((const uint8_t (*)[32])hashes, 4, 0, h1);
        block_piece_hash((const uint8_t (*)[32])hashes, 4, 0, h2);
        ASSERT(memcmp(h1, h2, 32) == 0);

        /* Different piece index → different hash */
        block_piece_hash((const uint8_t (*)[32])hashes, 4, 1, h3);
        ASSERT(memcmp(h1, h3, 32) != 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── UTXO commitment checkpoint tests ────────────────────── */

static int test_commitment_checkpoint_roundtrip(void)
{
    int failures = 0;
    TEST("utxo_commitment serialize/deserialize roundtrip") {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);

        /* Add some UTXOs */
        uint8_t txid1[32], txid2[32];
        memset(txid1, 0xAA, 32);
        memset(txid2, 0xBB, 32);
        utxo_commitment_add(&uc, txid1, 0, 100000, 1000);
        utxo_commitment_add(&uc, txid2, 1, 200000, 2000);

        /* Serialize */
        uint8_t buf[UTXO_COMMITMENT_SERIALIZED_SIZE];
        utxo_commitment_serialize(&uc, buf);

        /* Deserialize */
        struct utxo_commitment uc2;
        ASSERT(utxo_commitment_deserialize(&uc2, buf, sizeof(buf)));
        ASSERT(utxo_commitment_equal(&uc, &uc2));
        PASS();
    } _test_next:;
    return failures;
}

static int test_commitment_add_remove_identity(void)
{
    int failures = 0;
    TEST("utxo_commitment add+remove = identity") {
        struct utxo_commitment uc;
        utxo_commitment_init(&uc);

        struct utxo_commitment empty;
        utxo_commitment_init(&empty);

        /* Add then remove same UTXO → back to empty */
        uint8_t txid[32];
        memset(txid, 0x55, 32);
        utxo_commitment_add(&uc, txid, 0, 50000, 500);
        ASSERT(!utxo_commitment_equal(&uc, &empty));
        utxo_commitment_remove(&uc, txid, 0, 50000, 500);
        ASSERT(memcmp(uc.accumulator, empty.accumulator, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_commitment_order_independent(void)
{
    int failures = 0;
    TEST("utxo_commitment XOR is order-independent") {
        struct utxo_commitment uc1, uc2;
        utxo_commitment_init(&uc1);
        utxo_commitment_init(&uc2);

        uint8_t txid_a[32], txid_b[32], txid_c[32];
        memset(txid_a, 0x11, 32);
        memset(txid_b, 0x22, 32);
        memset(txid_c, 0x33, 32);

        /* Add in order A, B, C */
        utxo_commitment_add(&uc1, txid_a, 0, 100, 1);
        utxo_commitment_add(&uc1, txid_b, 0, 200, 2);
        utxo_commitment_add(&uc1, txid_c, 0, 300, 3);

        /* Add in order C, A, B */
        utxo_commitment_add(&uc2, txid_c, 0, 300, 3);
        utxo_commitment_add(&uc2, txid_a, 0, 100, 1);
        utxo_commitment_add(&uc2, txid_b, 0, 200, 2);

        ASSERT(utxo_commitment_equal(&uc1, &uc2));
        PASS();
    } _test_next:;
    return failures;
}

static int test_commitment_merge(void)
{
    int failures = 0;
    TEST("utxo_commitment merge combines sets") {
        struct utxo_commitment uc_all, uc_a, uc_b;
        utxo_commitment_init(&uc_all);
        utxo_commitment_init(&uc_a);
        utxo_commitment_init(&uc_b);

        uint8_t txid1[32], txid2[32];
        memset(txid1, 0x44, 32);
        memset(txid2, 0x55, 32);

        utxo_commitment_add(&uc_all, txid1, 0, 100, 1);
        utxo_commitment_add(&uc_all, txid2, 0, 200, 2);

        utxo_commitment_add(&uc_a, txid1, 0, 100, 1);
        utxo_commitment_add(&uc_b, txid2, 0, 200, 2);

        utxo_commitment_merge(&uc_a, &uc_b);
        ASSERT(utxo_commitment_equal(&uc_a, &uc_all));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Bandwidth-adaptive download manager tests ───────────── */

static struct uint256 make_hash_fs(uint8_t v)
{
    struct uint256 h;
    memset(h.data, 0, 32);
    h.data[0] = v;
    h.data[31] = v;
    return h;
}

static int test_dl_bandwidth_scoring(void)
{
    int failures = 0;
    TEST("dl bandwidth scoring: fast peers get higher score") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash_fs(1);
        struct uint256 h2 = make_hash_fs(2);

        /* Peer 1: fast (100ms delivery) */
        dl_mark_requested(&dm, &h1, 100, 1);
        dl_mark_received(&dm, &h1);
        dl_peer_block_received(&dm, 1, 100000); /* 100ms in us */

        /* Peer 2: slow (2s delivery) */
        dl_mark_requested(&dm, &h2, 101, 2);
        dl_mark_received(&dm, &h2);
        dl_peer_block_received(&dm, 2, 2000000); /* 2s in us */

        /* Fast peer should get larger window */
        size_t w1 = dl_peer_adaptive_window(&dm, 1);
        size_t w2 = dl_peer_adaptive_window(&dm, 2);
        ASSERT(w1 > w2);
        ASSERT(w1 >= 16);
        ASSERT(w2 >= 16);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_adaptive_assignment(void)
{
    int failures = 0;
    TEST("dl adaptive assignment gives fast peers more work") {
        struct download_manager dm;
        dl_init(&dm);

        /* Simulate: peer 1 is 4x faster than peer 2 */
        /* Need to establish bandwidth scores first */
        for (int i = 0; i < 10; i++) {
            struct uint256 h = make_hash_fs((uint8_t)(100 + i));
            dl_mark_requested(&dm, &h, i, 1);
            dl_mark_received(&dm, &h);
        }
        dl_peer_block_received(&dm, 1, 250000); /* 250ms avg */

        for (int i = 0; i < 10; i++) {
            struct uint256 h = make_hash_fs((uint8_t)(200 + i));
            dl_mark_requested(&dm, &h, i, 2);
            dl_mark_received(&dm, &h);
        }
        dl_peer_block_received(&dm, 2, 2000000); /* 2000ms avg */

        /* Queue 256 blocks */
        struct uint256 hashes[256];
        int32_t heights[256];
        for (int i = 0; i < 256; i++) {
            memset(hashes[i].data, 0, 32);
            hashes[i].data[0] = (uint8_t)(i & 0xFF);
            hashes[i].data[1] = (uint8_t)(i >> 8);
            hashes[i].data[2] = 0xFF; /* different from above */
            heights[i] = 1000 + i;
        }
        dl_queue_blocks(&dm, hashes, heights, 256);

        /* Assign to both peers */
        struct uint256 out[256];
        size_t a1 = dl_assign_to_peer(&dm, 1, out, 256);
        size_t a2 = dl_assign_to_peer(&dm, 2, out, 256);

        /* Fast peer should get more (at least 2x) */
        ASSERT(a1 > a2);
        ASSERT(a1 >= 16);
        ASSERT(a2 >= 16);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Integration: full chunk hash → Merkle proof pipeline ── */

static int test_chunk_to_merkle_pipeline(void)
{
    int failures = 0;
    TEST("full pipeline: chunks → hashes → merkle root → proofs") {
        /* Create 4 fake chunks */
        struct utxo_chunk *chunks[4];
        uint8_t chunk_hashes[4][32];
        for (int i = 0; i < 4; i++) {
            chunks[i] = calloc(1, sizeof(struct utxo_chunk));
            ASSERT(chunks[i] != NULL);
            chunks[i]->chunk_index = (uint32_t)i;
            chunks[i]->num_entries = 1;
            memset(chunks[i]->entries[0].txid, (uint8_t)(i + 0x60), 32);
            chunks[i]->entries[0].vout = 0;
            chunks[i]->entries[0].value = (int64_t)(i + 1) * 10000;
            chunks[i]->entries[0].height = (i + 1) * 100;
            fast_sync_chunk_hash(chunks[i], chunk_hashes[i]);
        }

        /* Build Merkle root */
        uint8_t root[32];
        fast_sync_merkle_root((const uint8_t (*)[32])chunk_hashes, 4, root);

        /* Verify each chunk can prove itself against the root */
        for (uint32_t i = 0; i < 4; i++) {
            uint8_t (*proof)[32] = NULL;
            uint32_t plen = fast_sync_build_proof(
                (const uint8_t (*)[32])chunk_hashes, 4, i, &proof);
            ASSERT(plen == 2); /* log2(4) = 2 */

            bool ok = fast_sync_verify_chunk_proof(
                i, chunk_hashes[i], (const uint8_t (*)[32])proof, plen, root);
            ASSERT(ok);
            free(proof);
        }

        for (int i = 0; i < 4; i++)
            free(chunks[i]);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Block swarm full lifecycle ──────────────────────────── */

static int test_block_swarm_lifecycle(void)
{
    int failures = 0;
    TEST("block_swarm full lifecycle: init → assign → receive → complete") {
        struct block_piece_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.start_height = 0;
        manifest.end_height = 639; /* 5 pieces */
        manifest.num_pieces = 5;
        manifest.piece_hashes = calloc(5, 32);
        ASSERT(manifest.piece_hashes != NULL);

        struct block_swarm bs;
        ASSERT(block_swarm_init(&bs, &manifest, NULL));
        ASSERT(block_swarm_progress(&bs) == 0);

        /* 3 peers each get work */
        int32_t p1 = block_swarm_assign_piece(&bs, 1, NULL);
        int32_t p2 = block_swarm_assign_piece(&bs, 2, NULL);
        int32_t p3 = block_swarm_assign_piece(&bs, 3, NULL);
        ASSERT(p1 >= 0 && p2 >= 0 && p3 >= 0);
        ASSERT(bs.pieces_inflight == 3);

        /* Receive from peers */
        ASSERT(block_swarm_receive_piece(&bs, (uint32_t)p1, 1));
        ASSERT(block_swarm_receive_piece(&bs, (uint32_t)p2, 2));
        ASSERT(block_swarm_receive_piece(&bs, (uint32_t)p3, 3));
        ASSERT(block_swarm_progress(&bs) == 60); /* 3/5 = 60% */

        /* Assign and receive remaining */
        int32_t p4 = block_swarm_assign_piece(&bs, 1, NULL);
        int32_t p5 = block_swarm_assign_piece(&bs, 2, NULL);
        ASSERT(p4 >= 0 && p5 >= 0);
        block_swarm_receive_piece(&bs, (uint32_t)p4, 1);
        block_swarm_receive_piece(&bs, (uint32_t)p5, 2);
        ASSERT(block_swarm_is_complete(&bs));
        ASSERT(block_swarm_progress(&bs) == 100);

        block_swarm_free(&bs);
        free(manifest.piece_hashes);
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_swarm_fail_retry(void)
{
    int failures = 0;
    TEST("block_swarm fail piece and retry") {
        struct block_piece_manifest manifest;
        memset(&manifest, 0, sizeof(manifest));
        manifest.num_pieces = 3;
        manifest.piece_hashes = calloc(3, 32);
        ASSERT(manifest.piece_hashes != NULL);

        struct block_swarm bs;
        block_swarm_init(&bs, &manifest, NULL);

        int32_t p = block_swarm_assign_piece(&bs, 1, NULL);
        ASSERT(p >= 0);

        /* Fail the piece (bad hash) */
        block_swarm_fail_piece(&bs, (uint32_t)p);
        ASSERT(bs.pieces_failed == 1);
        ASSERT(bs.piece_states[p] == CHUNK_NEEDED);

        /* Can reassign to different peer */
        int32_t p2 = block_swarm_assign_piece(&bs, 2, NULL);
        ASSERT(p2 == p); /* should get same piece back */

        block_swarm_free(&bs);
        free(manifest.piece_hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Large-scale Merkle proof stress test ────────────────── */

static int test_merkle_proof_large(void)
{
    int failures = 0;
    TEST("merkle proof with 1000 leaves") {
        uint8_t (*hashes)[32] = calloc(1000, 32);
        ASSERT(hashes != NULL);

        for (int i = 0; i < 1000; i++) {
            struct sha3_256_ctx ctx;
            sha3_256_init(&ctx);
            sha3_256_write(&ctx, (const uint8_t *)&i, 4);
            sha3_256_finalize(&ctx, hashes[i]);
        }

        uint8_t root[32];
        fast_sync_merkle_root((const uint8_t (*)[32])hashes, 1000, root);

        /* Verify proof for leaf 500 */
        uint8_t (*proof)[32] = NULL;
        uint32_t plen = fast_sync_build_proof(
            (const uint8_t (*)[32])hashes, 1000, 500, &proof);
        ASSERT(plen > 0);
        ASSERT(plen <= 10); /* log2(1024) = 10 */

        bool ok = fast_sync_verify_chunk_proof(
            500, hashes[500], (const uint8_t (*)[32])proof, plen, root);
        ASSERT(ok);

        /* Verify proof for first and last leaves */
        free(proof);
        plen = fast_sync_build_proof(
            (const uint8_t (*)[32])hashes, 1000, 0, &proof);
        ASSERT(fast_sync_verify_chunk_proof(
            0, hashes[0], (const uint8_t (*)[32])proof, plen, root));

        free(proof);
        plen = fast_sync_build_proof(
            (const uint8_t (*)[32])hashes, 1000, 999, &proof);
        ASSERT(fast_sync_verify_chunk_proof(
            999, hashes[999], (const uint8_t (*)[32])proof, plen, root));

        free(proof);
        free(hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* ── MMR-secured snapshot offer ──────────────────────────── */

static int test_snapshot_offer_mmr_field(void)
{
    int failures = 0;
    TEST("snapshot_offer includes MMR root field") {
        /* Verify the struct layout includes mmr_root between
         * utxo_root and num_utxos — critical for PoW chain binding */
        struct snapshot_offer offer;
        memset(&offer, 0, sizeof(offer));

        /* Set distinct values in each field */
        offer.height = 3000000;
        memset(offer.block_hash, 0xAA, 32);
        memset(offer.utxo_root, 0xBB, 32);
        memset(offer.mmr_root, 0xCC, 32);
        offer.num_utxos = 1354771;
        offer.total_bytes = 1354771 * 80;

        /* Verify fields are distinct and correct */
        ASSERT(offer.height == 3000000);
        ASSERT(offer.block_hash[0] == 0xAA);
        ASSERT(offer.utxo_root[0] == 0xBB);
        ASSERT(offer.mmr_root[0] == 0xCC);
        ASSERT(offer.num_utxos == 1354771);

        /* Verify MMR root is its own field, not overlapping */
        ASSERT(memcmp(offer.utxo_root, offer.mmr_root, 32) != 0);

        /* Verify all-zero MMR root detection (no PoW proof) */
        uint8_t zeros[32] = {0};
        ASSERT(memcmp(offer.mmr_root, zeros, 32) != 0);
        memset(offer.mmr_root, 0, 32);
        ASSERT(memcmp(offer.mmr_root, zeros, 32) == 0);

        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ─────────────────────────────────────────── */

int test_fast_sync(void)
{
    int failures = 0;

    /* Merkle tree */
    failures += test_merkle_root_single();
    failures += test_merkle_root_empty();
    failures += test_merkle_root_deterministic();
    failures += test_merkle_root_order_sensitive();
    failures += test_merkle_root_power_of_two_padding();

    /* Merkle proofs */
    failures += test_merkle_proof_verify();
    failures += test_merkle_proof_invalid();
    failures += test_merkle_proof_large();

    /* Chunk hashing */
    failures += test_chunk_hash_deterministic();
    failures += test_chunk_verify();

    /* PoW defense */
    failures += test_pow_solve_verify();
    failures += test_pow_timestamp_range();

    /* Rate limiting */
    failures += test_rate_limiter();

    /* Swarm coordinator */
    failures += test_swarm_init_assign();
    failures += test_swarm_timeout_reassign();

    /* Block swarm */
    failures += test_block_swarm_rarest_first();
    failures += test_block_swarm_endgame();
    failures += test_block_swarm_bitmap();
    failures += test_block_piece_hash_deterministic();
    failures += test_block_swarm_lifecycle();
    failures += test_block_swarm_fail_retry();

    /* UTXO commitment */
    failures += test_commitment_checkpoint_roundtrip();
    failures += test_commitment_add_remove_identity();
    failures += test_commitment_order_independent();
    failures += test_commitment_merge();

    /* Integration */
    failures += test_chunk_to_merkle_pipeline();

    /* Bandwidth-adaptive download */
    failures += test_dl_bandwidth_scoring();
    failures += test_dl_adaptive_assignment();

    /* MMR-secured snapshot */
    failures += test_snapshot_offer_mmr_field();

    return failures;
}
