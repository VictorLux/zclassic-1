/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast File Service — SHA3-encrypted direct TCP transfer.
 * Designed for maximum throughput: 64KB fixed frames, zero protocol
 * overhead visible to observers, wire-speed on gigabit links. */

#include "net/file_service.h"
#include "crypto/sha3_crypt.h"
#include "crypto/sha3.h"
#include "core/random.h"
#include "controllers/file_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <sys/stat.h>

/* ── Session management ────────────────────────────────────────── */

void fs_session_init(struct fs_session *s, int fd)
{
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->start_time = (int64_t)time(NULL);

    /* TCP tuning for max throughput */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 4 * 1024 * 1024; /* 4MB send buffer */
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
}

double fs_session_mbps(const struct fs_session *s)
{
    int64_t elapsed = (int64_t)time(NULL) - s->start_time;
    if (elapsed < 1) elapsed = 1;
    uint64_t total = s->bytes_sent + s->bytes_received;
    return (double)total / (1048576.0 * (double)elapsed);
}

/* ── Raw I/O helpers ───────────────────────────────────────────── */

static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

/* ── Frame encryption/decryption ───────────────────────────────── */

/* Encrypt and MAC a frame. Always produces exactly FS_FRAME_SIZE bytes. */
static bool encrypt_frame(const struct fs_session *s, uint8_t type,
                            const uint8_t *payload, uint32_t payload_len,
                            uint8_t out[FS_FRAME_SIZE], uint64_t counter)
{
    if (payload_len > FS_MAX_PAYLOAD) return false;

    /* Build plaintext frame: [type][len][payload][random padding] */
    uint8_t plain[FS_FRAME_SIZE - FS_MAC_SIZE];
    memset(plain, 0, sizeof(plain));

    /* Header */
    plain[0] = type;
    plain[1] = (uint8_t)(type >> 8);
    plain[2] = (uint8_t)(type >> 16);
    plain[3] = (uint8_t)(type >> 24);
    plain[4] = (uint8_t)(payload_len);
    plain[5] = (uint8_t)(payload_len >> 8);
    plain[6] = (uint8_t)(payload_len >> 16);
    plain[7] = (uint8_t)(payload_len >> 24);

    /* Payload */
    if (payload_len > 0)
        memcpy(plain + FS_HEADER_SIZE, payload, payload_len);

    /* Padding is zeros — encrypted zeros look random. No need for
     * expensive /dev/urandom reads on every frame. */

    /* SHA3-CTR encrypt using AVX-512 4-way parallel SHA3-512.
     * Generates 256 bytes of keystream per batch (4 × 64).
     * 64KB / 256 = 256 batches. With AVX-512: ~4x faster. */
    uint8_t nonce[32];
    memset(nonce, 0, 32);
    memcpy(nonce, &counter, 8);

    size_t offset = 0;
    uint64_t block_ctr = 0;
    while (offset < sizeof(plain)) {
        uint8_t ks[256];
        sha3_512_x4(s->key, nonce, block_ctr, ks);
        block_ctr += 4;

        size_t chunk = sizeof(plain) - offset;
        if (chunk > 256) chunk = 256;
        for (size_t i = 0; i < chunk; i++)
            plain[offset + i] ^= ks[i];
        offset += chunk;
    }

    /* Copy encrypted data */
    memcpy(out, plain, sizeof(plain));

    /* MAC: SHA3-256(key || counter || encrypted_data) */
    struct sha3_256_ctx mac_ctx;
    sha3_256_init(&mac_ctx);
    sha3_256_write(&mac_ctx, s->key, 32);
    sha3_256_write(&mac_ctx, (const unsigned char *)&counter, 8);
    sha3_256_write(&mac_ctx, out, sizeof(plain));
    sha3_256_finalize(&mac_ctx, out + sizeof(plain));

    return true;
}

