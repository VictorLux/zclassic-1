/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HTLC script builder/parser and swap state management.
 * Cross-chain atomic swaps: ZCL, BTC, LTC, DOGE.
 *
 * Contract script format derived from dcrdex (Decred DEX):
 *   https://github.com/decred/dcrdex
 *   Copyright (c) 2019-2024 The Decred developers
 *   Blue Oak Model License 1.0.0
 *   https://blueoakcouncil.org/license/1.0.0
 *
 * The 97-byte swap contract script and redeem/refund scriptSig
 * formats follow dcrdex's MakeContract() and RedeemP2SHContract()
 * implementations for cross-chain compatibility. */

#include "script/htlc.h"
#include "crypto/sha256.h"
#include "crypto/sha3.h"
#include "core/hash.h"
#include "core/random.h"
#include "encoding/base58.h"
#include "models/activerecord.h"
#include "models/database.h"
#include "models/swap_contract.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Chain Parameters ───────────────────────────────────────────── *
 * Address version bytes from dcrdex dex/networks/{ltc,doge,zcl}/params.go.
 * ZCL uses 2-byte prefixes (Zcash heritage); BTC/LTC/DOGE use 1-byte. */

static const struct swap_chain_params g_chains[] = {
    [SWAP_CHAIN_ZCL]  = { "zclassic", "ZCL",  {0x1c, 0xb8}, {0x1c, 0xbd}, 2 },
    [SWAP_CHAIN_BTC]  = { "bitcoin",  "BTC",  {0x00, 0x00}, {0x05, 0x00}, 1 },
    [SWAP_CHAIN_LTC]  = { "litecoin", "LTC",  {0x30, 0x00}, {0x32, 0x00}, 1 },
    [SWAP_CHAIN_DOGE] = { "dogecoin", "DOGE", {0x1e, 0x00}, {0x16, 0x00}, 1 },
};

const struct swap_chain_params *swap_get_chain_params(enum swap_chain chain)
{
    if (chain > SWAP_CHAIN_DOGE) return NULL;
    return &g_chains[chain];
}

int swap_parse_chain(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "zcl") == 0 || strcmp(name, "zclassic") == 0)
        return SWAP_CHAIN_ZCL;
    if (strcmp(name, "btc") == 0 || strcmp(name, "bitcoin") == 0)
        return SWAP_CHAIN_BTC;
    if (strcmp(name, "ltc") == 0 || strcmp(name, "litecoin") == 0)
        return SWAP_CHAIN_LTC;
    if (strcmp(name, "doge") == 0 || strcmp(name, "dogecoin") == 0)
        return SWAP_CHAIN_DOGE;
    return -1;
}

/* ── Script Opcodes ─────────────────────────────────────────────── */

#define OP_0            0x00
#define OP_1            0x51
#define OP_DUP          0x76
#define OP_HASH160      0xa9
#define OP_EQUALVERIFY  0x88
#define OP_CHECKSIG     0xac
#define OP_IF           0x63
#define OP_ELSE         0x67
#define OP_ENDIF        0x68
#define OP_SHA256_OP    0xa8
#define OP_SIZE         0x82
#define OP_CLTV         0xb1
#define OP_DROP         0x75
#define OP_DATA_1       0x01
#define OP_DATA_20      0x14
#define OP_DATA_32      0x20
#define OP_DATA_4       0x04

/* ── HTLC Script Builder ────────────────────────────────────────── *
 *
 * Contract script (97 bytes, matches dcrdex MakeContract):
 *
 *   OP_IF
 *     OP_SIZE OP_DATA_1 32 OP_EQUALVERIFY       ← validate secret is 32 bytes
 *     OP_SHA256 OP_DATA_32 <secret_hash>         ← check secret hash
 *     OP_EQUALVERIFY
 *     OP_DUP OP_HASH160 OP_DATA_20 <recipient>  ← recipient pubkey hash
 *   OP_ELSE
 *     OP_DATA_4 <locktime> OP_CLTV OP_DROP       ← timelock for refund
 *     OP_DUP OP_HASH160 OP_DATA_20 <refunder>    ← refunder pubkey hash
 *   OP_ENDIF
 *   OP_EQUALVERIFY OP_CHECKSIG                    ← shared sig check
 *
 * Total: 1+1+1+1+1+1+1+32+1+1+1+1+20 + 1 + 1+4+1+1+1+1+1+20 + 1+1+1 = 97
 */

