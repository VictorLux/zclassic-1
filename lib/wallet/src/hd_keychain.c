/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BIP32 HD keychain implementation.
 * Master seed -> account -> change -> address derivation.
 */

#include "wallet/hd_keychain.h"
#include "core/random.h"
#include "encoding/base58.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define DOMAIN "hd"

/* BIP32 serialized key is 78 bytes: 4 version + 74 ext_key payload */
#define BIP32_SERIALIZED_SIZE 78

/* ── Seed generation ──────────────────────────────────────────────── */

bool hd_generate_seed(unsigned char *seed_out, size_t seed_len)
{
    GUARD_NOT_NULL(seed_out, DOMAIN, "seed_out");
    GUARD(seed_len >= HD_SEED_MIN_BYTES && seed_len <= HD_SEED_MAX_BYTES,
          DOMAIN, "seed_len out of range: %zu", seed_len);

    GetRandBytes(seed_out, seed_len);
    return true;
}

/* ── Master key creation ──────────────────────────────────────────── */

bool hd_master_from_seed(struct ext_key *master_out,
                         const unsigned char *seed, size_t seed_len)
{
    GUARD_NOT_NULL(master_out, DOMAIN, "master_out");
    GUARD_NOT_NULL(seed, DOMAIN, "seed");
    GUARD(seed_len >= HD_SEED_MIN_BYTES && seed_len <= HD_SEED_MAX_BYTES,
          DOMAIN, "seed_len out of range: %zu", seed_len);

    ext_key_set_master(master_out, seed, (unsigned int)seed_len);

    if (!privkey_is_valid(&master_out->key))
        LOG_FAIL(DOMAIN, "master key derivation produced invalid key");

    return true;
}

/* ── Path parsing ─────────────────────────────────────────────────── */

int hd_parse_path(const char *path, uint32_t *indices_out, int max_indices)
{
    if (!path || !indices_out || max_indices <= 0)
        return -1;

    const char *p = path;

    /* Skip optional leading "m" or "m/" */
    if (*p == 'm' || *p == 'M') {
        p++;
        if (*p == '/') {
            p++;
            if (*p == '\0')
                return -1; /* "m/" with nothing after slash is invalid */
        }
    }

    if (*p == '\0')
        return 0; /* "m" alone = master key, zero components */

    int count = 0;
    while (*p != '\0') {
        if (count >= max_indices)
            return -1; /* too many components */

        /* Parse the numeric index */
        char *endptr;
        errno = 0;
        unsigned long val = strtoul(p, &endptr, 10);
        if (errno != 0 || endptr == p || val > 0x7FFFFFFFu)
            return -1; /* invalid number */

        uint32_t index = (uint32_t)val;

        /* Check for hardened marker: ' or h or H */
        if (*endptr == '\'' || *endptr == 'h' || *endptr == 'H') {
            index |= BIP32_HARDENED;
            endptr++;
        }

        indices_out[count++] = index;

        /* Expect '/' separator or end of string */
        if (*endptr == '/') {
            endptr++;
            if (*endptr == '\0')
                return -1; /* trailing slash */
        } else if (*endptr != '\0') {
            return -1; /* unexpected character */
        }

        p = endptr;
    }

    return count;
}

/* ── Path derivation ──────────────────────────────────────────────── */

bool hd_derive_path(const struct ext_key *parent, struct ext_key *child_out,
                    const uint32_t *indices, int num_indices)
{
    GUARD_NOT_NULL(parent, DOMAIN, "parent");
    GUARD_NOT_NULL(child_out, DOMAIN, "child_out");
    GUARD(num_indices >= 0 && num_indices <= HD_MAX_DEPTH,
          DOMAIN, "num_indices out of range: %d", num_indices);

    if (num_indices == 0) {
        *child_out = *parent;
        return true;
    }

    struct ext_key current = *parent;
    struct ext_key next;

    for (int i = 0; i < num_indices; i++) {
        if (!ext_key_derive(&current, &next, indices[i])) {
            LOG_FAIL(DOMAIN, "derivation failed at depth %d, index 0x%08x",
                     i, indices[i]);
        }
        current = next;
        memory_cleanse(&next, sizeof(next));
    }

    *child_out = current;
    memory_cleanse(&current, sizeof(current));
    return true;
}

