/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * P11.8 — MVP criterion #8 CI gate: parity diff.
 *
 * The production parity service/MCP surface is Agent-2 scope (P12.3 /
 * P12.3.1). This gate pins the comparison contract now so CI can fail
 * loudly on the same mismatch classes the service must report:
 *   1. local chain height must match the remote zclassicd peer
 *   2. sampled block hashes must match at every compared height
 *   3. local coins_best_block must match the active chain tip
 *   4. remote RPC outages must fail the gate, not silently pass
 */

#include "test/test_helpers.h"
#include "controllers/blockchain_controller.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#define P11_8_LOCAL_RPC_PORT 18232
#define P11_8_REMOTE_RPC_PORT 8232

enum parity_gate_verdict {
    PARITY_GATE_OK = 0,
    PARITY_GATE_FAIL_REMOTE_UNREACHABLE,
    PARITY_GATE_FAIL_HEIGHT_MISMATCH,
    PARITY_GATE_FAIL_HASH_MISMATCH,
    PARITY_GATE_FAIL_LOCAL_BEST_BLOCK_MISMATCH,
};

struct parity_gate_report {
    enum parity_gate_verdict verdict;
    int local_height;
    int remote_height;
    int mismatch_height;
    char local_hash[65];
    char remote_hash[65];
};

struct parity_remote_fixture {
    pthread_t thread;
    volatile bool running;
    uint16_t port;
    int height;
    int mismatch_height;
    bool short_chain;
    struct uint256 hashes[8];
};

struct parity_rpc_target {
    int port;
    char auth[256];
};

static uint16_t p11_8_reserve_test_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    uint16_t port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
            port = ntohs(addr.sin_port);
    }

    close(fd);
    return port;
}

static void p11_8_fill_hash(struct uint256 *hash, uint8_t tag)
{
    memset(hash, 0, sizeof(*hash));
    for (size_t i = 0; i < sizeof(hash->data); i++)
        hash->data[i] = (uint8_t)(tag + (uint8_t)i);
}

static void p11_8_build_chain(struct main_state *ms,
                              struct uint256 *hashes,
                              int blocks)
{
    for (int h = 0; h < blocks; h++) {
        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hashes[h]);
        if (!pi)
            return;
        pi->phashBlock = block_map_find_hash(&ms->map_block_index, &hashes[h]);
        pi->nHeight = h;
        pi->nTime = 1700000000u + (uint32_t)h * 150u;
        pi->nVersion = 4;
        pi->nBits = 0x1f07ffffu;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nChainTx = (uint32_t)(h + 1);
        arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        if (h > 0)
            pi->pprev = block_map_find(&ms->map_block_index, &hashes[h - 1]);
        ms->pindex_best_header = pi;
    }

    (void)active_chain_set_tip(&ms->chain_active,
                               block_map_find(&ms->map_block_index,
                                              &hashes[blocks - 1]));
}

static bool p11_8_remote_hash_hex(const struct parity_remote_fixture *fx,
                                  int height,
                                  char out[65])
{
    if (!fx || !out || height < 0)
        return false;
    if (fx->short_chain && height >= fx->height)
        return false;
    if (height > fx->height)
        return false;

    struct uint256 hash = fx->hashes[height];
    if (height == fx->mismatch_height)
        hash.data[0] ^= 0x5a;
    uint256_get_hex(&hash, out);
    return true;
}

static int p11_8_parse_height_param(const char *body)
{
    const char *params = strstr(body, "\"params\"");
    if (!params)
        return -1;

    params = strchr(params, '[');
    if (!params)
        return -1;

    int height = -1;
    if (sscanf(params, "[%d", &height) != 1)
        return -1;
    return height;
}