size_t htlc_build_script(const struct htlc_params *params,
                         uint8_t *out, size_t out_len)
{
    if (out_len < 97) return 0;

    size_t off = 0;

    /* OP_IF */
    out[off++] = OP_IF;

    /*   OP_SIZE <1> 32 OP_EQUALVERIFY  — secret must be 32 bytes */
    out[off++] = OP_SIZE;
    out[off++] = OP_DATA_1;
    out[off++] = 32; /* SecretKeySize */
    out[off++] = OP_EQUALVERIFY;

    /*   OP_SHA256 <32> <secret_hash> OP_EQUALVERIFY */
    out[off++] = OP_SHA256_OP;
    out[off++] = OP_DATA_32;
    memcpy(out + off, params->secret_hash, 32);
    off += 32;
    out[off++] = OP_EQUALVERIFY;

    /*   OP_DUP OP_HASH160 <20> <recipient_pkh> */
    out[off++] = OP_DUP;
    out[off++] = OP_HASH160;
    out[off++] = OP_DATA_20;
    memcpy(out + off, params->recipient_pkh, 20);
    off += 20;

    /* OP_ELSE */
    out[off++] = OP_ELSE;

    /*   <4> <locktime_le> OP_CHECKLOCKTIMEVERIFY OP_DROP */
    out[off++] = OP_DATA_4;
    out[off++] = (uint8_t)(params->locktime & 0xff);
    out[off++] = (uint8_t)((params->locktime >> 8) & 0xff);
    out[off++] = (uint8_t)((params->locktime >> 16) & 0xff);
    out[off++] = (uint8_t)((params->locktime >> 24) & 0xff);
    out[off++] = OP_CLTV;
    out[off++] = OP_DROP;

    /*   OP_DUP OP_HASH160 <20> <refunder_pkh> */
    out[off++] = OP_DUP;
    out[off++] = OP_HASH160;
    out[off++] = OP_DATA_20;
    memcpy(out + off, params->refunder_pkh, 20);
    off += 20;

    /* OP_ENDIF OP_EQUALVERIFY OP_CHECKSIG — shared between branches */
    out[off++] = OP_ENDIF;
    out[off++] = OP_EQUALVERIFY;
    out[off++] = OP_CHECKSIG;

    return off; /* should be exactly 97 */
}

bool htlc_p2sh_address(const uint8_t *redeem_script, size_t script_len,
                       enum swap_chain chain,
                       char *addr_out, size_t addr_len)
{
    const struct swap_chain_params *cp = swap_get_chain_params(chain);
    if (!cp) return false;

    /* HASH160 of the redeem script */
    uint8_t script_hash[20];
    hash160(redeem_script, script_len, script_hash);

    /* Build payload: prefix + script_hash */
    uint8_t payload[22];
    memcpy(payload, cp->p2sh_prefix, cp->prefix_len);
    memcpy(payload + cp->prefix_len, script_hash, 20);

    size_t out_len = 0;
    return base58check_encode(payload, cp->prefix_len + 20,
                              addr_out, addr_len, &out_len);
}

void htlc_generate_secret(uint8_t secret[32], uint8_t secret_hash[32])
{
    GetRandBytes(secret, 32);
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_write(&ctx, secret, 32);
    sha256_finalize(&ctx, secret_hash);
}