bool hd_derive_path_str(const struct ext_key *master, struct ext_key *child_out,
                        const char *path)
{
    GUARD_NOT_NULL(path, DOMAIN, "path");

    uint32_t indices[HD_MAX_PATH_COMPONENTS];
    int count = hd_parse_path(path, indices, HD_MAX_PATH_COMPONENTS);
    if (count < 0)
        LOG_FAIL(DOMAIN, "invalid path: %s", path);

    return hd_derive_path(master, child_out, indices, count);
}

bool hd_derive_child_index(const struct ext_key *parent,
                           struct ext_key *child_out, uint32_t index)
{
    GUARD_NOT_NULL(parent, DOMAIN, "parent");
    GUARD_NOT_NULL(child_out, DOMAIN, "child_out");

    if (!ext_key_derive(parent, child_out, index))
        LOG_FAIL(DOMAIN, "child derivation failed at index %u", index);

    return true;
}

/* ── Public key path derivation ───────────────────────────────────── */

bool hd_derive_pubkey_path(const struct ext_pubkey *parent,
                           struct ext_pubkey *child_out,
                           const uint32_t *indices, int num_indices)
{
    GUARD_NOT_NULL(parent, DOMAIN, "parent");
    GUARD_NOT_NULL(child_out, DOMAIN, "child_out");
    GUARD(num_indices >= 0 && num_indices <= HD_MAX_DEPTH,
          DOMAIN, "num_indices out of range: %d", num_indices);

    if (num_indices == 0) {
        *child_out = *parent;
        return true;
    }

    struct ext_pubkey current = *parent;
    struct ext_pubkey next;

    for (int i = 0; i < num_indices; i++) {
        if (indices[i] & BIP32_HARDENED)
            LOG_FAIL(DOMAIN, "cannot derive hardened child from public key at depth %d", i);

        if (!ext_pubkey_derive(&current, &next, indices[i]))
            LOG_FAIL(DOMAIN, "pubkey derivation failed at depth %d, index %u",
                     i, indices[i]);
        current = next;
    }

    *child_out = current;
    return true;
}

/* ── Serialization ────────────────────────────────────────────────── */

bool hd_serialize_xprv(const struct ext_key *ek,
                       const unsigned char version[4],
                       char *out, size_t out_size)
{
    GUARD_NOT_NULL(ek, DOMAIN, "ek");
    GUARD_NOT_NULL(version, DOMAIN, "version");
    GUARD_NOT_NULL(out, DOMAIN, "out");
    GUARD(out_size >= HD_XKEY_STRING_SIZE,
          DOMAIN, "out_size too small: %zu", out_size);

    unsigned char data[BIP32_SERIALIZED_SIZE];
    memcpy(data, version, 4);

    unsigned char payload[BIP32_EXTKEY_SIZE];
    ext_key_encode(ek, payload);
    memcpy(data + 4, payload, BIP32_EXTKEY_SIZE);

    size_t written = 0;
    if (!base58check_encode(data, BIP32_SERIALIZED_SIZE, out, out_size, &written)) {
        memory_cleanse(data, sizeof(data));
        LOG_FAIL(DOMAIN, "base58check_encode failed for xprv");
    }

    memory_cleanse(data, sizeof(data));
    return true;
}