static void p11_8_respond_remote(int fd, struct parity_remote_fixture *fx,
                                 const char *body)
{
    char method[64] = "";
    const char *mp = strstr(body, "\"method\"");
    if (mp) {
        mp = strchr(mp + 8, '"');
        if (mp) {
            const char *end = strchr(++mp, '"');
            if (end && (size_t)(end - mp) < sizeof(method)) {
                memcpy(method, mp, (size_t)(end - mp));
                method[end - mp] = '\0';
            }
        }
    }

    char result[512];
    if (strcmp(method, "getblockcount") == 0) {
        int remote_height = fx->short_chain ? (fx->height - 1) : fx->height;
        snprintf(result, sizeof(result),
                 "{\"result\":%d,\"error\":null,\"id\":1}", remote_height);
    } else if (strcmp(method, "getbestblockhash") == 0) {
        char hex[65] = "";
        int best_height = fx->short_chain ? (fx->height - 1) : fx->height;
        bool ok = p11_8_remote_hash_hex(fx, best_height, hex);
        snprintf(result, sizeof(result),
                 ok ? "{\"result\":\"%s\",\"error\":null,\"id\":1}"
                    : "{\"result\":null,\"error\":{\"code\":-8},\"id\":1}",
                 hex);
    } else if (strcmp(method, "getblockhash") == 0) {
        char hex[65] = "";
        int height = p11_8_parse_height_param(body);
        bool ok = p11_8_remote_hash_hex(fx, height, hex);
        snprintf(result, sizeof(result),
                 ok ? "{\"result\":\"%s\",\"error\":null,\"id\":1}"
                    : "{\"result\":null,\"error\":{\"code\":-8},\"id\":1}",
                 hex);
    } else {
        snprintf(result, sizeof(result),
                 "{\"result\":null,\"error\":{\"code\":-32601},\"id\":1}");
    }

    char http[1024];
    int len = snprintf(http, sizeof(http),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "\r\n%s",
                       strlen(result), result);
    (void)write(fd, http, (size_t)len);
}

static void *p11_8_remote_thread(void *arg)
{
    struct parity_remote_fixture *fx = arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return NULL;

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(fx->port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(srv);
        return NULL;
    }
    if (listen(srv, 8) != 0) {
        close(srv);
        return NULL;
    }

    while (fx->running) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }

        char req[4096] = "";
        ssize_t n = read(client, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = '\0';
            const char *body = strstr(req, "\r\n\r\n");
            p11_8_respond_remote(client, fx, body ? body + 4 : req);
        }
        close(client);
    }

    close(srv);
    return NULL;
}

static bool p11_8_remote_start(struct parity_remote_fixture *fx)
{
    if (!fx)
        return false;
    fx->port = p11_8_reserve_test_port();
    if (!fx->port)
        return false;
    fx->running = true;
    if (pthread_create(&fx->thread, NULL, p11_8_remote_thread, fx) != 0) {
        fx->running = false;
        return false;
    }
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }
    return true;
}

static void p11_8_remote_stop(struct parity_remote_fixture *fx)
{
    if (!fx || !fx->running)
        return;

    fx->running = false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(fx->port);
        (void)connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
    }

    pthread_join(fx->thread, NULL);
}

static bool p11_8_local_rpc_get_int(const struct rpc_table *tbl,
                                    const char *method,
                                    int *out)
{
    struct json_value params;
    struct json_value result;
    bool ok;

    json_init(&params);
    json_init(&result);
    json_set_array(&params);
    ok = rpc_table_execute(tbl, method, &params, &result);
    ok = ok && result.type == JSON_INT;
    if (ok && out)
        *out = (int)json_get_int(&result);

    json_free(&params);
    json_free(&result);
    return ok;
}

static void p11_8_ensure_rpc_ready(void)
{
    char status[32];
    if (rpc_is_in_warmup(status, sizeof(status)))
        set_rpc_warmup_finished();
}

static bool p11_8_local_rpc_get_hash(const struct rpc_table *tbl,
                                     const char *method,
                                     int height,
                                     char out[65])
{
    struct json_value params;
    struct json_value result;
    struct json_value h;
    bool ok;

    json_init(&params);
    json_init(&result);
    json_init(&h);
    json_set_array(&params);
    if (strcmp(method, "getblockhash") == 0) {
        json_set_int(&h, height);
        json_push_back(&params, &h);
    }
    ok = rpc_table_execute(tbl, method, &params, &result);
    ok = ok && result.type == JSON_STR;
    ok = ok && json_get_str(&result) != NULL;
    if (ok)
        snprintf(out, 65, "%s", json_get_str(&result));

    json_free(&params);
    json_free(&result);
    return ok;
}

static bool p11_8_remote_rpc_get_int(uint16_t port, const char *method, int *out)
{
    char resp[4096];
    struct json_value root;
    bool ok = rpc_call_local(port, "user:pass", method, "[]",
                             resp, sizeof(resp)) > 0;
    if (!ok)
        return false;

    json_init(&root);
    ok = json_read(&root, rpc_http_body(resp), strlen(rpc_http_body(resp)));
    ok = ok && root.type == JSON_OBJ;
    if (ok) {
        const struct json_value *res = json_get(&root, "result");
        ok = res && res->type == JSON_INT;
        if (ok && out)
            *out = (int)json_get_int(res);
    }

    json_free(&root);
    return ok;
}

