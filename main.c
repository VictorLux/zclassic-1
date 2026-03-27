/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZClassic full node — pure C23 implementation.
 *
 * One binary, two modes:
 *   zclassic23 [node options]         — run as full node
 *   zclassic23 <method> [params...]   — RPC client to running node */

#include "config/boot.h"
#include "rpc/client.h"
#include <sqlite3.h>
#include "json/json.h"
#include "views/wallet_gui.h"
#include "models/database.h"
#include "controllers/sync_controller.h"
#include "storage/coins_db.h"
#include "controllers/explorer_internal.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
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

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
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

int main(int argc, char **argv)
{
    /* CLI mode: zclassic23 getblockcount */
    if (argc > 1 && is_cli_mode(argc, argv))
        return cli_main(argc, argv);

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

        node_db_close(&ndb);
        return 0;
    }

    /* GUI mode: no args or --self-test = wallet viewer */
    {
        bool gui_mode = (argc <= 1);
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--self-test") == 0) gui_mode = true;
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
        snprintf(default_datadir, sizeof(default_datadir), "%s/.zclassic", home);
        snprintf(default_paramsdir, sizeof(default_paramsdir), "%s/.zcash-params", home);
    } else {
        snprintf(default_datadir, sizeof(default_datadir), ".zclassic");
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
        else if (strncmp(argv[i], "-rpcuser=", 9) == 0) ctx.rpc_user = argv[i]+9;
        else if (strncmp(argv[i], "-rpcpassword=", 13) == 0) ctx.rpc_password = argv[i]+13;
        else if (strcmp(argv[i], "-listen") == 0) ctx.listen = true;
        else if (strncmp(argv[i], "-addnode=", 9) == 0) { /* after init */ }
        else if (strncmp(argv[i], "-mineraddress=", 14) == 0) ctx.miner_address = argv[i]+14;
        else if (strncmp(argv[i], "-genproclimit=", 14) == 0) ctx.gen_threads = atoi(argv[i]+14);
        else if (strncmp(argv[i], "-importlegacy=", 14) == 0) ctx.import_legacy_dir = argv[i]+14;
        else if (strncmp(argv[i], "-fastsync=", 10) == 0) ctx.fastsync_dir = argv[i]+10;
        else if (strncmp(argv[i], "-snapshot=", 10) == 0) ctx.snapshot_dir = argv[i]+10;
        else if (strcmp(argv[i], "-saplingscan") == 0) ctx.sapling_scan = true;
        else if (strcmp(argv[i], "-reindex-chainstate") == 0) ctx.reindex_chainstate = true;
        else if (strncmp(argv[i], "-showmetrics=", 13) == 0) show_metrics = atoi(argv[i]+13) != 0;
        else if (strcmp(argv[i], "-tor") == 0) ctx.tor = true;
        else if (strncmp(argv[i], "-assumevalid=", 13) == 0) ctx.assume_valid = argv[i]+13;
        else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!app_init(&ctx)) {
        if (ctx.import_legacy_dir) return 0;
        fprintf(stderr, "Initialization failed.\n");
        return 1;
    }

    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "-addnode=", 9) == 0)
            app_add_node(argv[i] + 9, 0);

    if (show_metrics) app_start_metrics(ctx.gen);

    while (!g_shutdown_requested && app_is_running())
        sleep(1);

    if (show_metrics) app_stop_metrics();
    app_shutdown();
    return 0;
}
