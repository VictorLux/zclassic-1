/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZClassic full node — pure C23 implementation.
 *
 * One binary, two modes:
 *   zclassic23 [node options]         — run as full node
 *   zclassic23 <method> [params...]   — RPC client to running node */

#include "config/boot.h"
#include "rpc/client.h"
#include "net/file_service.h"
#include "util/thread_registry.h"
#include <sqlite3.h>
#include "json/json.h"
#include "views/wallet_gui.h"
#include "models/database.h"
#include "controllers/sync_controller.h"
#include "storage/coins_db.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/ldb_snapshot.h"
#include "chain/checkpoints.h"
#include "coins/utxo_commitment.h"
#include "crypto/sha3.h"
#include "core/uint256.h"
#include "controllers/explorer_internal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ════════════════════════════════════════════════════════════════
 *  CLI MODE — connect to running node, execute RPC, print result
 * ════════════════════════════════════════════════════════════════ */

static char cli_cookie[256];
static int cli_port = 18232;

static bool cli_read_cookie(const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/.cookie", datadir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(cli_cookie, 1, sizeof(cli_cookie) - 1, f);
    fclose(f);
    cli_cookie[n] = 0;
    char *nl = strchr(cli_cookie, '\n');
    if (nl) *nl = 0;
    return n > 0;
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const char *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint8_t a = (uint8_t)in[i], b = (uint8_t)in[i+1], c = (uint8_t)in[i+2];
        out[j++] = b64[a >> 2];
        out[j++] = b64[((a & 3) << 4) | (b >> 4)];
        out[j++] = b64[((b & 0xf) << 2) | (c >> 6)];
        out[j++] = b64[c & 0x3f];
    }
    if (i < len) {
        uint8_t a = (uint8_t)in[i];
        out[j++] = b64[a >> 2];
        if (i + 1 < len) {
            uint8_t b2 = (uint8_t)in[i+1];
            out[j++] = b64[((a & 3) << 4) | (b2 >> 4)];
            out[j++] = b64[(b2 & 0xf) << 2];
        } else {
            out[j++] = b64[(a & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

static char *cli_rpc_call(const char *body, size_t body_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cli_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to node at 127.0.0.1:%d\n", cli_port);
        close(sock);
        return NULL;
    }

    char auth[512];
    b64_encode(cli_cookie, strlen(cli_cookie), auth);

    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        auth, body_len);

    send(sock, hdr, (size_t)hlen, 0);
    send(sock, body, body_len, 0);

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sock); return NULL; }
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = realloc(buf, cap); }
        ssize_t n = recv(sock, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;

    char *start = strstr(buf, "\r\n\r\n");
    if (start) { start += 4; memmove(buf, start, strlen(start) + 1); }
    return buf;
}

static void cli_print(const char *json_str)
{
    struct json_value v;
    if (!json_read(&v, json_str, strlen(json_str))) {
        printf("%s\n", json_str);
        return;
    }
    const struct json_value *err = json_get(&v, "error");
    const struct json_value *res = json_get(&v, "result");
    char out[65536];

    if (err && err->type != JSON_NULL) {
        const struct json_value *msg = json_get(err, "message");
        if (msg && msg->type == JSON_STR)
            fprintf(stderr, "Error: %s\n", json_get_str(msg));
        else { json_write(err, out, sizeof(out)); fprintf(stderr, "Error: %s\n", out); }
        json_free(&v);
        return;
    }
    if (res) {
        if (res->type == JSON_STR) printf("%s\n", json_get_str(res));
        else if (res->type == JSON_INT) printf("%lld\n", (long long)json_get_int(res));
        else if (res->type == JSON_REAL) printf("%.8f\n", json_get_real(res));
        else if (res->type == JSON_BOOL) printf("%s\n", json_get_bool(res) ? "true" : "false");
        else if (res->type == JSON_NULL) printf("null\n");
        else { json_write(res, out, sizeof(out)); printf("%s\n", out); }
    }
    json_free(&v);
}

static int cli_main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    char datadir[512];
    if (home) snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23", home);
    else      snprintf(datadir, sizeof(datadir), ".zclassic-c23");

    int arg_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) {
            snprintf(datadir, sizeof(datadir), "%s", argv[i] + 9);
            arg_start = i + 1;
        } else if (strncmp(argv[i], "-rpcport=", 9) == 0) {
            cli_port = atoi(argv[i] + 9);
            arg_start = i + 1;
        } else break;
    }

    if (!cli_read_cookie(datadir)) {
        fprintf(stderr, "Node not running (no cookie at %s/.cookie)\n", datadir);
        return 1;
    }

    const char *method = argv[arg_start];
    const char **params = (const char **)&argv[arg_start + 1];
    int nparams = argc - arg_start - 1;

    struct json_value jp;
    if (!rpc_convert_values(method, params, (size_t)nparams, &jp)) {
        fprintf(stderr, "Bad parameters\n");
        return 1;
    }

    char pbuf[32768];
    json_write(&jp, pbuf, sizeof(pbuf));
    json_free(&jp);

    char body[65536];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"z\",\"method\":\"%s\",\"params\":%s}",
        method, pbuf);

    char *resp = cli_rpc_call(body, (size_t)blen);
    if (!resp) { fprintf(stderr, "RPC failed\n"); return 1; }
    cli_print(resp);
    free(resp);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 *  NODE MODE — full node daemon
 * ════════════════════════════════════════════════════════════════ */

volatile sig_atomic_t g_shutdown_requested = 0;

