# Worker Assignment — Phase 3 header_probe PR-2: reuse legacy RPC transport

**Worktree:** wt3
**Branch:** PUSH DIRECT TO MAIN
**Phase:** 3 (Dissolve mega-modules)
**Depends on:** header_probe PR-1 poll job shipped; C-2/C-3 shipped.
**Status: DONE — pushed 2026-05-25** to main as commits `bbc63530e`
and `27abadd47`.

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

## Completion (wt3, 2026-05-25)

### Summary

`header_probe_service.c` now reuses `lib/rpc/src/legacy_rpc_client.c` for
zclassic.conf parsing, Basic-auth encoding, and HTTP JSON-RPC transport.
The public header-probe API and polling job behavior are unchanged.

### Benchmark moved

Advances the Phase 3 mega-module dissolve / memory-uptime track by deleting
307 duplicated transport lines from `header_probe_service.c` (1251 -> 963 LOC)
and consolidating legacy RPC behavior behind one shared implementation.

### Commits

- `bbc63530e` wt3: claim header probe pr2
- `27abadd47` header_probe: reuse legacy rpc client

### Files modified

- `docs/work/wt-phase3-header-probe-pr2.md`
- `app/services/src/header_probe_service.c`

### Acceptance verification

- [x] `make -j$(nproc)` — PASS
- [x] `ZCL_TEST_ONLY=header_probe ./test_zcl` — PASS, 0 failures
- [x] `ZCL_TEST_ONLY=header_probe_poll ./test_zcl` — PASS, 0 failures
- [x] `make lint` — PASS; gate #20 remains WARN with grandfathered raw-controller-SQL violations
- [x] `git diff --check` — PASS
- [x] `./test_parallel --jobs=$(nproc)` — PASS, 0/208 groups failed

### Follow-ups

Continue PR-2 by moving header-specific peer scoring out of
`header_probe_service.c`, then trim or retire the blocking pull API when the
remaining callers have moved to async stage/job paths.