/* Extract secret from a P2SH redeem scriptSig.
 * dcrdex format: <sig> <pubkey> <secret> OP_1 <redeemscript>
 * The secret is the 3rd push (index 2), always 32 bytes. */
bool htlc_extract_secret(const uint8_t *script_sig, size_t sig_len,
                         uint8_t secret_out[32])
{
    const uint8_t *p = script_sig;
    const uint8_t *end = script_sig + sig_len;
    int push_index = 0;

    while (p < end) {
        uint8_t opcode = *p++;
        size_t data_len = 0;

        if (opcode >= 0x01 && opcode <= 0x4b) {
            data_len = opcode;
        } else if (opcode == 0x4c) {
            if (p >= end) return false;
            data_len = *p++;
        } else if (opcode == 0x4d) {
            if (p + 2 > end) return false;
            data_len = (size_t)p[0] | ((size_t)p[1] << 8);
            p += 2;
        } else {
            /* OP_0, OP_1, etc. — zero-length push */
            push_index++;
            continue;
        }

        if (p + data_len > end) return false;

        /* The 3rd push (index 2) is the secret */
        if (push_index == 2 && data_len == 32) {
            memcpy(secret_out, p, 32);
            return true;
        }

        p += data_len;
        push_index++;
    }
    return false;
}

/* ── Address → Pubkey Hash ───────────────────────────────────────── *
 * Base58Check decode, strip version prefix, extract 20-byte PKH. */

bool htlc_address_to_pkh(const char *address, enum swap_chain chain,
                         uint8_t pkh_out[20])
{
    if (!address || !address[0]) return false;

    const struct swap_chain_params *cp = swap_get_chain_params(chain);
    if (!cp) return false;

    uint8_t decoded[32];
    size_t decoded_len = 0;
    if (!base58check_decode(address, decoded, sizeof(decoded), &decoded_len))
        return false;

    /* decoded = [prefix bytes] [20-byte pubkey hash]
     * ZCL has 2-byte prefix, BTC/LTC/DOGE have 1-byte */
    size_t expected = cp->prefix_len + 20;
    if (decoded_len != expected)
        return false;

    memcpy(pkh_out, decoded + cp->prefix_len, 20);
    return true;
}

/* ── Redeem/Refund ScriptSig Builders ───────────────────────────── *
 * Matches dcrdex RedeemP2SHContract / RefundP2SHContract exactly:
 *   Redeem: <sig> <pubkey> <secret> OP_1 <redeemscript>
 *   Refund: <sig> <pubkey> OP_0 <redeemscript>         */

static size_t push_script_data(uint8_t *out, const uint8_t *data, size_t len)
{
    size_t off = 0;
    if (len <= 0x4b) {
        out[off++] = (uint8_t)len;
    } else if (len <= 0xff) {
        out[off++] = 0x4c; /* OP_PUSHDATA1 */
        out[off++] = (uint8_t)len;
    } else {
        out[off++] = 0x4d; /* OP_PUSHDATA2 */
        out[off++] = (uint8_t)(len & 0xff);
        out[off++] = (uint8_t)((len >> 8) & 0xff);
    }
    memcpy(out + off, data, len);
    return off + len;
}

size_t htlc_build_redeem_scriptsig(uint8_t *out, size_t out_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const uint8_t *pubkey, size_t pubkey_len,
                                   const uint8_t secret[32],
                                   const uint8_t *contract, size_t contract_len)
{
    if (out_len < sig_len + pubkey_len + 32 + contract_len + 10) return 0;

    size_t off = 0;
    off += push_script_data(out + off, sig, sig_len);
    off += push_script_data(out + off, pubkey, pubkey_len);
    off += push_script_data(out + off, secret, 32);
    out[off++] = OP_1;  /* push TRUE for OP_IF branch */
    off += push_script_data(out + off, contract, contract_len);
    return off;
}