/* Round 6 Part 3: alarm-based shutdown watchdog. Async-signal-safe.
 * Previous implementation used pthread_create from the signal handler
 * (not AS-safe) — under some kernel/glibc combinations the watchdog
 * thread never got CPU time and systemd's TimeoutStopSec=90 s fired
 * instead. Replaced with setitimer + SIGALRM handler: alarm() and
 * signal() ARE async-signal-safe, and the kernel guarantees SIGALRM
 * delivery at the scheduled time. */
static void shutdown_alarm_handler(int sig)
{
    (void)sig;
    static const char msg[] =
        "Shutdown watchdog: 90s timeout — forcing exit\n";
    /* write() is async-signal-safe; fprintf is not. */
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static void signal_handler(int sig)
{
    if (g_shutdown_requested) {
        /* Repeated SIGTERM is normal under service managers and must be
         * idempotent. Keep Ctrl-C as the operator's immediate escape hatch. */
        if (sig == SIGINT)
            _exit(1);
        return;
    }
    g_shutdown_requested = 1;
    /* P7.9 — mirror to the thread_registry flag so every loop that
     * polls thread_registry_shutdown_requested() drains alongside the
     * legacy g_shutdown_requested readers. The setter is an atomic
     * store, safe to call from the signal handler. */
    thread_registry_request_shutdown();
    /* Schedule a forced exit if graceful shutdown cannot get control.
     * Startup may still be finishing when SIGTERM arrives, so this must
     * allow enough time for app_init to unwind into app_shutdown. */
    signal(SIGALRM, shutdown_alarm_handler);
    alarm(90);
}

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s [node options]          Run full node\n", prog);
    printf("  %s <method> [params...]    RPC client\n\n", prog);
    printf("Node options:\n");
    printf("  -datadir=<dir>      Data directory\n");
    printf("  -paramsdir=<dir>    Params directory\n");
    printf("  -port=<port>        P2P port (default: 8233)\n");
    printf("  -rpcport=<port>     RPC port (default: 8232)\n");
    printf("  -addnode=<ip>       Add peer\n");
    printf("  -gen                Enable mining\n");
    printf("  -txindex            Transaction index\n");
    printf("  -tor                Start Tor hidden service (dynhost blog)\n");
    printf("  -profile=<name>     Service profile: full, zclassic-only, explorer, onion-node, legacy-compat\n");
    printf("  -nolegacyimport     Do not auto-read/link ~/.zclassic during boot\n");
    printf("  -help               This help\n\n");
    printf("RPC examples:\n");
    printf("  %s getblockcount\n", prog);
    printf("  %s getbalance\n", prog);
    printf("  %s z_gettotalbalance\n", prog);
    printf("  %s chainview 100 5\n", prog);
    printf("  %s z_sendmany \"zs1...\" '[{\"address\":\"zs1...\",\"amount\":0.001}]'\n", prog);
}

/* Detect CLI mode: first non-option arg doesn't start with '-' */
static bool is_cli_mode(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') return true;  /* RPC method name */
        /* Skip known node options with values */
        if (strncmp(argv[i], "-datadir=", 9) == 0 ||
            strncmp(argv[i], "-rpcport=", 9) == 0)
            continue;
        /* Any other -flag is a node option */
        return false;
    }
    return false;
}

/* ── --gen-utxo-snapshot mode ──────────────────────────────────
 *
 * Build-time tool: walks a legacy zclassicd chainstate LevelDB
 * (read via ldb_snapshot to avoid LOCK contention) and emits a
 * canonical UTXO sidecar ready for runtime mmap+SHA3-verify+
 * bulk-INSERT in phase 2.
 *
 * Usage: zclassic23 --gen-utxo-snapshot <legacy_datadir> <out_path>
 *
 * Output format (104-byte header + records, all little-endian):
 *   magic[8]="ZCLUTXO\0", version u32=1, reserved u32, height u32,
 *   reserved u32, count u64, total_supply i64,
 *   anchor_block_hash[32], sha3_hash[32]
 * Per record:
 *   txid[32], vout u32, value i64, script_len u32, script[*],
 *   height u32, is_coinbase u8
 *
 * Per-record encoding intentionally matches
 * lib/coins/src/utxo_commitment.c:utxo_commitment_sha3_compute()
 * so the computed SHA3 equals the compile-time checkpoint at
 * lib/chain/src/checkpoints.c (when run at the anchor height).
 */
struct gen_snap_ctx {
    FILE *out;
    struct sha3_256_ctx hasher;
    uint64_t records;
    uint64_t vouts;
    int64_t  total_supply;
    int      min_height;
    int      max_height;
    bool     fatal;
};

static void gen_snap_le32(uint8_t b[4], uint32_t v)
{ b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); }
static void gen_snap_le64(uint8_t b[8], uint64_t v)
{ for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8*i)); }

static bool gen_snap_write(struct gen_snap_ctx *g, const void *p, size_t n)
{
    if (fwrite(p, 1, n, g->out) != n) {
        fprintf(stderr, "gen_utxo_snapshot: fwrite failed\n");
        g->fatal = true;
        return false;
    }
    sha3_256_write(&g->hasher, (const unsigned char *)p, n);
    return true;
}