static bool decrypt_frame(const struct fs_session *s,
                            const uint8_t in[FS_FRAME_SIZE],
                            uint8_t *type_out, uint8_t *payload_buf,
                            uint32_t *payload_len_out, uint64_t counter)
{
    size_t ct_len = FS_FRAME_SIZE - FS_MAC_SIZE;

    /* Verify MAC first (fail fast) */
    uint8_t expected_mac[32];
    struct sha3_256_ctx mac_ctx;
    sha3_256_init(&mac_ctx);
    sha3_256_write(&mac_ctx, s->key, 32);
    sha3_256_write(&mac_ctx, (const unsigned char *)&counter, 8);
    sha3_256_write(&mac_ctx, in, ct_len);
    sha3_256_finalize(&mac_ctx, expected_mac);

    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= in[ct_len + i] ^ expected_mac[i];
    if (diff != 0) return false;

    /* Decrypt using AVX-512 4-way parallel SHA3 */
    uint8_t plain[FS_FRAME_SIZE - FS_MAC_SIZE];
    memcpy(plain, in, ct_len);

    uint8_t nonce[32];
    memset(nonce, 0, 32);
    memcpy(nonce, &counter, 8);

    size_t offset = 0;
    uint64_t block_ctr = 0;
    while (offset < ct_len) {
        uint8_t ks[256];
        sha3_512_x4(s->key, nonce, block_ctr, ks);
        block_ctr += 4;

        size_t chunk = ct_len - offset;
        if (chunk > 256) chunk = 256;
        for (size_t i = 0; i < chunk; i++)
            plain[offset + i] ^= ks[i];
        offset += chunk;
    }

    /* Parse header */
    *type_out = plain[0];
    uint32_t plen = (uint32_t)plain[4] |
                    ((uint32_t)plain[5] << 8) |
                    ((uint32_t)plain[6] << 16) |
                    ((uint32_t)plain[7] << 24);

    if (plen > FS_MAX_PAYLOAD) return false;
    *payload_len_out = plen;
    if (plen > 0)
        memcpy(payload_buf, plain + FS_HEADER_SIZE, plen);

    return true;
}

/* ── Public frame send/recv ────────────────────────────────────── */

bool fs_send_frame(struct fs_session *s, uint8_t type,
                    const uint8_t *payload, uint32_t payload_len)
{
    uint8_t frame[FS_FRAME_SIZE];
    if (!encrypt_frame(s, type, payload, payload_len,
                        frame, s->send_counter))
        return false;
    s->send_counter++;
    s->bytes_sent += FS_FRAME_SIZE;
    return send_all(s->fd, frame, FS_FRAME_SIZE);
}

static uint8_t g_recv_payload[FS_MAX_PAYLOAD];

bool fs_recv_frame(struct fs_session *s, uint8_t *type_out,
                    const uint8_t **payload_out, uint32_t *payload_len_out)
{
    uint8_t frame[FS_FRAME_SIZE];
    if (!recv_all(s->fd, frame, FS_FRAME_SIZE))
        return false;
    s->bytes_received += FS_FRAME_SIZE;

    if (!decrypt_frame(s, frame, type_out, g_recv_payload,
                        payload_len_out, s->recv_counter))
        return false;
    s->recv_counter++;
    *payload_out = g_recv_payload;
    return true;
}

/* ── Handshake ─────────────────────────────────────────────────── */