static bool p11_8_remote_rpc_get_hash(uint16_t port, const char *method,
                                      int height, char out[65])
{
    char params[32];
    char resp[4096];
    struct json_value root;
    const struct json_value *res = NULL;

    snprintf(params, sizeof(params), "[%d]", height);
    if (strcmp(method, "getbestblockhash") == 0)
        snprintf(params, sizeof(params), "[]");

    if (rpc_call_local(port, "user:pass", method, params,
                       resp, sizeof(resp)) <= 0)
        return false;

    json_init(&root);
    bool ok = json_read(&root, rpc_http_body(resp), strlen(rpc_http_body(resp)));
    ok = ok && root.type == JSON_OBJ;
    if (ok)
        res = json_get(&root, "result");
    ok = ok && res && res->type == JSON_STR && json_get_str(res) != NULL;
    if (ok)
        snprintf(out, 65, "%s", json_get_str(res));

    json_free(&root);
    return ok;
}

static bool p11_8_read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *f;

    if (!path || !out || out_size == 0)
        return false;

    f = fopen(path, "r");
    if (!f)
        return false;

    if (!fgets(out, (int)out_size, f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
    nl = strchr(out, '\r');
    if (nl)
        *nl = '\0';
    return out[0] != '\0';
}

static bool p11_8_load_auth_from_conf(const char *path,
                                      char *out,
                                      size_t out_size)
{
    FILE *f;
    char line[256];
    char user[128] = "";
    char pass[128] = "";

    if (!path || !out || out_size == 0)
        return false;

    f = fopen(path, "r");
    if (!f)
        return false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "rpcuser=", 8) == 0) {
            snprintf(user, sizeof(user), "%s", line + 8);
            char *nl = strchr(user, '\n');
            if (nl)
                *nl = '\0';
            nl = strchr(user, '\r');
            if (nl)
                *nl = '\0';
        } else if (strncmp(line, "rpcpassword=", 12) == 0) {
            snprintf(pass, sizeof(pass), "%s", line + 12);
            char *nl = strchr(pass, '\n');
            if (nl)
                *nl = '\0';
            nl = strchr(pass, '\r');
            if (nl)
                *nl = '\0';
        }
    }

    fclose(f);
    if (!user[0] || !pass[0])
        return false;

    snprintf(out, out_size, "%s:%s", user, pass);
    return true;
}

static bool p11_8_load_local_target(struct parity_rpc_target *target)
{
    const char *home = getenv("HOME");
    const char *auth_env = getenv("ZCL_PARITY_LOCAL_AUTH");
    const char *cookie_path = getenv("ZCL_PARITY_LOCAL_COOKIE");
    const char *conf_path = getenv("ZCL_PARITY_LOCAL_CONF");
    const char *datadir = getenv("ZCL_PARITY_LOCAL_DATADIR");
    const char *port_env = getenv("ZCL_PARITY_LOCAL_PORT");
    char path[768];

    if (!target)
        return false;

    memset(target, 0, sizeof(*target));
    target->port = P11_8_LOCAL_RPC_PORT;
    if (port_env && port_env[0])
        target->port = atoi(port_env);

    if (auth_env && auth_env[0]) {
        snprintf(target->auth, sizeof(target->auth), "%s", auth_env);
        return true;
    }

    if (cookie_path && cookie_path[0] &&
        p11_8_read_first_line(cookie_path, target->auth,
                              sizeof(target->auth)))
        return true;

    if (conf_path && conf_path[0] &&
        p11_8_load_auth_from_conf(conf_path, target->auth,
                                  sizeof(target->auth)))
        return true;

    if (datadir && datadir[0]) {
        snprintf(path, sizeof(path), "%s/.cookie", datadir);
        if (p11_8_read_first_line(path, target->auth, sizeof(target->auth)))
            return true;
        snprintf(path, sizeof(path), "%s/zclassic.conf", datadir);
        if (p11_8_load_auth_from_conf(path, target->auth,
                                      sizeof(target->auth)))
            return true;
    }

    if (!home)
        return false;

    snprintf(path, sizeof(path), "%s/.zclassic-c23/.cookie", home);
    if (p11_8_read_first_line(path, target->auth, sizeof(target->auth)))
        return true;

    snprintf(path, sizeof(path), "%s/.zclassic-c23/zclassic.conf", home);
    return p11_8_load_auth_from_conf(path, target->auth,
                                     sizeof(target->auth));
}

