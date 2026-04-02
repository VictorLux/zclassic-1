/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for snapshot sync service policy helpers. */

#include "test/test_helpers.h"
#include "services/snapshot_sync_service.h"
#include "core/serialize.h"
#include "net/fast_sync.h"
#include "net/net.h"
#include "validation/main_state.h"
#include <string.h>

static int test_snapshot_sync_service_followups(void)
{
    int failures = 0;

    TEST("snapshot sync service followup action tracks verification state") {
        struct snapshot_sync_service svc;
        memset(&svc, 0, sizeof(svc));

        svc.fc_verified = false;
        ASSERT(snapsync_offer_followup_action(&svc) ==
               SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE);

        svc.fc_verified = true;
        ASSERT(snapsync_offer_followup_action(&svc) ==
               SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ);

        ASSERT(snapsync_verify_followup_action(false) ==
               SNAPSYNC_FOLLOWUP_NONE);
        ASSERT(snapsync_verify_followup_action(true) ==
               SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ);
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_builds_pow(void)
{
    int failures = 0;

    TEST("snapshot sync service builds valid request pow from peer ip") {
        uint8_t ip[16] = {0};
        struct fast_sync_pow pow;

        ip[15] = 1;
        ASSERT(snapsync_build_request_pow(ip, &pow));
        ASSERT(fast_sync_verify_pow(&pow));
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_stream_helpers(void)
{
    int failures = 0;

    TEST("snapshot sync service parses offers and writes request/challenge payloads") {
        struct snapshot_offer_params params;
        struct snapshot_sync_service svc;
        struct byte_stream offer;
        struct byte_stream request;
        struct byte_stream challenge;
        uint8_t ip[16] = {0};

        memset(&svc, 0, sizeof(svc));
        memset(&params, 0, sizeof(params));
        memset(svc.fc_challenge.seed, 0x11, sizeof(svc.fc_challenge.seed));
        memset(svc.fc_challenge.mmb_root, 0x22, sizeof(svc.fc_challenge.mmb_root));
        svc.fc_challenge.chain_length = 12345;
        ip[15] = 7;

        stream_init(&offer, 160);
        stream_write_i32_le(&offer, 99);
        for (int i = 0; i < 32; i++) stream_write_u8(&offer, (uint8_t)i);
        for (int i = 0; i < 32; i++) stream_write_u8(&offer, (uint8_t)(i + 1));
        for (int i = 0; i < 32; i++) stream_write_u8(&offer, (uint8_t)(i + 2));
        stream_write_u64_le(&offer, 1234);
        stream_write_u64_le(&offer, 5678);
        for (int i = 0; i < 32; i++) stream_write_u8(&offer, (uint8_t)(i + 3));

        ASSERT(snapsync_parse_offer_params(&params, &offer));
        ASSERT(params.height == 99);
        ASSERT(params.num_utxos == 1234);
        ASSERT(params.total_bytes == 5678);
        ASSERT(params.block_hash[0] == 0);
        ASSERT(params.utxo_root[0] == 1);
        ASSERT(params.mmr_root[0] == 2);
        ASSERT(params.mmb_root[0] == 3);

        stream_init(&challenge, 72);
        ASSERT(snapsync_write_fc_challenge(&svc, &challenge));
        ASSERT(challenge.size == 72);

        stream_init(&request, 52);
        ASSERT(snapsync_write_snapshot_request(&request, 88, ip));
        ASSERT(request.size == 52);

        stream_free(&offer);
        stream_free(&challenge);
        stream_free(&request);
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_fc_roundtrip(void)
{
    int failures = 0;

    TEST("snapshot sync service serializes and parses FlyClient responses") {
        struct fc_response resp;
        struct fc_response parsed;
        struct byte_stream s;

        memset(&resp, 0, sizeof(resp));
        resp.num_samples = 1;
        resp.samples[0].leaf.height = 42;
        resp.samples[0].leaf.timestamp = 123;
        resp.samples[0].leaf.nBits = 0x1d00ffffU;
        resp.samples[0].proof.leaf_index = 7;
        resp.samples[0].proof.num_siblings = 1;
        resp.samples[0].proof.num_peaks = 1;
        resp.samples[0].proof.mmb_size = 99;
        memset(resp.samples[0].leaf.block_hash, 0x41, 32);
        memset(resp.samples[0].leaf.sapling_root, 0x42, 32);
        memset(resp.samples[0].leaf.chain_work, 0x43, 32);
        memset(resp.samples[0].proof.leaf_hash, 0x44, 32);
        memset(resp.samples[0].proof.siblings[0], 0x45, 32);
        memset(resp.samples[0].proof.peaks[0], 0x46, 32);

        stream_init(&s, 512);
        ASSERT(snapsync_write_fc_response(&s, &resp));
        s.read_pos = 0;
        ASSERT(snapsync_parse_fc_response(&parsed, &s));
        ASSERT(parsed.num_samples == 1);
        ASSERT(parsed.samples[0].leaf.height == 42);
        ASSERT(parsed.samples[0].proof.leaf_index == 7);
        ASSERT(parsed.samples[0].proof.num_siblings == 1);
        ASSERT(parsed.samples[0].proof.num_peaks == 1);
        ASSERT(parsed.samples[0].proof.mmb_size == 99);
        ASSERT(parsed.samples[0].leaf.block_hash[0] == 0x41);
        ASSERT(parsed.samples[0].proof.siblings[0][0] == 0x45);
        stream_free(&s);
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_activates_tip(void)
{
    int failures = 0;

    TEST("snapshot sync service activates verified snapshot tip") {
        struct snapshot_sync_service svc;
        struct main_state ms;
        struct block_index genesis, snap;
        struct uint256 h0 = {0}, h1 = {0};

        memset(&svc, 0, sizeof(svc));
        memset(&genesis, 0, sizeof(genesis));
        memset(&snap, 0, sizeof(snap));
        main_state_init(&ms);
        block_index_init(&genesis);
        block_index_init(&snap);

        h0.data[0] = 1;
        h1.data[0] = 2;
        genesis.phashBlock = &h0;
        genesis.nHeight = 0;
        snap.phashBlock = &h1;
        snap.nHeight = 1;
        snap.pprev = &genesis;

        block_map_insert(&ms.map_block_index, &h0, &genesis);
        block_map_insert(&ms.map_block_index, &h1, &snap);
        memcpy(svc.offered_block_hash, h1.data, 32);

        ASSERT(snapsync_activate_verified_tip(&svc, &ms) == 1);
        ASSERT(active_chain_tip(&ms.chain_active) == &snap);
        ASSERT(ms.pindex_best_header == &snap);

        main_state_free(&ms);
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_prepare_serve_step(void)
{
    int failures = 0;

    TEST("snapshot sync service prepares serving chunk and end marker") {
        struct p2p_node node;
        struct snapsync_serve_step step;
        uint8_t buf[128];
        size_t pos = 0;

        memset(&node, 0, sizeof(node));
        memset(buf, 0, sizeof(buf));
        node.zsync_total = 2;

        buf[pos++] = 1;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        pos += 32; /* txid */
        pos += 4;  /* vout */
        pos += 8;  /* value */
        pos += 4;  /* height */
        buf[pos++] = 0; /* is_coinbase */
        buf[pos++] = 1; /* script len */
        buf[pos++] = 0x51; /* script */

        ASSERT(snapsync_prepare_serve_step(&step, &node, buf, (int64_t)pos));
        ASSERT(step.action == SNAPSYNC_SERVE_ACTION_SEND_CHUNK);
        ASSERT(step.entries == 1);
        ASSERT(step.chunk_offset == 0);
        ASSERT(step.chunk_len == pos);
        ASSERT(node.zsync_offset == 1);
        ASSERT(node.zsync_sent == 1);
        ASSERT(node.zsync_file_offset == (int64_t)pos);

        ASSERT(snapsync_prepare_serve_step(&step, &node, buf, (int64_t)pos));
        ASSERT(step.action == SNAPSYNC_SERVE_ACTION_SEND_END);
        PASS();
    } _test_next:;

    return failures;
}

static int test_snapshot_sync_service_transition_results(void)
{
    int failures = 0;

    TEST("snapshot sync service exposes router transition results for accept and verify") {
        struct snapsync_offer_acceptance accepted;
        struct snapsync_end_result end_result;
        struct snapsync_serve_start serve_start;
        struct snapsync_offer_followup followup;
        struct snapsync_verify_result verify_result;
        struct snapsync_serve_complete serve_complete;
        struct snapshot_sync_service svc;

        memset(&accepted, 0, sizeof(accepted));
        memset(&end_result, 0, sizeof(end_result));
        memset(&serve_start, 0, sizeof(serve_start));
        memset(&followup, 0, sizeof(followup));
        memset(&verify_result, 0, sizeof(verify_result));
        memset(&serve_complete, 0, sizeof(serve_complete));
        memset(&svc, 0, sizeof(svc));

        snapsync_build_offer_acceptance(&accepted);
        ASSERT(accepted.should_begin_receive);
        ASSERT(accepted.should_store_offer_details);
        ASSERT(accepted.should_reset_offset);
        ASSERT(accepted.should_update_peer_state);
        ASSERT(accepted.peer_state == PEER_SNAPSHOT_RECEIVING);
        ASSERT(accepted.should_set_sync_state);
        ASSERT(accepted.sync_state == SYNC_SNAPSHOT_RECEIVE);

        snapsync_build_serve_start(&serve_start, 1234);
        ASSERT(serve_start.should_begin_serving);
        ASSERT(serve_start.should_reset_progress);
        ASSERT(serve_start.should_reset_cursor);
        ASSERT(serve_start.should_update_peer_state);
        ASSERT(serve_start.peer_state == PEER_SNAPSHOT_SERVING);
        ASSERT(serve_start.total_utxos == 1234);

        svc.fc_verified = false;
        snapsync_build_offer_followup(&followup, &svc);
        ASSERT(followup.should_send);
        ASSERT(followup.action == SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE);

        svc.fc_verified = true;
        snapsync_build_offer_followup(&followup, &svc);
        ASSERT(followup.should_send);
        ASSERT(followup.action == SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ);

        snapsync_build_verify_result(&verify_result, true);
        ASSERT(verify_result.verified);
        ASSERT(verify_result.should_send);
        ASSERT(verify_result.action == SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ);

        snapsync_build_verify_result(&verify_result, false);
        ASSERT(!verify_result.verified);
        ASSERT(!verify_result.should_send);
        ASSERT(verify_result.action == SNAPSYNC_FOLLOWUP_NONE);

        snapsync_build_end_result(&end_result, true);
        ASSERT(end_result.verified);
        ASSERT(end_result.should_resume_header_sync);
        ASSERT(end_result.should_update_peer_state);
        ASSERT(end_result.peer_state == PEER_ACTIVE);
        ASSERT(end_result.should_activate_tip);
        ASSERT(end_result.should_set_sync_state);
        ASSERT(end_result.sync_state == SYNC_HEADERS_DOWNLOAD);

        snapsync_build_end_result(&end_result, false);
        ASSERT(!end_result.verified);
        ASSERT(!end_result.should_resume_header_sync);

        snapsync_build_serve_complete(&serve_complete);
        ASSERT(serve_complete.should_finish_serving);
        ASSERT(serve_complete.should_update_peer_state);
        ASSERT(serve_complete.peer_state == PEER_ACTIVE);
        PASS();
    } _test_next:;

    return failures;
}

int test_snapshot_sync_service(void)
{
    int failures = 0;
    failures += test_snapshot_sync_service_followups();
    failures += test_snapshot_sync_service_builds_pow();
    failures += test_snapshot_sync_service_stream_helpers();
    failures += test_snapshot_sync_service_fc_roundtrip();
    failures += test_snapshot_sync_service_activates_tip();
    failures += test_snapshot_sync_service_prepare_serve_step();
    failures += test_snapshot_sync_service_transition_results();
    return failures;
}