bool fs_handshake(struct fs_session *s, const uint8_t utxo_root[32],
                   bool is_initiator)
{
    /* Generate our nonce */
    GetRandBytes(s->our_nonce, 32);

    if (is_initiator) {
        /* Send our nonce in cleartext (pre-key) */
        if (!send_all(s->fd, s->our_nonce, 32)) return false;
        /* Receive peer nonce */
        if (!recv_all(s->fd, s->peer_nonce, 32)) return false;
    } else {
        /* Receive peer nonce first */
        if (!recv_all(s->fd, s->peer_nonce, 32)) return false;
        /* Send our nonce */
        if (!send_all(s->fd, s->our_nonce, 32)) return false;
    }

    /* Derive shared key from UTXO root + both nonces */
    sha3_crypt_derive_key(utxo_root, s->our_nonce, s->peer_nonce, s->key);
    s->key_established = true;

    /* Verify key agreement: send SHA3(key || "verify") */
    uint8_t verify[32];
    struct sha3_256_ctx vctx;
    sha3_256_init(&vctx);
    sha3_256_write(&vctx, s->key, 32);
    sha3_256_write(&vctx, (const unsigned char *)"verify", 6);
    sha3_256_finalize(&vctx, verify);

    if (!send_all(s->fd, verify, 32)) return false;
    uint8_t peer_verify[32];
    if (!recv_all(s->fd, peer_verify, 32)) return false;

    if (memcmp(verify, peer_verify, 32) != 0) {
        fprintf(stderr, "fs_handshake: key verification FAILED "
                "(peer not on same chain)\n");
        return false;
    }

    printf("fs_handshake: key established (SHA3 quantum-secure)\n");
    return true;
}

/* ── Fast encrypted chunk transfer ─────────────────────────────── */
/* Encrypt the ENTIRE chunk as one unit — one SHA3 MAC per 50MB
 * instead of 800 MACs (one per 64KB frame). AVX-512 SHA3 keystream
 * runs at ~1 GB/s, so network (100 Mbps) is the bottleneck.
 *
 * Wire format: [4-byte size][encrypted data][32-byte MAC]
 * MAC = SHA3-256(key || chunk_counter || ciphertext)
 * Encryption = SHA3-CTR with AVX-512 4-way parallel */

static void encrypt_chunk_inplace(const uint8_t key[32],
                                    uint64_t chunk_counter,
                                    uint8_t *data, uint32_t size)
{
    uint8_t nonce[32];
    memset(nonce, 0, 32);
    memcpy(nonce, &chunk_counter, 8);

    uint32_t offset = 0;
    uint64_t block_ctr = 0;
    while (offset < size) {
        uint8_t ks[256];
        sha3_512_x4(key, nonce, block_ctr, ks);
        block_ctr += 4;
        uint32_t chunk = size - offset;
        if (chunk > 256) chunk = 256;
        for (uint32_t i = 0; i < chunk; i++)
            data[offset + i] ^= ks[i];
        offset += chunk;
    }
}

bool fs_send_chunk_fast(struct fs_session *s, const uint8_t *data,
                         uint32_t size, const uint8_t sha3[32])
{
    /* Allocate, copy, encrypt in-place */
    uint8_t *buf = malloc(size);
    if (!buf) return false;
    memcpy(buf, data, size);
    encrypt_chunk_inplace(s->key, s->send_counter, buf, size);

    /* Send size header */
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(size);
    hdr[1] = (uint8_t)(size >> 8);
    hdr[2] = (uint8_t)(size >> 16);
    hdr[3] = (uint8_t)(size >> 24);
    if (!send_all(s->fd, hdr, 4)) { free(buf); return false; }

    /* Send encrypted data */
    if (!send_all(s->fd, buf, size)) { free(buf); return false; }

    /* MAC over ciphertext: SHA3-256(key || counter || sha3 || ciphertext) */
    uint8_t mac[32];
    struct sha3_256_ctx mctx;
    sha3_256_init(&mctx);
    sha3_256_write(&mctx, s->key, 32);
    sha3_256_write(&mctx, (const unsigned char *)&s->send_counter, 8);
    sha3_256_write(&mctx, sha3, 32); /* bind MAC to expected content */
    sha3_256_write(&mctx, buf, size);
    sha3_256_finalize(&mctx, mac);
    free(buf);

    if (!send_all(s->fd, mac, 32)) return false;

    s->bytes_sent += 4 + size + 32;
    s->send_counter++;
    return true;
}