static bool p11_8_load_remote_target(struct parity_rpc_target *target)
{
    const char *home = getenv("HOME");
    const char *auth_env = getenv("ZCL_PARITY_REMOTE_AUTH");
    const char *cookie_path = getenv("ZCL_PARITY_REMOTE_COOKIE");
    const char *conf_path = getenv("ZCL_PARITY_REMOTE_CONF");
    const char *datadir = getenv("ZCL_PARITY_REMOTE_DATADIR");
    const char *port_env = getenv("ZCL_PARITY_REMOTE_PORT");
    char path[768];

    if (!target)
        return false;

    memset(target, 0, sizeof(*target));
    target->port = P11_8_REMOTE_RPC_PORT;
    if (port_env && port_env[0])
        target->port = atoi(port_env);

    if (auth_env && auth_env[0]) {
        snprintf(target->auth, sizeof(target->auth), "%s", auth_env);
        return true;
    }

    if (cookie_path && cookie_path[0] &&
        p11_8_read_first_line(cookie_path, target->auth,
                              sizeof(target->auth)))
        return true;

    if (conf_path && conf_path[0] &&
        p11_8_load_auth_from_conf(conf_path, target->auth,
                                  sizeof(target->auth)))
        return true;

    if (datadir && datadir[0]) {
        snprintf(path, sizeof(path), "%s/zclassic.conf", datadir);
        if (p11_8_load_auth_from_conf(path, target->auth,
                                      sizeof(target->auth)))
            return true;
        snprintf(path, sizeof(path), "%s/.cookie", datadir);
        if (p11_8_read_first_line(path, target->auth, sizeof(target->auth)))
            return true;
    }

    if (!home)
        return false;

    snprintf(path, sizeof(path), "%s/.zclassic/zclassic.conf", home);
    if (p11_8_load_auth_from_conf(path, target->auth, sizeof(target->auth)))
        return true;

    snprintf(path, sizeof(path), "%s/.zclassic/.cookie", home);
    return p11_8_read_first_line(path, target->auth, sizeof(target->auth));
}

static bool p11_8_rpc_response_get_result(const char *resp,
                                          struct json_value *root,
                                          const struct json_value **result)
{
    const char *body;
    const struct json_value *err;

    if (!resp || !root || !result)
        return false;

    body = rpc_http_body(resp);
    if (!body)
        return false;

    json_init(root);
    if (!json_read(root, body, strlen(body))) {
        json_free(root);
        return false;
    }

    if (root->type != JSON_OBJ) {
        json_free(root);
        return false;
    }

    err = json_get(root, "error");
    if (err && err->type != JSON_NULL) {
        json_free(root);
        return false;
    }

    *result = json_get(root, "result");
    if (!*result) {
        json_free(root);
        return false;
    }

    return true;
}

static bool p11_8_rpc_get_int(const struct parity_rpc_target *target,
                              const char *method,
                              int *out)
{
    char resp[8192];
    struct json_value root;
    const struct json_value *result = NULL;
    bool ok;

    if (!target || !target->auth[0] || !method)
        return false;

    if (rpc_call_local(target->port, target->auth, method, "[]",
                       resp, sizeof(resp)) <= 0)
        return false;

    ok = p11_8_rpc_response_get_result(resp, &root, &result);
    ok = ok && result->type == JSON_INT;
    if (ok && out)
        *out = (int)json_get_int(result);
    if (ok)
        json_free(&root);
    return ok;
}

static bool p11_8_rpc_get_hash(const struct parity_rpc_target *target,
                               const char *method,
                               int height,
                               char out[65])
{
    char params[32];
    char resp[8192];
    struct json_value root;
    const struct json_value *result = NULL;
    bool ok;

    if (!target || !target->auth[0] || !method || !out)
        return false;

    snprintf(params, sizeof(params), "[]");
    if (strcmp(method, "getblockhash") == 0)
        snprintf(params, sizeof(params), "[%d]", height);

    if (rpc_call_local(target->port, target->auth, method, params,
                       resp, sizeof(resp)) <= 0)
        return false;

    ok = p11_8_rpc_response_get_result(resp, &root, &result);
    ok = ok && result->type == JSON_STR && json_get_str(result) != NULL;
    if (ok)
        snprintf(out, 65, "%s", json_get_str(result));
    if (ok)
        json_free(&root);
    return ok;
}