bool hd_serialize_xpub(const struct ext_pubkey *epk,
                       const unsigned char version[4],
                       char *out, size_t out_size)
{
    GUARD_NOT_NULL(epk, DOMAIN, "epk");
    GUARD_NOT_NULL(version, DOMAIN, "version");
    GUARD_NOT_NULL(out, DOMAIN, "out");
    GUARD(out_size >= HD_XKEY_STRING_SIZE,
          DOMAIN, "out_size too small: %zu", out_size);

    unsigned char data[BIP32_SERIALIZED_SIZE];
    memcpy(data, version, 4);

    unsigned char payload[BIP32_EXTKEY_SIZE];
    ext_pubkey_encode(epk, payload);
    memcpy(data + 4, payload, BIP32_EXTKEY_SIZE);

    size_t written = 0;
    if (!base58check_encode(data, BIP32_SERIALIZED_SIZE, out, out_size, &written))
        LOG_FAIL(DOMAIN, "base58check_encode failed for xpub");

    return true;
}

bool hd_deserialize_xprv(const char *str,
                         const unsigned char expected_version[4],
                         struct ext_key *ek_out)
{
    GUARD_NOT_NULL(str, DOMAIN, "str");
    GUARD_NOT_NULL(expected_version, DOMAIN, "expected_version");
    GUARD_NOT_NULL(ek_out, DOMAIN, "ek_out");

    unsigned char data[BIP32_SERIALIZED_SIZE + 4]; /* extra room for safety */
    size_t decoded_len = 0;

    if (!base58check_decode(str, data, sizeof(data), &decoded_len))
        LOG_FAIL(DOMAIN, "base58check_decode failed for xprv");

    if (decoded_len != BIP32_SERIALIZED_SIZE)
        LOG_FAIL(DOMAIN, "unexpected decoded length: %zu (expected %d)",
                 decoded_len, BIP32_SERIALIZED_SIZE);

    if (memcmp(data, expected_version, 4) != 0)
        LOG_FAIL(DOMAIN, "version mismatch in xprv");

    ext_key_decode(ek_out, data + 4);

    if (!privkey_is_valid(&ek_out->key)) {
        memory_cleanse(data, sizeof(data));
        LOG_FAIL(DOMAIN, "decoded xprv has invalid private key");
    }

    memory_cleanse(data, sizeof(data));
    return true;
}

bool hd_deserialize_xpub(const char *str,
                         const unsigned char expected_version[4],
                         struct ext_pubkey *epk_out)
{
    GUARD_NOT_NULL(str, DOMAIN, "str");
    GUARD_NOT_NULL(expected_version, DOMAIN, "expected_version");
    GUARD_NOT_NULL(epk_out, DOMAIN, "epk_out");

    unsigned char data[BIP32_SERIALIZED_SIZE + 4];
    size_t decoded_len = 0;

    if (!base58check_decode(str, data, sizeof(data), &decoded_len))
        LOG_FAIL(DOMAIN, "base58check_decode failed for xpub");

    if (decoded_len != BIP32_SERIALIZED_SIZE)
        LOG_FAIL(DOMAIN, "unexpected decoded length: %zu (expected %d)",
                 decoded_len, BIP32_SERIALIZED_SIZE);

    if (memcmp(data, expected_version, 4) != 0)
        LOG_FAIL(DOMAIN, "version mismatch in xpub");

    ext_pubkey_decode(epk_out, data + 4);

    if (!pubkey_is_valid(&epk_out->pubkey))
        LOG_FAIL(DOMAIN, "decoded xpub has invalid public key");

    return true;
}

/* ── Address generation helpers ───────────────────────────────────── */

bool hd_get_pubkey(const struct ext_key *ek, struct pubkey *pk_out)
{
    GUARD_NOT_NULL(ek, DOMAIN, "ek");
    GUARD_NOT_NULL(pk_out, DOMAIN, "pk_out");
    return privkey_get_pubkey(&ek->key, pk_out);
}

struct key_id hd_get_key_id(const struct ext_key *ek)
{
    struct pubkey pk;
    privkey_get_pubkey(&ek->key, &pk);
    return pubkey_get_id(&pk);
}

void hd_get_fingerprint(const struct ext_key *ek, unsigned char fp[4])
{
    struct pubkey pk;
    privkey_get_pubkey(&ek->key, &pk);
    struct key_id kid = pubkey_get_id(&pk);
    memcpy(fp, kid.id.data, 4);
}