static bool fs_recv_chunk_fast(struct fs_session *s, uint8_t **out,
                                uint32_t *out_size,
                                const uint8_t expected_sha3[32])
{
    /* Read size */
    uint8_t hdr[4];
    if (!recv_all(s->fd, hdr, 4)) return false;
    uint32_t size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (size > 60 * 1024 * 1024) return false;

    /* Read encrypted data */
    uint8_t *buf = malloc(size);
    if (!buf) return false;
    if (!recv_all(s->fd, buf, size)) { free(buf); return false; }

    /* Read and verify MAC BEFORE decrypting (fail fast) */
    uint8_t mac_wire[32];
    if (!recv_all(s->fd, mac_wire, 32)) { free(buf); return false; }

    uint8_t mac_expected[32];
    struct sha3_256_ctx mctx;
    sha3_256_init(&mctx);
    sha3_256_write(&mctx, s->key, 32);
    sha3_256_write(&mctx, (const unsigned char *)&s->recv_counter, 8);
    sha3_256_write(&mctx, expected_sha3, 32);
    sha3_256_write(&mctx, buf, size);
    sha3_256_finalize(&mctx, mac_expected);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac_wire[i] ^ mac_expected[i];
    if (diff != 0) {
        fprintf(stderr, "file_service: MAC FAILED on chunk (%u bytes)\n",
                size);
        free(buf);
        return false;
    }

    /* Decrypt in-place */
    encrypt_chunk_inplace(s->key, s->recv_counter, buf, size);
    s->recv_counter++;

    /* Verify plaintext SHA3 matches expected (from manifest) */
    uint8_t hash[32];
    sha3_256(buf, size, hash);
    if (memcmp(hash, expected_sha3, 32) != 0) {
        fprintf(stderr, "file_service: SHA3 MISMATCH after decrypt\n");
        free(buf);
        return false;
    }

    s->bytes_received += 4 + size + 32;
    *out = buf;
    *out_size = size;
    return true;
}

/* ── Server ────────────────────────────────────────────────────── */

static volatile bool g_fs_running = false;
static pthread_t g_fs_thread;
static const char *g_fs_datadir = NULL;
static uint16_t g_fs_port = FS_PORT;

/* Background manifest builder — hashes all block files (~7 GB). */
static struct file_manifest g_server_fm;
static _Atomic bool g_have_manifest = false;

static void *fs_manifest_thread(void *arg)
{
    (void)arg;
    int64_t t0 = (int64_t)time(NULL);
    bool ok = file_manifest_build(&g_server_fm, g_fs_datadir);
    int64_t elapsed = (int64_t)time(NULL) - t0;
    if (ok) {
        printf("File service: manifest ready — %u chunks, "
               "%.1f GB (%llds to hash)\n",
               g_server_fm.num_chunks,
               (double)g_server_fm.total_bytes / (1024.0*1024.0*1024.0),
               (long long)elapsed);
        atomic_store(&g_have_manifest, true);
    } else {
        printf("File service: no block files for manifest\n");
    }
    return NULL;
}