static bool p11_8_run_gate(struct parity_gate_report *report,
                           const struct rpc_table *local_tbl,
                           struct coins_view_cache *coins_tip,
                           uint16_t remote_port)
{
    if (!report || !local_tbl || !coins_tip)
        return false;

    memset(report, 0, sizeof(*report));
    report->mismatch_height = -1;

    if (!p11_8_local_rpc_get_int(local_tbl, "getblockcount",
                                 &report->local_height) ||
        !p11_8_remote_rpc_get_int(remote_port, "getblockcount",
                                  &report->remote_height)) {
        report->verdict = PARITY_GATE_FAIL_REMOTE_UNREACHABLE;
        return true;
    }

    if (report->local_height != report->remote_height) {
        report->verdict = PARITY_GATE_FAIL_HEIGHT_MISMATCH;
        return true;
    }

    struct uint256 coins_best;
    char chain_best_hex[65];
    char coins_best_hex[65];
    coins_view_cache_get_best_block(coins_tip, &coins_best);
    if (!p11_8_local_rpc_get_hash(local_tbl, "getbestblockhash", 0,
                                  chain_best_hex)) {
        return false;
    }
    uint256_get_hex(&coins_best, coins_best_hex);
    if (strcmp(chain_best_hex, coins_best_hex) != 0) {
        report->verdict = PARITY_GATE_FAIL_LOCAL_BEST_BLOCK_MISMATCH;
        snprintf(report->local_hash, sizeof(report->local_hash), "%s",
                 chain_best_hex);
        snprintf(report->remote_hash, sizeof(report->remote_hash), "%s",
                 coins_best_hex);
        return true;
    }

    for (int h = 0; h <= report->local_height; h++) {
        if (!p11_8_local_rpc_get_hash(local_tbl, "getblockhash", h,
                                      report->local_hash) ||
            !p11_8_remote_rpc_get_hash(remote_port, "getblockhash", h,
                                       report->remote_hash)) {
            report->verdict = PARITY_GATE_FAIL_REMOTE_UNREACHABLE;
            report->mismatch_height = h;
            return true;
        }
        if (strcmp(report->local_hash, report->remote_hash) != 0) {
            report->verdict = PARITY_GATE_FAIL_HASH_MISMATCH;
            report->mismatch_height = h;
            return true;
        }
    }

    report->verdict = PARITY_GATE_OK;
    return true;
}

static int t_p11_8_match_is_ok(void)
{
    struct main_state ms;
    struct rpc_table tbl;
    struct coins_view_cache cache;
    struct coins_view null_view;
    struct uint256 hashes[4];
    struct parity_remote_fixture fx;
    struct parity_gate_report report;
    bool ok = true;

    memset(&fx, 0, sizeof(fx));
    fx.mismatch_height = -1;
    memset(&null_view, 0, sizeof(null_view));
    main_state_init(&ms);
    p11_8_ensure_rpc_ready();
    rpc_table_init(&tbl);
    register_blockchain_rpc_commands(&tbl);
    rpc_blockchain_set_state(&ms, NULL, "/tmp");
    coins_view_cache_init(&cache, &null_view);

    for (int i = 0; i < 4; i++) {
        p11_8_fill_hash(&hashes[i], (uint8_t)(0x10 + i));
        fx.hashes[i] = hashes[i];
    }
    p11_8_build_chain(&ms, hashes, 4);
    coins_view_cache_set_best_block(&cache, active_chain_tip(&ms.chain_active)->phashBlock);

    fx.height = 3;
    ok = p11_8_remote_start(&fx);
    ok = ok && p11_8_run_gate(&report, &tbl, &cache, fx.port);
    ok = ok && report.verdict == PARITY_GATE_OK;

    p11_8_remote_stop(&fx);
    coins_view_cache_free(&cache);
    rpc_blockchain_set_state(NULL, NULL, NULL);
    main_state_free(&ms);
    return ok ? 0 : 1;
}

