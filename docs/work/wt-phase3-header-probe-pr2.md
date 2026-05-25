# Worker Assignment — Phase 3 header_probe PR-2: reuse legacy RPC transport

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-1 poll job shipped; C-2/C-3 shipped.
**Status: IN PROGRESS (wt3)** — claimed 2026-05-25.

## Scope

Shrink `app/services/src/header_probe_service.c` by removing the private
legacy JSON-RPC transport helpers that duplicate `lib/rpc/src/legacy_rpc_client.c`.
Keep the public header-probe API unchanged.

## Tasks

1. Replace the private zclassic.conf parser, Basic-auth base64 encoder, and
   POSIX HTTP client helpers with `legacy_rpc_parse_conf`,
   `legacy_rpc_call`, and `legacy_rpc_http_body`.
2. Preserve current behavior for fixed-buffer single calls and dynamic batched
   calls, including existing timeout/cap semantics inherited from the shared
   transport.
3. Run focused header-probe tests plus the normal lint/build gates.

## Acceptance

- `ZCL_TEST_ONLY=header_probe ./test_zcl` PASS.
- `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` PASS.
- `make -j$(nproc)` PASS.
- `make lint` PASS.
- `./test_parallel --jobs=$(nproc)` PASS before completion.

## Notes

This is the first PR-2 shrink slice. It does not rename the service or delete
the blocking pull API; those remain follow-up PR-2/PR-3 work after this
transport duplication is gone.
