/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

int test_script(void)
{
    int failures = 0;

    printf("script opcodes... ");
    {
        if (strcmp(script_get_op_name(OP_DUP), "OP_DUP") == 0 &&
            strcmp(script_get_op_name(OP_CHECKSIG), "OP_CHECKSIG") == 0 &&
            strcmp(script_get_op_name(OP_HASH160), "OP_HASH160") == 0) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script P2PKH... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        unsigned char hash[20];
        memset(hash, 0xab, 20);
        script_push_data(&s, hash, 20);
        script_push_op(&s, OP_EQUALVERIFY);
        script_push_op(&s, OP_CHECKSIG);
        if (script_is_p2pkh(&s) && s.size == 25)
            printf("OK (size=%zu)\n", s.size);
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("compress_amount roundtrip... ");
    {
        uint64_t values[] = {0, 1, 100000000, 50000000, 2100000000000000ULL};
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            uint64_t c = compress_amount(values[i]);
            uint64_t d = decompress_amount(c);
            if (d != values[i]) { ok = false; break; }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_compress P2PKH... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        unsigned char hash20[20];
        memset(hash20, 0xAB, 20);
        script_push_data(&s, hash20, 20);
        script_push_op(&s, OP_EQUALVERIFY);
        script_push_op(&s, OP_CHECKSIG);

        unsigned char out[33];
        size_t out_len = 0;
        if (script_compress(&s, out, &out_len) && out_len == 21 &&
            out[0] == 0x00 && memcmp(out + 1, hash20, 20) == 0) {
            struct script decoded;
            script_decompress(&decoded, 0x00, out + 1, 20);
            if (decoded.size == 25 && script_is_p2pkh(&decoded))
                printf("OK\n");
            else { printf("FAIL (decompress)\n"); failures++; }
        } else { printf("FAIL (compress)\n"); failures++; }
    }

    printf("block_index_get_ancestor... ");
    {
        struct block_index blocks[5];
        for (int i = 0; i < 5; i++) {
            block_index_init(&blocks[i]);
            blocks[i].nHeight = i;
            blocks[i].pprev = i > 0 ? &blocks[i - 1] : NULL;
        }
        for (int i = 0; i < 5; i++)
            block_index_build_skip(&blocks[i]);

        struct block_index *anc = block_index_get_ancestor(&blocks[4], 1);
        if (anc == &blocks[1])
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
    }

    printf("script_solver P2PKH... ");
    {
        struct key_id kid;
        uint160_set_null(&kid.id);
        unsigned char kbytes[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
        memcpy(kid.id.data, kbytes, 20);
        struct script s;
        script_for_p2pkh(&s, &kid);
        enum txnouttype type;
        unsigned char solutions[20][65];
        size_t solution_sizes[20];
        size_t num_solutions;
        if (script_solver(&s, &type, solutions, solution_sizes, &num_solutions) &&
            type == TX_PUBKEYHASH && num_solutions == 1 && solution_sizes[0] == 20 &&
            solutions[0][0] == 1 && solutions[0][19] == 20)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_solver P2SH... ");
    {
        struct script_id sid;
        unsigned char sbytes[20] = {0xaa,0xbb,0xcc,0xdd,0xee,0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0x11};
        memcpy(sid.hash.data, sbytes, 20);
        struct script s;
        script_for_p2sh(&s, &sid);
        enum txnouttype type;
        unsigned char solutions[20][65];
        size_t solution_sizes[20];
        size_t num_solutions;
        if (script_solver(&s, &type, solutions, solution_sizes, &num_solutions) &&
            type == TX_SCRIPTHASH && num_solutions == 1 && solution_sizes[0] == 20 &&
            solutions[0][0] == 0xaa && solutions[0][19] == 0x11)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_extract_destination P2PKH... ");
    {
        struct key_id kid;
        unsigned char kbytes[20] = {10,20,30,40,50,60,70,80,90,100,110,120,130,140,150,160,170,180,190,200};
        memcpy(kid.id.data, kbytes, 20);
        struct script s;
        script_for_p2pkh(&s, &kid);
        struct tx_destination dest;
        if (script_extract_destination(&s, &dest) && dest.type == DEST_KEY_ID &&
            memcmp(dest.id.key.id.data, kid.id.data, 20) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_for_destination roundtrip... ");
    {
        struct key_id kid;
        unsigned char kbytes[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
        memcpy(kid.id.data, kbytes, 20);
        struct tx_destination dest = { .type = DEST_KEY_ID };
        memcpy(dest.id.key.id.data, kid.id.data, 20);
        struct script s;
        script_for_destination(&s, &dest);
        struct tx_destination dest2;
        if (script_extract_destination(&s, &dest2) && dest2.type == DEST_KEY_ID &&
            memcmp(dest2.id.key.id.data, kid.id.data, 20) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("get_txn_output_type... ");
    {
        if (strcmp(get_txn_output_type(TX_PUBKEYHASH), "pubkeyhash") == 0 &&
            strcmp(get_txn_output_type(TX_SCRIPTHASH), "scripthash") == 0 &&
            strcmp(get_txn_output_type(TX_NULL_DATA), "nulldata") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_id_from_script... ");
    {
        struct script s;
        struct key_id kid;
        memset(&kid, 0, sizeof(kid));
        script_for_p2pkh(&s, &kid);
        struct script_id sid;
        script_id_from_script(&sid, &s);
        bool non_zero = false;
        for (int i = 0; i < 20; i++) {
            if (sid.hash.data[i] != 0) { non_zero = true; break; }
        }
        if (non_zero)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("coins init/alloc/spend... ");
    {
        struct coins c;
        coins_init(&c);
        coins_alloc(&c, 3);
        c.vout[0].value = 50 * COIN;
        c.vout[1].value = 25 * COIN;
        c.vout[2].value = 10 * COIN;
        c.is_coinbase = true;
        c.height = 100;
        if (coins_is_available(&c, 0) && coins_is_available(&c, 1) &&
            !coins_is_pruned(&c)) {
            coins_spend(&c, 1);
            if (!coins_is_available(&c, 1) && coins_is_available(&c, 0)) {
                coins_spend(&c, 0);
                coins_spend(&c, 2);
                if (coins_is_pruned(&c))
                    printf("OK\n");
                else { printf("FAIL (not pruned)\n"); failures++; }
            } else { printf("FAIL (spend)\n"); failures++; }
        } else { printf("FAIL (init)\n"); failures++; }
        coins_free(&c);
    }

    printf("coins_from_transaction... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 2);
        tx.vout[0].value = 100 * COIN;
        tx.vout[1].value = 50 * COIN;
        tx.version = 1;

        struct coins c;
        coins_init(&c);
        coins_from_transaction(&c, &tx, 500);
        if (c.height == 500 && c.version == 1 &&
            c.is_coinbase && c.num_vout == 2 &&
            c.vout[0].value == 100 * COIN)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        coins_free(&c);
        transaction_free(&tx);
    }

    printf("script_num roundtrip... ");
    {
        int64_t values[] = {0, 1, -1, 127, -128, 255, -255, 32767, -32768,
                            2147483647LL, -2147483647LL};
        bool ok = true;
        for (int i = 0; i < 11; i++) {
            struct script_num sn = script_num_from_int(values[i]);
            unsigned char buf[8];
            size_t len = script_num_serialize(&sn, buf, sizeof(buf));
            struct script_num sn2;
            if (!script_num_from_bytes(&sn2, buf, len, true, 8) ||
                sn2.value != values[i]) {
                ok = false; break;
            }
        }
        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_get_op... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_DUP);
        unsigned char payload[] = {0xAA, 0xBB};
        script_push_data(&s, payload, 2);
        script_push_op(&s, OP_CHECKSIG);

        size_t pc = 0;
        enum opcodetype op;
        unsigned char data[520];
        size_t datalen;
        bool ok = true;
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (op == OP_DUP && datalen == 0);
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (datalen == 2 && data[0] == 0xAA && data[1] == 0xBB);
        ok &= script_get_op(&s, &pc, &op, data, &datalen);
        ok &= (op == OP_CHECKSIG);
        ok &= (pc == s.size);
        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_is_push_only... ");
    {
        struct script s;
        script_init(&s);
        unsigned char data[] = {1, 2, 3};
        script_push_data(&s, data, 3);
        if (script_is_push_only(&s)) {
            script_push_op(&s, OP_CHECKSIG);
            if (!script_is_push_only(&s))
                printf("OK\n");
            else { printf("FAIL (non-push passed)\n"); failures++; }
        } else { printf("FAIL (push-only failed)\n"); failures++; }
    }

    printf("sigencoding valid DER... ");
    {
        unsigned char sig[70];
        sig[0] = 0x30; sig[1] = 68;
        sig[2] = 0x02; sig[3] = 32;
        memset(&sig[4], 0x01, 32);
        sig[36] = 0x02; sig[37] = 32;
        memset(&sig[38], 0x01, 32);
        ScriptError err = SCRIPT_ERR_OK;
        bool ok = check_data_signature_encoding(sig, 70, 0, &err);
        if (ok && err == SCRIPT_ERR_OK)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("sigencoding invalid DER... ");
    {
        unsigned char sig[] = {0x30, 0x01, 0x00};
        ScriptError err = SCRIPT_ERR_OK;
        bool ok = check_data_signature_encoding(sig, 3, 0, &err);
        if (!ok && err == SCRIPT_ERR_SIG_DER)
            printf("OK\n");
        else { printf("FAIL (ok=%d, err=%d)\n", ok, err); failures++; }
    }

    printf("sigencoding empty sig... ");
    {
        ScriptError err = SCRIPT_ERR_OK;
        if (check_data_signature_encoding(NULL, 0, 0, &err) &&
            check_transaction_signature_encoding(NULL, 0, 0, &err))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("check_pubkey_encoding... ");
    {
        unsigned char compressed[33] = {0x02};
        unsigned char uncompressed[65] = {0x04};
        unsigned char bad[10] = {0x05};
        ScriptError err;
        if (check_pubkey_encoding(compressed, 33, SCRIPT_VERIFY_STRICTENC, &err) &&
            check_pubkey_encoding(uncompressed, 65, SCRIPT_VERIFY_STRICTENC, &err) &&
            !check_pubkey_encoding(bad, 10, SCRIPT_VERIFY_STRICTENC, &err))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_TRUE... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_TRUE);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1 && cast_to_bool(stack_top(&stk, -1)))
            printf("OK\n");
        else { printf("FAIL (ok=%d, count=%zu)\n", ok, stk.count); failures++; }
    }

    printf("eval_script OP_ADD... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_2);
        script_push_op(&s, OP_3);
        script_push_op(&s, OP_ADD);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1) {
            struct script_num sn;
            script_num_from_bytes(&sn, stack_top(&stk, -1)->data,
                                  stack_top(&stk, -1)->size, false, 4);
            if (sn.value == 5)
                printf("OK\n");
            else { printf("FAIL (value=%" PRId64 ")\n", sn.value); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_EQUAL... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_5);
        script_push_op(&s, OP_5);
        script_push_op(&s, OP_EQUAL);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1 && cast_to_bool(stack_top(&stk, -1)))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("eval_script OP_DUP OP_HASH160... ");
    {
        struct script s;
        script_init(&s);
        unsigned char data[] = {0x01, 0x02, 0x03};
        script_push_data(&s, data, 3);
        script_push_op(&s, OP_DUP);
        script_push_op(&s, OP_HASH160);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 2 && stack_top(&stk, -1)->size == 20)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("verify_script P2PKH (no checker)... ");
    {
        struct key_id kid;
        memset(&kid, 0xAB, sizeof(kid));
        struct script spk;
        script_for_p2pkh(&spk, &kid);
        struct script ss;
        script_init(&ss);
        ScriptError err;
        bool ok = verify_script(&ss, &spk, 0, NULL, 0, &err);
        if (!ok)
            printf("OK (correctly fails without sig)\n");
        else { printf("FAIL (should have failed)\n"); failures++; }
    }

    printf("eval_script OP_IF/OP_ELSE/OP_ENDIF... ");
    {
        struct script s;
        script_init(&s);
        script_push_op(&s, OP_1);
        script_push_op(&s, OP_IF);
        script_push_op(&s, OP_2);
        script_push_op(&s, OP_ELSE);
        script_push_op(&s, OP_3);
        script_push_op(&s, OP_ENDIF);
        struct script_stack stk;
        stack_init(&stk);
        ScriptError err;
        bool ok = eval_script(&stk, &s, 0, NULL, 0, &err);
        if (ok && stk.count == 1) {
            struct script_num sn;
            script_num_from_bytes(&sn, stack_top(&stk, -1)->data,
                                  stack_top(&stk, -1)->size, false, 4);
            if (sn.value == 2)
                printf("OK\n");
            else { printf("FAIL (value=%" PRId64 ")\n", sn.value); failures++; }
        } else { printf("FAIL\n"); failures++; }
    }

    printf("validation_state... ");
    {
        struct validation_state vs;
        validation_state_init(&vs);
        if (validation_state_is_valid(&vs)) {
            validation_state_dos(&vs, 10, false, REJECT_INVALID,
                                 "bad-txns", false, NULL);
            int dos = 0;
            if (validation_state_is_invalid(&vs) &&
                validation_state_get_dos(&vs, &dos) && dos == 10 &&
                strcmp(vs.reject_reason, "bad-txns") == 0)
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        } else { printf("FAIL (init)\n"); failures++; }
    }

    printf("sigcache set/get/erase... ");
    {
        struct sig_cache cache;
        sig_cache_init(&cache);
        struct uint256 hash;
        memset(hash.data, 0x42, 32);
        unsigned char sig[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01};
        unsigned char pk[] = {0x02, 0x01};
        struct uint256 entry;
        sig_cache_compute_entry(&cache, &entry, &hash, sig, 8, pk, 2);
        if (!sig_cache_get(&cache, &entry)) {
            sig_cache_set(&cache, &entry);
            if (sig_cache_get(&cache, &entry)) {
                sig_cache_erase(&cache, &entry);
                if (!sig_cache_get(&cache, &entry))
                    printf("OK\n");
                else { printf("FAIL (erase)\n"); failures++; }
            } else { printf("FAIL (get after set)\n"); failures++; }
        } else { printf("FAIL (false positive)\n"); failures++; }
        sig_cache_destroy(&cache);
    }

    printf("pagelocker lock/unlock... ");
    {
        struct locked_page_manager m;
        locked_page_manager_init(&m);
        unsigned char buf[64];
        locked_page_manager_lock_range(&m, buf, sizeof(buf));
        int count = locked_page_manager_get_count(&m);
        locked_page_manager_unlock_range(&m, buf, sizeof(buf));
        int count2 = locked_page_manager_get_count(&m);
        if (count >= 1 && count2 == 0)
            printf("OK (locked=%d, unlocked=%d)\n", count, count2);
        else { printf("FAIL (locked=%d, unlocked=%d)\n", count, count2); failures++; }
        locked_page_manager_destroy(&m);
    }

    printf("lock_object/unlock_object... ");
    {
        unsigned char secret[32];
        memset(secret, 0xAA, 32);
        lock_object(secret, sizeof(secret));
        unlock_object(secret, sizeof(secret));
        bool zeroed = true;
        for (int i = 0; i < 32; i++) {
            if (secret[i] != 0) { zeroed = false; break; }
        }
        if (zeroed)
            printf("OK (memory cleansed)\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("script_get_sig_op_count... ");
    {
        struct script s;
        s.data[0] = OP_CHECKSIG;
        s.data[1] = OP_CHECKSIG;
        s.data[2] = OP_CHECKMULTISIG;
        s.size = 3;
        uint32_t n = script_get_sig_op_count(&s, 0, false);
        if (n == 22)
            printf("OK\n");
        else { printf("FAIL (%u)\n", n); failures++; }
    }

    printf("script_get_sig_op_count accurate... ");
    {
        struct script s;
        s.data[0] = OP_2;
        s.data[1] = OP_CHECKMULTISIG;
        s.size = 2;
        uint32_t n = script_get_sig_op_count(&s, 0, true);
        if (n == 2)
            printf("OK\n");
        else { printf("FAIL (%u)\n", n); failures++; }
    }

    printf("get_legacy_sig_op_count... ");
    {
        struct transaction tx;
        transaction_init(&tx);
        transaction_alloc(&tx, 1, 1);
        tx.version = 1;
        memset(tx.vin[0].prevout.hash.data, 0x11, 32);
        tx.vin[0].prevout.n = 0;
        tx.vin[0].script_sig.size = 0;
        tx.vout[0].value = COIN;
        tx.vout[0].script_pub_key.data[0] = OP_CHECKSIG;
        tx.vout[0].script_pub_key.size = 1;
        uint64_t ops = get_legacy_sig_op_count(&tx, 0);
        if (ops == 1)
            printf("OK\n");
        else { printf("FAIL (%" PRIu64 ")\n", ops); failures++; }
        transaction_free(&tx);
    }

    printf("script_is_pay_to_script_hash... ");
    {
        struct script s;
        s.data[0] = OP_HASH160;
        s.data[1] = 0x14;
        memset(s.data + 2, 0xAA, 20);
        s.data[22] = OP_EQUAL;
        s.size = 23;
        if (script_is_pay_to_script_hash(&s))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zcl_consensus_version... ");
    {
        if (zcl_consensus_version() == ZCASHCONSENSUS_API_VER)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