static int t_p11_8_hash_mismatch_fails(void)
{
    struct main_state ms;
    struct rpc_table tbl;
    struct coins_view_cache cache;
    struct coins_view null_view;
    struct uint256 hashes[4];
    struct parity_remote_fixture fx;
    struct parity_gate_report report;
    bool ok = true;

    memset(&fx, 0, sizeof(fx));
    fx.mismatch_height = -1;
    memset(&null_view, 0, sizeof(null_view));
    main_state_init(&ms);
    p11_8_ensure_rpc_ready();
    rpc_table_init(&tbl);
    register_blockchain_rpc_commands(&tbl);
    rpc_blockchain_set_state(&ms, NULL, "/tmp");
    coins_view_cache_init(&cache, &null_view);

    for (int i = 0; i < 4; i++) {
        p11_8_fill_hash(&hashes[i], (uint8_t)(0x30 + i));
        fx.hashes[i] = hashes[i];
    }
    p11_8_build_chain(&ms, hashes, 4);
    coins_view_cache_set_best_block(&cache, active_chain_tip(&ms.chain_active)->phashBlock);

    fx.height = 3;
    fx.mismatch_height = 2;
    ok = p11_8_remote_start(&fx);
    ok = ok && p11_8_run_gate(&report, &tbl, &cache, fx.port);
    ok = ok && report.verdict == PARITY_GATE_FAIL_HASH_MISMATCH;
    ok = ok && report.mismatch_height == 2;

    p11_8_remote_stop(&fx);
    coins_view_cache_free(&cache);
    rpc_blockchain_set_state(NULL, NULL, NULL);
    main_state_free(&ms);
    return ok ? 0 : 1;
}

static int t_p11_8_remote_outage_fails(void)
{
    struct main_state ms;
    struct rpc_table tbl;
    struct coins_view_cache cache;
    struct coins_view null_view;
    struct uint256 hashes[3];
    struct parity_gate_report report;
    bool ok = true;

    memset(&null_view, 0, sizeof(null_view));
    main_state_init(&ms);
    p11_8_ensure_rpc_ready();
    rpc_table_init(&tbl);
    register_blockchain_rpc_commands(&tbl);
    rpc_blockchain_set_state(&ms, NULL, "/tmp");
    coins_view_cache_init(&cache, &null_view);

    for (int i = 0; i < 3; i++)
        p11_8_fill_hash(&hashes[i], (uint8_t)(0x50 + i));
    p11_8_build_chain(&ms, hashes, 3);
    coins_view_cache_set_best_block(&cache, active_chain_tip(&ms.chain_active)->phashBlock);

    ok = p11_8_run_gate(&report, &tbl, &cache, 6553);
    ok = ok && report.verdict == PARITY_GATE_FAIL_REMOTE_UNREACHABLE;

    coins_view_cache_free(&cache);
    rpc_blockchain_set_state(NULL, NULL, NULL);
    main_state_free(&ms);
    return ok ? 0 : 1;
}

static int t_p11_8_local_best_block_drift_fails(void)
{
    struct main_state ms;
    struct rpc_table tbl;
    struct coins_view_cache cache;
    struct coins_view null_view;
    struct uint256 hashes[4];
    struct parity_remote_fixture fx;
    struct parity_gate_report report;
    bool ok = true;

    memset(&fx, 0, sizeof(fx));
    fx.mismatch_height = -1;
    memset(&null_view, 0, sizeof(null_view));
    main_state_init(&ms);
    p11_8_ensure_rpc_ready();
    rpc_table_init(&tbl);
    register_blockchain_rpc_commands(&tbl);
    rpc_blockchain_set_state(&ms, NULL, "/tmp");
    coins_view_cache_init(&cache, &null_view);

    for (int i = 0; i < 4; i++) {
        p11_8_fill_hash(&hashes[i], (uint8_t)(0x70 + i));
        fx.hashes[i] = hashes[i];
    }
    p11_8_build_chain(&ms, hashes, 4);
    coins_view_cache_set_best_block(&cache, &hashes[1]);

    fx.height = 3;
    ok = p11_8_remote_start(&fx);
    ok = ok && p11_8_run_gate(&report, &tbl, &cache, fx.port);
    ok = ok && report.verdict == PARITY_GATE_FAIL_LOCAL_BEST_BLOCK_MISMATCH;

    p11_8_remote_stop(&fx);
    coins_view_cache_free(&cache);
    rpc_blockchain_set_state(NULL, NULL, NULL);
    main_state_free(&ms);
    return ok ? 0 : 1;
}