size_t htlc_build_refund_scriptsig(uint8_t *out, size_t out_len,
                                   const uint8_t *sig, size_t sig_len,
                                   const uint8_t *pubkey, size_t pubkey_len,
                                   const uint8_t *contract, size_t contract_len)
{
    if (out_len < sig_len + pubkey_len + contract_len + 10) return 0;

    size_t off = 0;
    off += push_script_data(out + off, sig, sig_len);
    off += push_script_data(out + off, pubkey, pubkey_len);
    out[off++] = OP_0;  /* push FALSE for OP_ELSE branch */
    off += push_script_data(out + off, contract, contract_len);
    return off;
}

/* ── Swap ID ────────────────────────────────────────────────────── */

void swap_compute_id(const char *my_addr, const char *counter_addr,
                     const uint8_t secret_hash[32], char out[65])
{
    struct sha3_256_ctx sha3;
    sha3_256_init(&sha3);
    sha3_256_write(&sha3, (const unsigned char *)my_addr, strlen(my_addr));
    sha3_256_write(&sha3, (const unsigned char *)counter_addr,
                   strlen(counter_addr));
    sha3_256_write(&sha3, secret_hash, 32);
    uint8_t hash[32];
    sha3_256_finalize(&sha3, hash);

    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[64] = '\0';
}

/* ── SQLite Persistence ─────────────────────────────────────────── */

bool db_swap_save(struct node_db *ndb, const struct swap_contract *swap)
{
    if (!ndb || !ndb->open) LOG_FAIL("htlc", "db_swap_save: db not open");
    if (!swap) LOG_FAIL("htlc", "db_swap_save: swap is NULL");

    struct ar_callbacks *cbs = db_swap_contract_callbacks();
    AR_VALIDATE_RECORD(cbs, "swap_contract", swap, db_swap_contract_validate);
    if (!ar_run_before_save(cbs, (void *)swap))
        return false;

    const char *sql =
        "INSERT OR REPLACE INTO zswp_contracts"
        "(swap_id,role,state,chain,secret_hash,secret,amount,"
        "locktime,my_address,counter_address,funding_txid,"
        "funding_vout,redeem_script,redeem_script_len,"
        "p2sh_address,created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) LOG_FAIL("htlc", "db_swap_save: prepare failed: %s", sqlite3_errmsg(ndb->db));

    sqlite3_bind_text(s, 1, swap->swap_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, swap->role);
    sqlite3_bind_int(s, 3, swap->state);
    sqlite3_bind_int(s, 4, swap->chain);
    sqlite3_bind_blob(s, 5, swap->secret_hash, 32, SQLITE_STATIC);
    if (swap->has_secret)
        sqlite3_bind_blob(s, 6, swap->secret, 32, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 6);
    sqlite3_bind_int64(s, 7, swap->amount);
    sqlite3_bind_int(s, 8, (int)swap->locktime);
    sqlite3_bind_text(s, 9, swap->my_address, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 10, swap->counter_address, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 11, swap->funding_txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 12, (int)swap->funding_vout);
    sqlite3_bind_blob(s, 13, swap->redeem_script, (int)swap->redeem_script_len,
                      SQLITE_STATIC);
    sqlite3_bind_int(s, 14, (int)swap->redeem_script_len);
    sqlite3_bind_text(s, 15, swap->p2sh_address, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 16, swap->created_at);

    bool ok = AR_STEP_DONE(s);
    sqlite3_finalize(s);
    if (ok) ar_run_after_save(cbs, (void *)swap);
    return ok;
}