static bool gen_snap_cb(const struct uint256 *txid,
                        const struct legacy_coins *coins,
                        void *ctx)
{
    struct gen_snap_ctx *g = ctx;
    if (g->fatal) return false;
    g->records++;
    if (g->min_height == 0 || coins->height < g->min_height)
        g->min_height = coins->height;
    if (coins->height > g->max_height)
        g->max_height = coins->height;

    for (size_t i = 0; i < coins->num_vouts; i++) {
        const struct legacy_coins_vout *o = &coins->vouts[i];
        uint8_t b[8];
        if (!gen_snap_write(g, txid->data, 32)) return false;
        gen_snap_le32(b, o->n);          if (!gen_snap_write(g, b, 4)) return false;
        gen_snap_le64(b, (uint64_t)o->value); if (!gen_snap_write(g, b, 8)) return false;
        gen_snap_le32(b, (uint32_t)o->script_len);
        if (!gen_snap_write(g, b, 4)) return false;
        if (o->script_len > 0 &&
            !gen_snap_write(g, o->script, o->script_len)) return false;
        gen_snap_le32(b, (uint32_t)coins->height);
        if (!gen_snap_write(g, b, 4)) return false;
        uint8_t cb = (uint8_t)(coins->coinbase ? 1 : 0);
        if (!gen_snap_write(g, &cb, 1)) return false;
        g->total_supply += o->value;
        g->vouts++;
    }
    return true;
}

static int gen_utxo_snapshot_mode(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s --gen-utxo-snapshot <legacy_datadir> "
                "<out_sidecar_path>\n", argv[0]);
        return 2;
    }
    const char *legacy = argv[2];
    const char *out_path = argv[3];

    char chainstate_path[1024];
    snprintf(chainstate_path, sizeof(chainstate_path),
             "%s/chainstate", legacy);
    struct stat st;
    if (stat(chainstate_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "missing chainstate at %s\n", chainstate_path);
        return 2;
    }

    char snap_path[1200];
    snprintf(snap_path, sizeof(snap_path),
             "%s.snap_for_utxo_gen", chainstate_path);
    char snap_err[256] = {0};
    bool snap_ok = false;
    for (int t = 0; t < 3 && !snap_ok; t++) {
        snap_ok = ldb_snapshot_make(chainstate_path, snap_path,
                                    snap_err, sizeof(snap_err));
        if (!snap_ok && strcmp(snap_err, "manifest_changed") != 0) break;
    }
    if (!snap_ok) {
        fprintf(stderr, "ldb_snapshot_make(%s) failed: %s\n",
                chainstate_path, snap_err);
        return 1;
    }
    fprintf(stderr, "[gen_utxo] snapshot built at %s\n", snap_path);

    void *h = NULL;
    if (!chainstate_legacy_open(snap_path, &h) || !h) {
        fprintf(stderr, "chainstate_legacy_open failed\n");
        ldb_snapshot_destroy(snap_path);
        return 1;
    }

    struct uint256 best;
    uint256_set_null(&best);
    if (!chainstate_legacy_get_best_block(h, &best)) {
        fprintf(stderr, "no best block\n");
        chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path);
        return 1;
    }
    char hex[65];
    uint256_get_hex(&best, hex);
    fprintf(stderr, "[gen_utxo] chainstate best block: %s\n", hex);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "fopen(%s) failed\n", out_path);
        chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path);
        return 1;
    }
    uint8_t header[104] = {0};
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }

    struct gen_snap_ctx g = { .out = out };
    sha3_256_init(&g.hasher);

    fprintf(stderr, "[gen_utxo] streaming UTXOs ...\n");
    int64_t n = chainstate_legacy_iter(h, gen_snap_cb, &g);
    if (n < 0 || g.fatal) {
        fprintf(stderr, "iter failed (n=%lld fatal=%d)\n",
                (long long)n, g.fatal);
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }

    uint8_t body_sha3[32];
    sha3_256_finalize(&g.hasher, body_sha3);
    char shex[65];
    for (int i = 0; i < 32; i++)
        snprintf(shex + 2*i, 3, "%02x", body_sha3[i]);
    shex[64] = '\0';
    fprintf(stderr,
            "[gen_utxo] records=%llu vouts=%llu total=%.4f ZCL "
            "heights=[%d..%d]\n[gen_utxo] sha3=%s\n",
            (unsigned long long)g.records,
            (unsigned long long)g.vouts,
            (double)g.total_supply / 1e8,
            g.min_height, g.max_height, shex);

    memcpy(header, "ZCLUTXO\x00", 8);
    gen_snap_le32(header + 8, 1);
    gen_snap_le32(header + 16, (uint32_t)g.max_height);
    gen_snap_le64(header + 24, g.vouts);
    gen_snap_le64(header + 32, (uint64_t)g.total_supply);
    memcpy(header + 40, best.data, 32);
    memcpy(header + 72, body_sha3, 32);

    if (fseek(out, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fprintf(stderr, "header rewrite failed\n");
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }
    fclose(out);
    chainstate_legacy_close(h);
    ldb_snapshot_destroy(snap_path);

    fprintf(stderr, "[gen_utxo] wrote %s (%llu records, %llu vouts)\n",
            out_path, (unsigned long long)g.records,
            (unsigned long long)g.vouts);
    return 0;
}