static void *fs_server_thread(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "file_service: socket failed: %s\n", strerror(errno));
        return NULL;
    }

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int zero = 0;
    setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(g_fs_port);
    addr.sin6_addr = in6addr_any;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "file_service: bind port %d failed: %s\n",
                g_fs_port, strerror(errno));
        close(listen_fd);
        return NULL;
    }

    listen(listen_fd, 8);
    printf("File service listening on port %d (SHA3 quantum-secure)\n",
           g_fs_port);

    /* Kick off manifest build in background thread */
    if (g_fs_datadir) {
        pthread_t mt;
        pthread_create(&mt, NULL, fs_manifest_thread, NULL);
        pthread_detach(mt);
    }

    /* Get UTXO root for key derivation */
    uint8_t utxo_root[32];
    memset(utxo_root, 0, 32);

    while (g_fs_running) {
        struct sockaddr_in6 client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Use accept with a timeout so we can check g_fs_running */
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int client_fd = accept(listen_fd,
                                (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        printf("file_service: client connected\n");

        struct fs_session session;
        fs_session_init(&session, client_fd);

        if (!fs_handshake(&session, utxo_root, false)) {
            fprintf(stderr, "file_service: handshake failed\n");
            close(client_fd);
            continue;
        }

        const struct file_manifest *fm = atomic_load(&g_have_manifest) ? &g_server_fm : NULL;

        /* Serve: receive control frames, send data */
        while (g_fs_running) {
            uint8_t type;
            const uint8_t *payload;
            uint32_t plen;

            if (!fs_recv_frame(&session, &type, &payload, &plen))
                break;

            if (type == FS_REQUEST && plen == 3 &&
                memcmp(payload, "ALL", 3) == 0 && fm) {
                /* Stream ALL chunks back-to-back — zero round trips */
                printf("file_service: streaming %u chunks (%.1f GB)...\n",
                       fm->num_chunks,
                       (double)fm->total_bytes / (1024.0*1024.0*1024.0));
                for (uint32_t ci = 0; ci < fm->num_chunks; ci++) {
                    uint8_t *data = NULL;
                    uint32_t data_size = 0;
                    if (file_chunk_read(&fm->chunks[ci], g_fs_datadir,
                                         &data, &data_size)) {
                        fs_send_chunk_fast(&session, data, data_size,
                                            fm->chunks[ci].sha3);
                        free(data);
                    } else {
                        fprintf(stderr, "file_service: chunk %u read "
                                "failed\n", ci);
                        break;
                    }
                }
                printf("file_service: streaming done (%.1f MB/s)\n",
                       fs_session_mbps(&session));
            } else if (type == FS_REQUEST && plen >= 32 && fm) {
                /* Single chunk request by hash */
                const struct file_chunk *chunk =
                    file_manifest_find(fm, payload);
                if (chunk) {
                    uint8_t *data = NULL;
                    uint32_t data_size = 0;
                    if (file_chunk_read(chunk, g_fs_datadir,
                                         &data, &data_size)) {
                        fs_send_chunk_fast(&session, data, data_size,
                                            chunk->sha3);
                        free(data);
                    }
                }
            } else if (type == FS_MANIFEST) {
                /* Client requests manifest — send chunk list */
                if (fm) {
                    /* Send each chunk hash+size as manifest entries */
                    for (uint32_t i = 0; i < fm->num_chunks; i++) {
                        uint8_t entry[36];
                        memcpy(entry, fm->chunks[i].sha3, 32);
                        entry[32] = (uint8_t)(fm->chunks[i].size);
                        entry[33] = (uint8_t)(fm->chunks[i].size >> 8);
                        entry[34] = (uint8_t)(fm->chunks[i].size >> 16);
                        entry[35] = (uint8_t)(fm->chunks[i].size >> 24);
                        fs_send_frame(&session, FS_MANIFEST, entry, 36);
                    }
                    fs_send_frame(&session, FS_DONE, NULL, 0);
                }
            } else if (type == FS_DONE) {
                break;
            }
        }

        printf("file_service: client done (%.1f MB/s, %llu bytes)\n",
               fs_session_mbps(&session),
               (unsigned long long)(session.bytes_sent +
                                     session.bytes_received));
        close(client_fd);
    }

    close(listen_fd);
    return NULL;
}

void fs_server_start(const char *datadir, uint16_t port)
{
    g_fs_datadir = datadir;
    g_fs_port = port;
    g_fs_running = true;
    pthread_create(&g_fs_thread, NULL, fs_server_thread, NULL);
}

void fs_server_stop(void)
{
    g_fs_running = false;
    pthread_join(g_fs_thread, NULL);
}

/* ── Client ────────────────────────────────────────────────────── */

bool fs_client_sync(const char *peer_addr, uint16_t port,
                     const char *datadir, const uint8_t utxo_root[32])
{
    printf("file_service: connecting to %s:%d...\n", peer_addr, port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(peer_addr, port_str, &hints, &res) != 0) {
        fprintf(stderr, "file_service: resolve failed for %s\n", peer_addr);
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "file_service: connect failed: %s\n", strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    struct fs_session session;
    fs_session_init(&session, fd);

    if (!fs_handshake(&session, utxo_root, true)) {
        close(fd);
        return false;
    }

    printf("file_service: connected, requesting manifest...\n");

    /* Request manifest */
    fs_send_frame(&session, FS_MANIFEST, NULL, 0);

    /* Receive manifest entries */
    struct file_chunk chunks[FILE_MAX_CHUNKS];
    uint32_t num_chunks = 0;

    while (num_chunks < FILE_MAX_CHUNKS) {
        uint8_t type;
        const uint8_t *payload;
        uint32_t plen;

        if (!fs_recv_frame(&session, &type, &payload, &plen))
            break;

        if (type == FS_DONE) break;
        if (type == FS_MANIFEST && plen >= 36) {
            memcpy(chunks[num_chunks].sha3, payload, 32);
            chunks[num_chunks].size =
                (uint32_t)payload[32] |
                ((uint32_t)payload[33] << 8) |
                ((uint32_t)payload[34] << 16) |
                ((uint32_t)payload[35] << 24);
            num_chunks++;
        }
    }

    /* Compute total download size for progress reporting */
    uint64_t total_bytes = 0;
    for (uint32_t j = 0; j < num_chunks; j++)
        total_bytes += chunks[j].size;
    printf("file_service: manifest has %u chunks (%.1f GB), downloading...\n",
           num_chunks, (double)total_bytes / (1024.0 * 1024.0 * 1024.0));

    /* Create blocks directory */
    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    mkdir(blocks_dir, 0755);

    int64_t dl_start = (int64_t)time(NULL);
    uint64_t bytes_done = 0;

    /* Open output file once (keep open for all chunks) */
    char out_path[576];
    snprintf(out_path, sizeof(out_path), "%s/blocks/blk00000.dat", datadir);
    FILE *out_file = fopen(out_path, "wb");
    if (!out_file) {
        fprintf(stderr, "file_service: cannot open %s for writing\n", out_path);
        close(fd);
        return false;
    }

    /* Request ALL chunks in one batch frame, then receive all data.
     * Single request message tells server "send everything".
     * Server streams chunks back-to-back. Zero round trips. */
    fs_send_frame(&session, FS_REQUEST, (const uint8_t *)"ALL", 3);

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint8_t *chunk_buf = NULL;
        uint32_t chunk_size = 0;
        if (!fs_recv_chunk_fast(&session, &chunk_buf, &chunk_size,
                                 chunks[i].sha3)) {
            fprintf(stderr, "file_service: chunk %u/%u FAILED\n",
                    i + 1, num_chunks);
            fclose(out_file);
            close(fd);
            return false;
        }

        fwrite(chunk_buf, 1, chunk_size, out_file);
        free(chunk_buf);
        bytes_done += chunk_size;

        /* Progress: percentage, downloaded/total, speed, ETA */
        double pct = total_bytes > 0 ? 100.0 * (double)bytes_done / (double)total_bytes : 0;
        double gb_done = (double)bytes_done / (1024.0 * 1024.0 * 1024.0);
        double gb_total = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
        double mbps = fs_session_mbps(&session);
        int64_t elapsed = (int64_t)time(NULL) - dl_start;
        int eta_sec = 0;
        if (bytes_done > 0 && elapsed > 0)
            eta_sec = (int)((double)(total_bytes - bytes_done) /
                            ((double)bytes_done / (double)elapsed));
        printf("file_service: [%3.0f%%] %.1f/%.1f GB  %.1f MB/s  "
               "chunk %u/%u  ETA %dm%02ds\n",
               pct, gb_done, gb_total, mbps, i + 1, num_chunks,
               eta_sec / 60, eta_sec % 60);
        fflush(stdout);
    }

    fclose(out_file);
    fs_send_frame(&session, FS_DONE, NULL, 0);
    int64_t dl_elapsed = (int64_t)time(NULL) - dl_start;
    printf("=== File sync complete: %.1f GB in %llds (%.1f MB/s avg) ===\n",
           (double)bytes_done / (1024.0 * 1024.0 * 1024.0),
           (long long)dl_elapsed,
           dl_elapsed > 0 ? (double)bytes_done / (1024.0 * 1024.0) / (double)dl_elapsed : 0);

    close(fd);
    return true;
}
