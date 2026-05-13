/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Legacy-node JSON-RPC client.
 *
 * A tiny POSIX-sockets HTTP/1.1 JSON-RPC client used to talk to a
 * sibling zclassicd (the legacy C++ daemon) when bootstrapping our
 * tip from its already-synced datadir. Bookkeeping that was previously
 * private to header_probe_service.c lives here so other consumers
 * (legacy_body_pull, ad-hoc tooling) can share the transport without
 * forking the code.
 *
 * Design constraints:
 *   - No external HTTP/JSON deps; reuses libjson + AF_INET sockets.
 *   - Caller supplies the credentials (or asks the parser to read
 *     ~/.zclassic/zclassic.conf).
 *   - The response buffer is malloc'd by the library and ownership
 *     transfers to the caller on success — caller frees with free().
 *   - Both per-call timeout (5 s) and dynamic response growth (up
 *     to 1 MB) are baked in; this is a transport, not a streamer.
 */

#ifndef ZCL_RPC_LEGACY_RPC_CLIENT_H
#define ZCL_RPC_LEGACY_RPC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* Parse ~/.zclassic/zclassic.conf into the supplied buffers. Returns
 * true iff both rpcuser and rpcpassword were found. *out_port (if
 * non-NULL) is updated only when rpcport is present; otherwise it
 * is left untouched. */
bool legacy_rpc_parse_conf(char *out_user, size_t user_sz,
                           char *out_pass, size_t pass_sz,
                           int *out_port);

/* POST `body_json` to host:port with HTTP Basic auth user:pass and
 * receive the full response body into a newly malloc'd buffer.
 *
 * On success: *out_resp = NUL-terminated response buffer (HTTP
 * headers + blank line + JSON body). Caller must free(). Returns
 * true.
 *
 * On failure: *out_resp = NULL, err populated, returns false.
 *
 * Buffer grows up to 1 MB; larger responses fail. 5 s send + recv
 * timeout. */
bool legacy_rpc_call(const char *host, int port,
                     const char *user, const char *pass,
                     const char *body_json,
                     char **out_resp,
                     char *err, size_t err_sz);

/* Extract the HTTP body (after the first "\r\n\r\n") from a raw
 * response. Returns NULL if no separator found. */
const char *legacy_rpc_http_body(const char *raw);

#endif /* ZCL_RPC_LEGACY_RPC_CLIENT_H */