int main(int argc, char **argv)
{
    /* CLI mode: zclassic23 getblockcount */
    if (argc > 1 && is_cli_mode(argc, argv))
        return cli_main(argc, argv);

    /* --gen-utxo-snapshot: build sidecar UTXO file from legacy datadir */
    if (argc >= 2 && strcmp(argv[1], "--gen-utxo-snapshot") == 0)
        return gen_utxo_snapshot_mode(argc, argv);

    /* UTXO repair mode — fetch missing UTXOs from zclassicd, no full node.
     * Usage: zclassic23 --repair [num_blocks] [port] [creds]
     * Scans blocks ahead of current tip via zclassicd RPC, inserts missing
     * UTXOs into SQLite with correct byte order. Restart node after. */
    if (argc >= 2 && strcmp(argv[1], "--repair") == 0) {
        int num_blocks = argc > 2 ? atoi(argv[2]) : 5000;
        int port = argc > 3 ? atoi(argv[3]) : 8232;
        const char *creds = argc > 4 ? argv[4] : "zcluser:zclpass";
        const char *home = getenv("HOME");
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                 home ? home : ".");

        printf("=== UTXO Repair ===\n");
        printf("DB:     %s\n", db_path);
        printf("Source: zclassicd on port %d\n", port);
        printf("Scan:   %d blocks ahead\n\n", num_blocks);

        sqlite3 *db = NULL;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            fprintf(stderr, "Cannot open %s\n", db_path);
            return 1;
        }

        /* Get current tip */
        int tip = 0;
        { sqlite3_stmt *s = NULL;
          sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks", -1, &s, NULL);
          if (s && sqlite3_step(s) == SQLITE_ROW) tip = sqlite3_column_int(s, 0);
          if (s) sqlite3_finalize(s);
        }
        printf("Current tip: %d\n", tip);

        /* Get zclassicd tip */
        char rbuf[4096];
        int ztip = 0;
        if (rpc_call_local(port, creds, "getblockcount", "[]",
                            rbuf, sizeof(rbuf)) > 0) {
            const char *body = rpc_http_body(rbuf);
            if (body) {
                const char *rp = strstr(body, "\"result\":");
                if (rp) ztip = (int)strtol(rp + 9, NULL, 10);
            }
        }
        if (ztip == 0) {
            fprintf(stderr, "Cannot reach zclassicd on port %d\n", port);
            sqlite3_close(db);
            return 1;
        }
        int scan_end = tip + num_blocks;
        if (scan_end > ztip) scan_end = ztip;
        printf("zclassicd tip: %d\nScan range: %d → %d\n\n", ztip, tip+1, scan_end);

        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO utxos"
            "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
            " VALUES(?,?,?,?,?,?,?,0)", -1, &ins, NULL);

        int fixed = 0, checked = 0;

        for (int h = tip + 1; h <= scan_end; h++) {
            /* Get block hash */
            char params[64];
            snprintf(params, sizeof(params), "[%d]", h);
            char hbuf[256];
            if (rpc_call_local(port, creds, "getblockhash", params,
                                hbuf, sizeof(hbuf)) <= 0) break;
            const char *hbody = rpc_http_body(hbuf);
            if (!hbody) break;
            const char *hp = strstr(hbody, "\"result\":\"");
            if (!hp) break;
            char bhash[65] = "";
            { const char *s = hp + 10; int i = 0;
              while (*s && *s != '"' && i < 64) bhash[i++] = *s++;
              bhash[i] = '\0'; }

            /* Get block with full tx data */
            char bparams[128];
            snprintf(bparams, sizeof(bparams), "[\"%s\",2]", bhash);
            char *bbuf = malloc(2*1024*1024); /* 2MB for block data */
            if (!bbuf) break;
            int brc = rpc_call_local(port, creds, "getblock", bparams,
                                      bbuf, 2*1024*1024);
            if (brc <= 0) { free(bbuf); break; }
            const char *bbody = rpc_http_body(bbuf);
            if (!bbody) { free(bbuf); break; }

            /* Parse inputs: find each "vin":[{"txid":"...","vout":N}] */
            const char *p = bbody;
            while ((p = strstr(p, "\"vin\"")) != NULL) {
                p += 5;
                /* Walk through vin array entries */
                while ((p = strstr(p, "\"txid\"")) != NULL) {
                    p += 6;
                    const char *q = strchr(p, '"');
                    if (!q) break;
                    q++;
                    const char *end = strchr(q, '"');
                    if (!end || end - q != 64) { p = end ? end : q; continue; }

                    char txid_hex[65];
                    memcpy(txid_hex, q, 64);
                    txid_hex[64] = '\0';

                    /* Get vout */
                    const char *vp = strstr(end, "\"vout\"");
                    if (!vp) break;
                    int vout = (int)strtol(vp + 7, NULL, 10);
                    checked++;

                    /* Reverse txid for internal byte order */
                    uint8_t txid_bin[32];
                    for (int i = 0; i < 32; i++) {
                        char hex2[3] = { txid_hex[62-2*i], txid_hex[63-2*i], 0 };
                        txid_bin[i] = (uint8_t)strtol(hex2, NULL, 16);
                    }

                    /* Check if exists */
                    sqlite3_stmt *chk = NULL;
                    sqlite3_prepare_v2(db,
                        "SELECT 1 FROM utxos WHERE txid=? AND vout=?",
                        -1, &chk, NULL);
                    sqlite3_bind_blob(chk, 1, txid_bin, 32, SQLITE_STATIC);
                    sqlite3_bind_int(chk, 2, vout);
                    bool exists = (sqlite3_step(chk) == SQLITE_ROW);
                    sqlite3_finalize(chk);

                    if (!exists) {
                        /* Fetch from zclassicd */
                        char txp[128];
                        snprintf(txp, sizeof(txp), "[\"%s\",1]", txid_hex);
                        char txbuf[65536];
                        if (rpc_call_local(port, creds, "getrawtransaction", txp,
                                            txbuf, sizeof(txbuf)) > 0) {
                            const char *txbody = rpc_http_body(txbuf);
                            if (txbody) {
                                /* Find the output at vout index */
                                /* Find "vout":[...{...value...scriptPubKey...}...] */
                                const char *vouts = strstr(txbody, "\"vout\"");
                                if (vouts) {
                                    /* Skip to the vout-th entry */
                                    const char *entry = vouts;
                                    for (int vi = 0; vi <= vout && entry; vi++)
                                        entry = strstr(entry + 1, "\"value\"");
                                    if (entry) {
                                        double val = strtod(entry + 8, NULL);
                                        int64_t val_sat = (int64_t)(val * 1e8 + 0.5);

                                        /* Find scriptPubKey hex */
                                        const char *sp = strstr(entry, "\"hex\":\"");
                                        uint8_t script[520] = {0};
                                        int script_len = 0;
                                        if (sp) {
                                            sp += 7;
                                            const char *se = strchr(sp, '"');
                                            if (se) {
                                                script_len = (int)(se - sp) / 2;
                                                if (script_len > 520) script_len = 520;
                                                for (int si = 0; si < script_len; si++) {
                                                    char h2[3] = { sp[si*2], sp[si*2+1], 0 };
                                                    script[si] = (uint8_t)strtol(h2, NULL, 16);
                                                }
                                            }
                                        }

                                        int stype = 0;
                                        uint8_t addr_hash[20] = {0};
                                        if (script_len == 25 && script[0] == 0x76) {
                                            memcpy(addr_hash, script + 3, 20);
                                        } else if (script_len == 23 && script[0] == 0xa9) {
                                            memcpy(addr_hash, script + 2, 20);
                                            stype = 1;
                                        }

                                        /* Get block height */
                                        int txht = 0;
                                        const char *bhp = strstr(txbody, "\"blockhash\"");
                                        if (bhp) {
                                            /* Get height from block header */
                                            const char *bhs = strchr(bhp + 11, '"');
                                            if (bhs) {
                                                bhs++;
                                                char bhhex[65] = "";
                                                const char *bhe = strchr(bhs, '"');
                                                if (bhe && bhe - bhs == 64) {
                                                    memcpy(bhhex, bhs, 64);
                                                    char bhp2[128];
                                                    snprintf(bhp2, sizeof(bhp2), "[\"%s\"]", bhhex);
                                                    char bhbuf[4096];
                                                    if (rpc_call_local(port, creds, "getblockheader", bhp2,
                                                                        bhbuf, sizeof(bhbuf)) > 0) {
                                                        const char *bhb = rpc_http_body(bhbuf);
                                                        if (bhb) {
                                                            const char *hhp = strstr(bhb, "\"height\":");
                                                            if (hhp) txht = (int)strtol(hhp + 9, NULL, 10);
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* Only insert UTXOs created at or below
                                         * our tip. UTXOs from blocks above the
                                         * tip will be created by connect_block
                                         * when those blocks are connected.
                                         * Inserting them early causes BIP30
                                         * violations (duplicate txid). */
                                        if (txht > 0 && txht <= tip) {
                                        sqlite3_reset(ins);
                                        sqlite3_bind_blob(ins, 1, txid_bin, 32, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 2, vout);
                                        sqlite3_bind_int64(ins, 3, val_sat);
                                        sqlite3_bind_blob(ins, 4, script, script_len, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 5, stype);
                                        sqlite3_bind_blob(ins, 6, addr_hash, 20, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 7, txht);
                                        sqlite3_step(ins);
                                        fixed++;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    p = end + 1;
                }
                break; /* only process first vin array per tx */
            }

            if (h % 200 == 0) {
                sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
                sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                printf("  h=%d  checked=%d  fixed=%d\n", h, checked, fixed);
                fflush(stdout);
            }

            free(bbuf);
        }

        if (ins) sqlite3_finalize(ins);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

        /* Reset coins_best_block to match our SQLite tip so the node
         * doesn't roll back on restart. The --repair modified the utxos
         * table directly, invalidating the cached coins state. */
        if (fixed > 0) {
            sqlite3_stmt *tip_s = NULL;
            sqlite3_prepare_v2(db,
                "SELECT hash FROM blocks WHERE height = "
                "(SELECT MAX(height) FROM blocks)",
                -1, &tip_s, NULL);
            if (tip_s && sqlite3_step(tip_s) == SQLITE_ROW) {
                const void *tip_hash = sqlite3_column_blob(tip_s, 0);
                int tip_len = sqlite3_column_bytes(tip_s, 0);
                if (tip_hash && tip_len >= 32) {
                    sqlite3_stmt *up = NULL;
                    sqlite3_prepare_v2(db,
                        "INSERT OR REPLACE INTO node_state(key,value)"
                        " VALUES('coins_best_block',?)",
                        -1, &up, NULL);
                    if (up) {
                        sqlite3_bind_blob(up, 1, tip_hash, tip_len,
                                          SQLITE_STATIC);
                        sqlite3_step(up);
                        sqlite3_finalize(up);
                        printf("Reset coins_best_block to tip\n");
                    }
                }
            }
            if (tip_s) sqlite3_finalize(tip_s);
        }

        sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        sqlite3_close(db);

        printf("\nDone: checked %d inputs, fixed %d UTXOs\n", checked, fixed);
        printf("Restart the node to apply.\n");
        return 0;
    }

    /* Direct chainstate import mode — no full node startup needed.
     * Usage: zclassic23 --importchainstate /path/to/chainstate [dbpath] */
    if (argc >= 3 && strcmp(argv[1], "--importchainstate") == 0) {
        const char *cs_path = argv[2];
        const char *home = getenv("HOME");
        char db_path[512];
        if (argc > 3)
            snprintf(db_path, sizeof(db_path), "%s", argv[3]);
        else
            snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                     home ? home : ".");

        printf("=== ZClassic UTXO Import ===\n");
        printf("Source: %s (LevelDB chainstate)\n", cs_path);
        printf("Target: %s (SQLite)\n\n", db_path);

        struct node_db ndb;
        if (!node_db_open(&ndb, db_path)) {
            fprintf(stderr, "Cannot open SQLite: %s\n", db_path);
            return 1;
        }

        struct coins_view_db cvdb;
        memset(&cvdb, 0, sizeof(cvdb));
        if (!coins_view_db_open(&cvdb, cs_path, 512, false, false)) {
            fprintf(stderr, "Cannot open LevelDB: %s\n", cs_path);
            node_db_close(&ndb);
            return 1;
        }

        int64_t t0 = (int64_t)time(NULL);
        int count = node_db_sync_import_utxos(&ndb, &cvdb);
        int64_t t1 = (int64_t)time(NULL);
        coins_view_db_close(&cvdb);

        if (count < 0) {
            fprintf(stderr, "Import failed\n");
            node_db_close(&ndb);
            return 1;
        }
        printf("\nImported %d UTXOs in %llds\n", count, (long long)(t1 - t0));

        /* Rebuild wallet_utxos from ground truth */
        printf("Rebuilding wallet_utxos...\n");
        sqlite3_exec(ndb.db, "BEGIN", NULL, NULL, NULL);
        sqlite3_exec(ndb.db, "DELETE FROM wallet_utxos", NULL, NULL, NULL);
        sqlite3_exec(ndb.db,
            "INSERT INTO wallet_utxos "
            "(txid, vout, value, address_hash, script, height, is_coinbase) "
            "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
            "u.height, u.is_coinbase "
            "FROM utxos u INNER JOIN wallet_keys wk "
            "ON u.address_hash = wk.pubkey_hash",
            NULL, NULL, NULL);
        sqlite3_exec(ndb.db, "COMMIT", NULL, NULL, NULL);

        /* Rebuild addresses */
        printf("Rebuilding addresses...\n");
        sqlite3_exec(ndb.db, "BEGIN", NULL, NULL, NULL);
        sqlite3_exec(ndb.db, "DELETE FROM addresses", NULL, NULL, NULL);
        sqlite3_exec(ndb.db,
            "INSERT OR REPLACE INTO addresses "
            "(address_hash, script_type, balance, utxo_count, "
            "first_seen_height, last_seen_height) "
            "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
            "MIN(height), MAX(height) "
            "FROM utxos WHERE address_hash IS NOT NULL "
            "GROUP BY address_hash",
            NULL, NULL, NULL);
        sqlite3_exec(ndb.db, "COMMIT", NULL, NULL, NULL);

        /* Verify results */
        sqlite3_stmt *s = NULL;
        int64_t utxo_count = 0, utxo_sum = 0;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*), COALESCE(SUM(value),0) FROM utxos",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW) {
            utxo_count = sqlite3_column_int64(s, 0);
            utxo_sum = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);

        int64_t wallet_bal = 0, wallet_cnt = 0;
        s = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*), COALESCE(SUM(value),0) FROM wallet_utxos "
            "WHERE spent_txid IS NULL", -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW) {
            wallet_cnt = sqlite3_column_int64(s, 0);
            wallet_bal = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);

        int64_t addr_count = 0;
        s = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM addresses WHERE balance > 0",
            -1, &s, NULL);
        if (sqlite3_step(s) == SQLITE_ROW)
            addr_count = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);

        printf("\n=== Results ===\n");
        printf("UTXOs:     %lld (%.8f ZCL)\n",
               (long long)utxo_count, (double)utxo_sum / 1e8);
        printf("Wallet:    %lld UTXOs (%.8f ZCL)\n",
               (long long)wallet_cnt, (double)wallet_bal / 1e8);
        printf("Addresses: %lld with balance\n", (long long)addr_count);
        printf("Time:      %llds\n", (long long)(t1 - t0));

        /* ── SHA3 UTXO integrity verification ── */
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (cp && cp->height > 0) {
            printf("\nVerifying UTXO set against SHA3 checkpoint "
                   "(h=%d, %llu expected UTXOs)...\n",
                   cp->height, (unsigned long long)cp->utxo_count);
            uint8_t sha3[32];
            uint64_t sha3_count = 0;
            utxo_commitment_sha3_compute(ndb.db, sha3, &sha3_count);
            if (memcmp(sha3, cp->sha3_hash, 32) == 0 &&
                sha3_count == cp->utxo_count) {
                printf("SHA3 UTXO verification PASSED (%llu UTXOs match)\n",
                       (unsigned long long)sha3_count);
            } else {
                fprintf(stderr,
                    "SHA3 UTXO MISMATCH — imported UTXO set is corrupt!\n"
                    "  Expected: %llu UTXOs, got %llu\n",
                    (unsigned long long)cp->utxo_count,
                    (unsigned long long)sha3_count);
                if (sha3_count != cp->utxo_count)
                    fprintf(stderr, "  Count delta: %lld\n",
                            (long long)sha3_count - (long long)cp->utxo_count);
                node_db_close(&ndb);
                return 1;
            }
        }

        node_db_close(&ndb);
        return 0;
    }

    /* MCP server mode: speaks Model Context Protocol on stdio.
     * Install: claude mcp add zcl23 -- zclassic23 -mcp */
    extern int mcp_server_main(const char *datadir, int rpc_port);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-mcp") == 0) {
            const char *h = getenv("HOME");
            char dd[512] = ".zclassic-c23";
            int port = 18232;
            if (h) snprintf(dd, sizeof(dd), "%s/.zclassic-c23", h);
            for (int j = 1; j < argc; j++) {
                if (strncmp(argv[j], "-datadir=", 9) == 0) snprintf(dd, sizeof(dd), "%s", argv[j]+9);
                if (strncmp(argv[j], "-rpcport=", 9) == 0) port = atoi(argv[j]+9);
            }
            return mcp_server_main(dd, port);
        }
    }

    /* Default: headless node. GUI only with explicit --gui flag.
     * ./zclassic23         → node (always)
     * ./zclassic23 --gui   → wallet GUI (if display available) */
    {
        bool gui_mode = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--gui") == 0 ||
                strcmp(argv[i], "--self-test") == 0)
                gui_mode = true;
        }
        if (gui_mode) {
            const char *h = getenv("HOME");
            char dd[512];
            snprintf(dd, sizeof(dd), "%s/.zclassic-c23", h ? h : ".");
            return wallet_gui_main(argc, argv, dd);
        }
    }

    /* Node mode */
    struct app_context ctx;
    app_context_defaults(&ctx);

    const char *home = getenv("HOME");
    char default_datadir[512];
    char default_paramsdir[512];
    if (home) {
        snprintf(default_datadir, sizeof(default_datadir), "%s/.zclassic-c23", home);
        snprintf(default_paramsdir, sizeof(default_paramsdir), "%s/.zcash-params", home);
    } else {
        snprintf(default_datadir, sizeof(default_datadir), ".zclassic-c23");
        snprintf(default_paramsdir, sizeof(default_paramsdir), ".zcash-params");
    }
    ctx.datadir = default_datadir;
    ctx.params_dir = default_paramsdir;

    bool show_metrics = true;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) ctx.datadir = argv[i] + 9;
        else if (strncmp(argv[i], "-paramsdir=", 11) == 0) ctx.params_dir = argv[i] + 11;
        else if (strcmp(argv[i], "-testnet") == 0) ctx.testnet = true;
        else if (strcmp(argv[i], "-regtest") == 0) ctx.regtest = true;
        else if (strcmp(argv[i], "-txindex") == 0) ctx.tx_index = true;
        else if (strcmp(argv[i], "-gen") == 0) ctx.gen = true;
        else if (strncmp(argv[i], "-port=", 6) == 0) { ctx.p2p_port = atoi(argv[i]+6); ctx.listen = true; }
        else if (strncmp(argv[i], "-rpcport=", 9) == 0) ctx.rpc_port = atoi(argv[i]+9);
        else if (strncmp(argv[i], "-httpsport=", 11) == 0) ctx.https_port = atoi(argv[i]+11);
        else if (strncmp(argv[i], "-fsport=", 8) == 0) ctx.fs_port = atoi(argv[i]+8);
        else if (strncmp(argv[i], "-rpcuser=", 9) == 0) ctx.rpc_user = argv[i]+9;
        else if (strncmp(argv[i], "-rpcpassword=", 13) == 0) ctx.rpc_password = argv[i]+13;
        else if (strcmp(argv[i], "-listen") == 0) ctx.listen = true;
        else if (strncmp(argv[i], "-addnode=", 9) == 0) { /* after init */ }
        else if (strncmp(argv[i], "-connect=", 9) == 0) { ctx.connect_only = true; /* after init */ }
        else if (strncmp(argv[i], "-mineraddress=", 14) == 0) ctx.miner_address = argv[i]+14;
        else if (strncmp(argv[i], "-genproclimit=", 14) == 0) ctx.gen_threads = atoi(argv[i]+14);
        else if (strncmp(argv[i], "-cold-import=", 13) == 0) {
            ctx.cold_import_from = argv[i]+13;
        }
        else if (strcmp(argv[i], "-cold-import") == 0) {
            const char *home = getenv("HOME");
            static char default_cold_import_path[1024];
            if (home && *home) {
                snprintf(default_cold_import_path,
                         sizeof(default_cold_import_path),
                         "%s/.zclassic", home);
                ctx.cold_import_from = default_cold_import_path;
            } else {
                fprintf(stderr,
                    "-cold-import with no path requires $HOME; "
                    "use -cold-import=PATH\n");
                return 1;
            }
        }
        else if (strncmp(argv[i], "-fastimport=", 12) == 0) {
            ctx.fastimport_from = argv[i]+12;
        }
        else if (strcmp(argv[i], "-fastimport") == 0) {
            const char *home = getenv("HOME");
            static char default_fastimport_path[1024];
            if (home && *home) {
                snprintf(default_fastimport_path,
                         sizeof(default_fastimport_path),
                         "%s/.zclassic", home);
                ctx.fastimport_from = default_fastimport_path;
            } else {
                fprintf(stderr,
                    "-fastimport with no path requires $HOME; "
                    "use -fastimport=PATH\n");
                return 1;
            }
        }
        else if (strncmp(argv[i], "-snapshot=", 10) == 0) ctx.snapshot_dir = argv[i]+10;
        else if (strcmp(argv[i], "-saplingscan") == 0) ctx.sapling_scan = true;
        else if (strcmp(argv[i], "-reindex-chainstate") == 0) ctx.reindex_chainstate = true;
        else if (strcmp(argv[i], "-reimport-utxos") == 0) ctx.reimport_utxos = true;
        else if (strcmp(argv[i], "-allow-degraded") == 0) ctx.allow_degraded = true;
        else if (strncmp(argv[i], "-showmetrics=", 13) == 0) show_metrics = atoi(argv[i]+13) != 0;
        else if (strcmp(argv[i], "-tor") == 0) ctx.tor = true;
        else if (strncmp(argv[i], "-profile=", 9) == 0) {
            if (!app_runtime_profile_parse(argv[i] + 9,
                                           &ctx.runtime_profile)) {
                fprintf(stderr, "Unknown runtime profile: %s\n", argv[i] + 9);
                return 1;
            }
        }
        else if (strncmp(argv[i], "-assumevalid", 12) == 0) {
            fprintf(stderr,
                    "-assumevalid has been removed; use "
                    "-deferproofvalidationbelow=<blockhash|0>\n");
            return 1;
        }
        else if (strncmp(argv[i], "-deferproofvalidationbelow=",
                         sizeof("-deferproofvalidationbelow=") - 1) == 0) {
            ctx.defer_proof_validation_below =
                argv[i] + sizeof("-deferproofvalidationbelow=") - 1;
        }
        else if (strncmp(argv[i], "-filesync=", 10) == 0) { /* handled above */ }
        else if (strncmp(argv[i], "-fileservice=", 13) == 0) ctx.file_service_peer = argv[i]+13;
        else if (strcmp(argv[i], "-nofilesync") == 0) ctx.no_file_sync = true;
        else if (strcmp(argv[i], "-nobgvalidation") == 0) ctx.no_bg_validation = true;
        else if (strcmp(argv[i], "-nolegacyimport") == 0) ctx.no_legacy_auto_import = true;
        else if (strcmp(argv[i], "-leveldb-no-verify-checksums") == 0) {
            /* Turns off LevelDB checksum verification for both point
             * reads and iteration.  Use only when chasing a suspected
             * corruption issue — silent truncation returns. */
            setenv("ZCL_LEVELDB_NO_VERIFY_CHECKSUMS", "1", 1);
        }
        else if (strncmp(argv[i], "-externalip=", 12) == 0) ctx.external_ip = argv[i] + 12;
        else if (strcmp(argv[i], "-daemon") == 0) { /* legacy compat */ }
        else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
    }

    /* Fast file sync: download block files via SHA3 encrypted service
     * BEFORE starting the full node. Wire speed, not block-by-block. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-filesync=", 10) == 0) {
            const char *host = argv[i] + 10;
            printf("=== SHA3 File Sync from %s:%d ===\n", host, FS_PORT);
            uint8_t utxo_root[32];
            memset(utxo_root, 0, 32);
            char blocks_dir[512];
            snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", ctx.datadir);
            mkdir(blocks_dir, 0755);
            int64_t t0 = (int64_t)time(NULL);
            bool ok = fs_client_sync(host, FS_PORT, ctx.datadir, utxo_root);
            int64_t elapsed = (int64_t)time(NULL) - t0;
            if (elapsed < 1) elapsed = 1;
            if (ok) {
                printf("=== File sync complete: %lld seconds ===\n",
                       (long long)elapsed);
            } else {
                fprintf(stderr, "File sync failed from %s\n", host);
            }
            break;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* -connect mode: only connect to specified peers, no seeds */
    if (ctx.connect_only) {
        extern bool g_connect_only;
        g_connect_only = true;
    }

    printf("zclassic23 starting (datadir=%s)...\n", ctx.datadir);

    if (!app_init(&ctx)) {
        fprintf(stderr, "Initialization failed.\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-addnode=", 9) == 0)
            app_add_node(argv[i] + 9, 0);
        else if (strncmp(argv[i], "-connect=", 9) == 0)
            app_add_node(argv[i] + 9, 0);
    }

    /* Auto-addnode the co-located zclassicd peer (Option F from the
     * fast-sync plan). Local loopback bypasses external network
     * latency entirely; zclassicd is a fully-validated reference node
     * sharing the same chain. Connecting to it gives us tip-tracking
     * resilience even if all internet peers go away.
     *
     * Conservative — only auto-add when:
     *   - $HOME/.zclassic/zclassic.conf is present (zclassicd is set up)
     *   - we haven't been told -connect=… (which means "ONLY these peers")
     *   - no explicit -addnode=127.0.0.1:8034 already on the command line
     *
     * Reads the P2P port out of zclassic.conf (default 8034). */
    if (!ctx.connect_only) {
        const char *home = getenv("HOME");
        if (home && *home) {
            char conf_path[1024];
            snprintf(conf_path, sizeof(conf_path),
                     "%s/.zclassic/zclassic.conf", home);
            FILE *cf = fopen(conf_path, "r");
            if (cf) {
                int p2p_port = 8034;
                char line[256];
                while (fgets(line, sizeof(line), cf)) {
                    int v;
                    if (sscanf(line, " port = %d", &v) == 1 ||
                        sscanf(line, "port=%d", &v) == 1) {
                        if (v > 0 && v < 65536) p2p_port = v;
                    }
                }
                fclose(cf);
                bool already_listed = false;
                char hostport[64];
                snprintf(hostport, sizeof(hostport),
                         "127.0.0.1:%d", p2p_port);
                for (int i = 1; i < argc; i++) {
                    if (strstr(argv[i], hostport) != NULL) {
                        already_listed = true;
                        break;
                    }
                }
                if (!already_listed) {
                    printf("auto-addnode: local zclassicd at %s "
                           "(zclassic.conf detected)\n", hostport);
                    app_add_node("127.0.0.1", p2p_port);
                }
            }
        }
    }

    if (show_metrics) app_start_metrics(ctx.gen);

    while (!g_shutdown_requested &&
           !thread_registry_shutdown_requested() &&
           app_is_running())
        sleep(1);
    if (thread_registry_shutdown_requested())
        g_shutdown_requested = 1;

    if (show_metrics) app_stop_metrics();
    app_shutdown();
    return 0;
}
