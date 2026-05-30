/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * block_log_legacy_rpc — read-only block_log_port over zclassicd RPC.
 * See adapters/outbound/persistence/block_log_legacy_rpc.h for the
 * rationale (no LevelDB lock, no data-dir byte copy) and the contract. */

#include "adapters/outbound/persistence/block_log_legacy_rpc.h"

#include "rpc/legacy_chain_oracle.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdlib.h>
#include <string.h>

/* A ZClassic block serializes to a few hundred KB at most; the legacy
 * RPC transport itself caps responses at 1 MB. The hex string is twice
 * the byte length, so a 2 MB hex buffer (1 MB block) is a generous,
 * safe upper bound. */
#define BLLR_MAX_HEX_LEN (2u * 1024u * 1024u)

struct block_log_legacy_rpc {
    char    *hex;        /* scratch for the getblock hex response */
    size_t   hex_cap;
    uint8_t *read_buf;   /* owned decoded bytes returned by read_at_height */
    size_t   read_cap;
};

/* ── hex decode ─────────────────────────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode `hex` (length `hex_len`, must be even) into h->read_buf, growing
 * it as needed. On success sets *out_len to the decoded byte count. */
static struct zcl_result decode_into_read_buf(struct block_log_legacy_rpc *h,
                                              const char *hex, size_t hex_len,
                                              size_t *out_len)
{
    if (hex_len == 0 || (hex_len % 2) != 0)
        return ZCL_ERR(BLOCK_LOG_ERR_CORRUPT,
                       "legacy_rpc: bad hex length %zu", hex_len);
    size_t want = hex_len / 2;
    if (want > h->read_cap) {
        uint8_t *p = realloc(h->read_buf, want);
        if (!p)
            return ZCL_ERR(BLOCK_LOG_ERR_IO,
                           "legacy_rpc: read_buf realloc %zu", want);
        h->read_buf = p;
        h->read_cap = want;
    }
    for (size_t i = 0; i < want; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return ZCL_ERR(BLOCK_LOG_ERR_CORRUPT,
                           "legacy_rpc: non-hex char at %zu", i * 2);
        h->read_buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = want;
    return ZCL_OK;
}

/* ── port methods ───────────────────────────────────────────────────── */

static struct zcl_result bllr_append(void *self_v, uint32_t height,
                                     const struct block_hash *hash,
                                     const uint8_t *bytes, size_t len)
{
    (void)self_v; (void)height; (void)hash; (void)bytes; (void)len;
    return ZCL_ERR(BLOCK_LOG_ERR_NOT_SUPPORTED,
                   "legacy_rpc: append() on a read-only RPC view");
}

static struct zcl_result bllr_read_by_hash(void *self_v,
                                           const struct block_hash *hash,
                                           const uint8_t **bytes_out,
                                           size_t *len_out)
{
    (void)self_v; (void)hash; (void)bytes_out; (void)len_out;
    /* Height-addressed transport; the conservation diff Job only needs
     * read_at_height. A hash lookup would cost an extra getblock round
     * trip with no current caller. */
    return ZCL_ERR(BLOCK_LOG_ERR_NOT_SUPPORTED,
                   "legacy_rpc: read_by_hash not supported");
}

static struct zcl_result bllr_read_at_height(void *self_v, uint32_t height,
                                             const uint8_t **bytes_out,
                                             size_t *len_out)
{
    struct block_log_legacy_rpc *h = self_v;
    if (!h || !bytes_out || !len_out)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc: null arg(s)");
    *bytes_out = NULL;
    *len_out = 0;

    if (height > (uint32_t)INT32_MAX)
        return ZCL_ERR(BLOCK_LOG_ERR_NOT_FOUND,
                       "legacy_rpc: height %u out of int range", height);

    /* A height strictly above the legacy tip is "not present yet", not a
     * transport error — distinguish so the Job idles rather than blocks. */
    int tip = 0;
    if (legacy_chain_rpc_get_block_count(&tip)) {
        if ((int64_t)height > (int64_t)tip)
            return ZCL_ERR(BLOCK_LOG_ERR_NOT_FOUND,
                           "legacy_rpc: height %u above legacy tip %d",
                           height, tip);
    }
    /* If getblockcount itself failed, fall through: the getblock below
     * will surface the transport error as BLOCK_LOG_ERR_IO. */

    if (!h->hex) {
        h->hex = zcl_malloc(BLLR_MAX_HEX_LEN, "legacy_rpc.hex");
        if (!h->hex)
            return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc: hex alloc");
        h->hex_cap = BLLR_MAX_HEX_LEN;
    }
    h->hex[0] = '\0';

    if (!legacy_chain_rpc_get_block_hex((int)height, h->hex, h->hex_cap))
        return ZCL_ERR(BLOCK_LOG_ERR_IO,
                       "legacy_rpc: getblock failed at height %u "
                       "(zclassicd unreachable or oversize)", height);

    size_t out_len = 0;
    struct zcl_result rd = decode_into_read_buf(h, h->hex, strlen(h->hex),
                                                &out_len);
    if (!rd.ok)
        return rd;

    *bytes_out = h->read_buf;
    *len_out = out_len;
    return ZCL_OK;
}

static uint32_t bllr_tip_height(void *self_v)
{
    (void)self_v;
    int tip = 0;
    if (!legacy_chain_rpc_get_block_count(&tip) || tip < 0)
        return UINT32_MAX;
    return (uint32_t)tip;
}

static struct zcl_result bllr_iter_from(void *self_v, uint32_t start_height,
                                        block_log_iter_fn cb, void *user_data)
{
    struct block_log_legacy_rpc *h = self_v;
    if (!h || !cb)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc: iter null arg(s)");
    uint32_t tip = bllr_tip_height(h);
    if (tip == UINT32_MAX)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc: iter tip unavailable");
    for (uint32_t hgt = start_height; hgt <= tip; hgt++) {
        const uint8_t *bytes = NULL;
        size_t len = 0;
        struct zcl_result r = bllr_read_at_height(h, hgt, &bytes, &len);
        if (!r.ok) {
            if (r.code == BLOCK_LOG_ERR_NOT_FOUND)
                break;
            return r;
        }
        /* read_by_hash is unsupported, so we cannot supply a real hash
         * to the callback cheaply; pass a zeroed hash. The conservation
         * Job does not use iter_from (it steps by height), so this path
         * exists only for contract completeness / diagnostics. */
        struct block_hash zero = {0};
        if (!cb(hgt, &zero, bytes, len, user_data))
            break;
        if (hgt == UINT32_MAX) break;
    }
    return ZCL_OK;
}

/* ── lifecycle ──────────────────────────────────────────────────────── */

struct zcl_result block_log_legacy_rpc_open(
        struct block_log_legacy_rpc **out_handle,
        struct block_log_port *out_port)
{
    if (!out_handle || !out_port)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc_open: null arg(s)");
    *out_handle = NULL;

    struct block_log_legacy_rpc *h =
        zcl_calloc(1, sizeof *h, "block_log_legacy_rpc");
    if (!h)
        return ZCL_ERR(BLOCK_LOG_ERR_IO, "legacy_rpc_open: calloc");

    out_port->self          = h;
    out_port->append        = bllr_append;
    out_port->read_by_hash  = bllr_read_by_hash;
    out_port->read_at_height = bllr_read_at_height;
    out_port->tip_height    = bllr_tip_height;
    out_port->iter_from     = bllr_iter_from;

    *out_handle = h;
    return ZCL_OK;
}

void block_log_legacy_rpc_close(struct block_log_legacy_rpc *h)
{
    if (!h) return;
    free(h->hex);
    free(h->read_buf);
    free(h);
}