static int t_p11_8_live_rpc_parity(void)
{
    static const int sample_heights[] = {
        1, 100, 1000, 10000, 100000, 500000,
        1000000, 1500000, 2000000, 2500000, 3000000,
    };
    struct parity_rpc_target local;
    struct parity_rpc_target remote;
    char local_hash[65];
    char remote_hash[65];
    char best_hash[65];
    int local_height;
    int remote_height;
    bool ok = true;

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run live zclassicd parity)\n");
        return 0;
    }

    if (!p11_8_load_local_target(&local)) {
        printf("SKIP (missing local C23 RPC auth at ~/.zclassic-c23)\n");
        return 0;
    }
    if (!p11_8_load_remote_target(&remote)) {
        printf("SKIP (missing legacy zclassicd RPC auth at ~/.zclassic)\n");
        return 0;
    }

    ok = p11_8_rpc_get_int(&local, "getblockcount", &local_height);
    ok = ok && p11_8_rpc_get_int(&remote, "getblockcount", &remote_height);
    if (!ok) {
        printf("FAIL (cannot reach local=%d or remote=%d RPC)\n",
               local.port, remote.port);
        return 1;
    }

    for (size_t i = 0; i < sizeof(sample_heights) / sizeof(sample_heights[0]); i++) {
        int height = sample_heights[i];

        if (height > local_height) {
            printf("SKIP (local C23 height=%d below sample height=%d)\n",
                   local_height, height);
            return 0;
        }
        if (height > remote_height) {
            printf("SKIP (legacy zclassicd height=%d below sample height=%d)\n",
                   remote_height, height);
            return 0;
        }

        ok = p11_8_rpc_get_hash(&local, "getblockhash", height, local_hash);
        ok = ok && p11_8_rpc_get_hash(&remote, "getblockhash", height, remote_hash);
        if (!ok) {
            printf("FAIL (getblockhash(%d) RPC failed)\n", height);
            return 1;
        }
        if (strcmp(local_hash, remote_hash) != 0) {
            printf("FAIL (height=%d local=%s remote=%s)\n",
                   height, local_hash, remote_hash);
            return 1;
        }
    }

    ok = p11_8_rpc_get_hash(&local, "getbestblockhash", 0, best_hash);
    ok = ok && p11_8_rpc_get_hash(&local, "getblockhash", local_height, local_hash);
    ok = ok && p11_8_rpc_get_hash(&remote, "getblockhash", local_height, remote_hash);
    if (!ok) {
        printf("FAIL (tip hash lookup failed at local height=%d)\n", local_height);
        return 1;
    }
    if (strcmp(best_hash, local_hash) != 0) {
        printf("FAIL (local best=%s tip=%s mismatch)\n", best_hash, local_hash);
        return 1;
    }
    if (strcmp(local_hash, remote_hash) != 0) {
        printf("FAIL (local tip height=%d hash=%s remote=%s)\n",
               local_height, local_hash, remote_hash);
        return 1;
    }

    printf("OK (local=%d remote=%d sampled=%zu)\n",
           local_height, remote_height,
           sizeof(sample_heights) / sizeof(sample_heights[0]));
    return 0;
}

int test_parity_diff_gate(void)
{
    int failures = 0;

    printf("\n=== P11.8 parity diff (MVP #8) ===\n");

    printf("parity_diff P11.8: matching local/remote chain is OK... ");
    if (t_p11_8_match_is_ok()) { printf("FAIL\n"); failures++; }
    else printf("OK\n");

    printf("parity_diff P11.8: blockhash divergence fails loudly... ");
    if (t_p11_8_hash_mismatch_fails()) { printf("FAIL\n"); failures++; }
    else printf("OK\n");

    printf("parity_diff P11.8: remote outage fails gate... ");
    if (t_p11_8_remote_outage_fails()) { printf("FAIL\n"); failures++; }
    else printf("OK\n");

    printf("parity_diff P11.8: local coins_best_block drift fails gate... ");
    if (t_p11_8_local_best_block_drift_fails()) { printf("FAIL\n"); failures++; }
    else printf("OK\n");

    printf("parity_diff P11.8: live C23 vs zclassicd sampled block parity... ");
    failures += t_p11_8_live_rpc_parity();

    return failures;
}
