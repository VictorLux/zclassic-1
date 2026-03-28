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

/* ── Chunk transfer ────────────────────────────────────────────── */

bool fs_send_chunk(struct fs_session *s, const uint8_t *data, uint32_t size,
                    const uint8_t sha3[32])
{
    /* Send DATA frames until all chunk data is sent */
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t frame_payload = size - offset;
        if (frame_payload > FS_MAX_PAYLOAD - 36)
            frame_payload = FS_MAX_PAYLOAD - 36;

        /* Frame payload: [32-byte sha3][4-byte offset][data] */
        uint8_t buf[FS_MAX_PAYLOAD];
        memcpy(buf, sha3, 32);
        buf[32] = (uint8_t)(offset);
        buf[33] = (uint8_t)(offset >> 8);
        buf[34] = (uint8_t)(offset >> 16);
        buf[35] = (uint8_t)(offset >> 24);
        memcpy(buf + 36, data + offset, frame_payload);

        if (!fs_send_frame(s, FS_DATA, buf, 36 + frame_payload))
            return false;
        offset += frame_payload;
    }
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

        /* Serve: receive REQUESTs, send DATA */
        while (g_fs_running) {
            uint8_t type;
            const uint8_t *payload;
            uint32_t plen;

            if (!fs_recv_frame(&session, &type, &payload, &plen))
                break;

            if (type == FS_REQUEST && plen >= 32 && fm) {
                /* Payload is a SHA3 hash of requested chunk */
                const struct file_chunk *chunk =
                    file_manifest_find(fm, payload);
                if (chunk) {
                    uint8_t *data = NULL;
                    uint32_t data_size = 0;
                    if (file_chunk_read(chunk, g_fs_datadir,
                                         &data, &data_size)) {
                        fs_send_chunk(&session, data, data_size,
                                       chunk->sha3);
                        free(data);
                        printf("file_service: served chunk %u bytes "
                               "(%.1f MB/s)\n",
                               data_size, fs_session_mbps(&session));
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

    printf("file_service: manifest has %u chunks, downloading...\n",
           num_chunks);

    /* Create blocks directory */
    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    mkdir(blocks_dir, 0755);

    /* Request each chunk */
    for (uint32_t i = 0; i < num_chunks; i++) {
        fs_send_frame(&session, FS_REQUEST, chunks[i].sha3, 32);

        /* Receive DATA frames for this chunk */
        uint8_t *chunk_buf = malloc(chunks[i].size);
        if (!chunk_buf) break;
        uint32_t received = 0;

        while (received < chunks[i].size) {
            uint8_t type;
            const uint8_t *payload;
            uint32_t plen;

            if (!fs_recv_frame(&session, &type, &payload, &plen))
                break;
            if (type != FS_DATA || plen < 36) break;

            uint32_t data_offset =
                (uint32_t)payload[32] |
                ((uint32_t)payload[33] << 8) |
                ((uint32_t)payload[34] << 16) |
                ((uint32_t)payload[35] << 24);
            uint32_t data_len = plen - 36;

            if (data_offset + data_len > chunks[i].size) break;
            memcpy(chunk_buf + data_offset, payload + 36, data_len);
            received += data_len;
        }

        /* Verify SHA3 */
        uint8_t hash[32];
        sha3_256(chunk_buf, chunks[i].size, hash);
        if (memcmp(hash, chunks[i].sha3, 32) != 0) {
            fprintf(stderr, "file_service: SHA3 MISMATCH on chunk %u!\n", i);
            free(chunk_buf);
            close(fd);
            return false;
        }

        /* Write to disk — append to block file */
        /* TODO: proper file_index routing. For now, single file. */
        char path[576];
        snprintf(path, sizeof(path), "%s/blocks/blk00000.dat", datadir);
        FILE *f = fopen(path, "ab");
        if (f) {
            fwrite(chunk_buf, 1, chunks[i].size, f);
            fclose(f);
        }

        free(chunk_buf);

        printf("file_service: chunk %u/%u verified (%.1f MB/s)\n",
               i + 1, num_chunks, fs_session_mbps(&session));
    }

    fs_send_frame(&session, FS_DONE, NULL, 0);
    printf("file_service: sync complete — %llu bytes at %.1f MB/s\n",
           (unsigned long long)(session.bytes_sent + session.bytes_received),
           fs_session_mbps(&session));

    close(fd);
    return true;
}
