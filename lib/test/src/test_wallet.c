/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "wallet/keystore.h"
#include "wallet/sapling_keys.h"
#include "wallet/wallet.h"

int test_wallet(void)
{
    int failures = 0;

    printf("keystore_init zeroes state... ");
    {
        struct basic_keystore ks;
        keystore_init(&ks);
        if (ks.num_keys == 0 && ks.num_scripts == 0 && ks.num_watching == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(&ks);
    }

    printf("keystore_add_key + keystore_have_key... ");
    {
        struct basic_keystore ks;
        keystore_init(&ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        bool added = keystore_add_key(&ks, &k);
        bool have = keystore_have_key(&ks, &kid);

        if (added && have && ks.num_keys == 1)
            printf("OK\n");
        else {
            printf("FAIL (added=%d, have=%d, num_keys=%zu)\n",
                   added, have, ks.num_keys);
            failures++;
        }
        keystore_free(&ks);
    }

    printf("keystore_have_key returns false for unknown key... ");
    {
        struct basic_keystore ks;
        keystore_init(&ks);

        struct key_id kid;
        memset(kid.id.data, 0xFF, 20);

        if (!keystore_have_key(&ks, &kid))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(&ks);
    }

    printf("keystore_get_key retrieves added key... ");
    {
        struct basic_keystore ks;
        keystore_init(&ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        keystore_add_key(&ks, &k);

        struct privkey retrieved;
        bool got = keystore_get_key(&ks, &kid, &retrieved);
        if (got && memcmp(retrieved.vch, k.vch, 32) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(&ks);
    }

    printf("keystore_get_pubkey retrieves pubkey... ");
    {
        struct basic_keystore ks;
        keystore_init(&ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        keystore_add_key(&ks, &k);

        struct pubkey retrieved;
        bool got = keystore_get_pubkey(&ks, &kid, &retrieved);
        if (got && retrieved.size == pk.size &&
            memcmp(retrieved.vch, pk.vch, pk.size) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(&ks);
    }

    printf("wallet_is_mine false for unknown script... ");
    {
        struct wallet *w = calloc(1, sizeof(struct wallet));
        wallet_init(w);

        struct tx_out txout;
        tx_out_set_null(&txout);
        txout.value = 100000;
        txout.script_pub_key.size = 3;
        txout.script_pub_key.data[0] = OP_TRUE;

        bool mine = wallet_is_mine(w, &txout);
        if (!mine)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        wallet_free(w);
        free(w);
    }

    printf("wallet_is_mine true for own P2PKH output... ");
    {
        struct wallet *w = calloc(1, sizeof(struct wallet));
        wallet_init(w);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);

        keystore_add_key(&w->keystore, &k);

        struct key_id kid = pubkey_get_id(&pk);
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        dest.id.key = kid;

        struct tx_out txout;
        tx_out_set_null(&txout);
        txout.value = 100000;
        script_for_destination(&txout.script_pub_key, &dest);

        bool mine = wallet_is_mine(w, &txout);
        if (mine)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        wallet_free(w);
        free(w);
    }

    printf("wallet_get_balance empty wallet returns 0... ");
    {
        struct wallet *w = calloc(1, sizeof(struct wallet));
        wallet_init(w);

        int64_t balance = wallet_get_balance(w);
        if (balance == 0)
            printf("OK\n");
        else {
            printf("FAIL (balance=%" PRId64 ")\n", balance);
            failures++;
        }
        wallet_free(w);
        free(w);
    }

    printf("wallet_get_unconfirmed_balance empty wallet returns 0... ");
    {
        struct wallet *w = calloc(1, sizeof(struct wallet));
        wallet_init(w);

        int64_t balance = wallet_get_unconfirmed_balance(w);
        if (balance == 0)
            printf("OK\n");
        else {
            printf("FAIL (balance=%" PRId64 ")\n", balance);
            failures++;
        }
        wallet_free(w);
        free(w);
    }

    printf("sapling_keystore_init zeroes state... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);

        if (!sks.has_seed && sks.num_keys == 0 && sks.next_child_index == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_generate_seed... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);

        bool ok = sapling_keystore_generate_seed(&sks);
        uint8_t zero[32];
        memset(zero, 0, 32);

        if (ok && sks.has_seed && memcmp(sks.seed, zero, 32) != 0)
            printf("OK\n");
        else {
            printf("FAIL (ok=%d, has_seed=%d)\n", ok, sks.has_seed);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_new_address produces valid diversifier... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        bool ok = sapling_keystore_new_address(&sks, diversifier, pk_d);

        uint8_t zero_d[ZC_DIVERSIFIER_SIZE];
        memset(zero_d, 0, ZC_DIVERSIFIER_SIZE);
        uint8_t zero_pk[32];
        memset(zero_pk, 0, 32);

        if (ok && sks.num_keys == 1 &&
            (memcmp(diversifier, zero_d, ZC_DIVERSIFIER_SIZE) != 0 ||
             memcmp(pk_d, zero_pk, 32) != 0))
            printf("OK\n");
        else {
            printf("FAIL (ok=%d, num_keys=%zu)\n", ok, sks.num_keys);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_new_address increments child index... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t d1[ZC_DIVERSIFIER_SIZE], pk1[32];
        uint8_t d2[ZC_DIVERSIFIER_SIZE], pk2[32];
        bool ok1 = sapling_keystore_new_address(&sks, d1, pk1);
        bool ok2 = sapling_keystore_new_address(&sks, d2, pk2);

        if (ok1 && ok2 && sks.num_keys == 2 && sks.next_child_index == 2)
            printf("OK\n");
        else {
            printf("FAIL (ok1=%d, ok2=%d, num_keys=%zu, next=%u)\n",
                   ok1, ok2, sks.num_keys, sks.next_child_index);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_have_spending_key... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t diversifier[ZC_DIVERSIFIER_SIZE], pk_d[32];
        sapling_keystore_new_address(&sks, diversifier, pk_d);

        const struct sapling_key_entry *entry = &sks.keys[0];
        bool have = sapling_keystore_have_spending_key(&sks, entry->ivk);

        if (have)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    return failures;
}