static void row_to_swap(sqlite3_stmt *s, struct swap_contract *out)
{
    memset(out, 0, sizeof(*out));
    const char *str = (const char *)sqlite3_column_text(s, 0);
    if (str) snprintf(out->swap_id, sizeof(out->swap_id), "%s", str);

    out->role = (enum swap_role)sqlite3_column_int(s, 1);
    out->state = (enum swap_state)sqlite3_column_int(s, 2);
    out->chain = (enum swap_chain)sqlite3_column_int(s, 3);

    const void *blob = sqlite3_column_blob(s, 4);
    if (blob) memcpy(out->secret_hash, blob, 32);

    blob = sqlite3_column_blob(s, 5);
    if (blob) { memcpy(out->secret, blob, 32); out->has_secret = true; }

    out->amount = sqlite3_column_int64(s, 6);
    out->locktime = (uint32_t)sqlite3_column_int(s, 7);

    str = (const char *)sqlite3_column_text(s, 8);
    if (str) snprintf(out->my_address, sizeof(out->my_address), "%s", str);

    str = (const char *)sqlite3_column_text(s, 9);
    if (str) snprintf(out->counter_address, sizeof(out->counter_address),
                      "%s", str);

    blob = sqlite3_column_blob(s, 10);
    if (blob) memcpy(out->funding_txid, blob, 32);

    out->funding_vout = (uint32_t)sqlite3_column_int(s, 11);

    blob = sqlite3_column_blob(s, 12);
    int rlen = sqlite3_column_int(s, 13);
    if (blob && rlen > 0 && rlen <= 256) {
        memcpy(out->redeem_script, blob, (size_t)rlen);
        out->redeem_script_len = (size_t)rlen;
    }

    str = (const char *)sqlite3_column_text(s, 14);
    if (str) snprintf(out->p2sh_address, sizeof(out->p2sh_address), "%s", str);

    out->created_at = sqlite3_column_int64(s, 15);
}

bool db_swap_find(struct node_db *ndb, const char *swap_id,
                  struct swap_contract *out)
{
    if (!ndb || !ndb->open) return false;

    const char *sql =
        "SELECT swap_id,role,state,chain,secret_hash,secret,amount,"
        "locktime,my_address,counter_address,funding_txid,"
        "funding_vout,redeem_script,redeem_script_len,"
        "p2sh_address,created_at"
        " FROM zswp_contracts WHERE swap_id=?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(s, 1, swap_id, -1, SQLITE_STATIC);
    bool found = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        row_to_swap(s, out);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

int db_swap_list(struct node_db *ndb, struct swap_contract *out,
                 size_t max, int state_filter)
{
    if (!ndb || !ndb->open) return 0;

    const char *sql = (state_filter >= 0)
        ? "SELECT swap_id,role,state,chain,secret_hash,secret,amount,"
          "locktime,my_address,counter_address,funding_txid,"
          "funding_vout,redeem_script,redeem_script_len,"
          "p2sh_address,created_at"
          " FROM zswp_contracts WHERE state=? ORDER BY created_at DESC LIMIT ?"
        : "SELECT swap_id,role,state,chain,secret_hash,secret,amount,"
          "locktime,my_address,counter_address,funding_txid,"
          "funding_vout,redeem_script,redeem_script_len,"
          "p2sh_address,created_at"
          " FROM zswp_contracts ORDER BY created_at DESC LIMIT ?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;

    if (state_filter >= 0) {
        sqlite3_bind_int(s, 1, state_filter);
        sqlite3_bind_int(s, 2, (int)max);
    } else {
        sqlite3_bind_int(s, 1, (int)max);
    }

    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && (size_t)count < max) {
        row_to_swap(s, &out[count]);
        count++;
    }
    sqlite3_finalize(s);
    return count;
}

bool db_swap_update_state(struct node_db *ndb, const char *swap_id,
                          enum swap_state state, const uint8_t *secret)
{
    if (!ndb || !ndb->open) return false;

    const char *sql = secret
        ? "UPDATE zswp_contracts SET state=?, secret=? WHERE swap_id=?"
        : "UPDATE zswp_contracts SET state=? WHERE swap_id=?";

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(s, 1, state);
    if (secret) {
        sqlite3_bind_blob(s, 2, secret, 32, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, swap_id, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_text(s, 2, swap_id, -1, SQLITE_STATIC);
    }

    bool ok = AR_STEP_WRITE(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
