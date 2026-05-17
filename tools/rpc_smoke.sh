#!/usr/bin/env bash
# Wave 9a CI gate: call every registered RPC method against the live node
# and report any that return -32601 ("Method not found"). This is the
# end-to-end pair to the boot-time `rpc_table_must_append` abort: even
# if registration succeeds, this confirms the method is genuinely
# dispatchable from a CLI client.
#
# Method names are extracted from the source — every entry like
#   { "category", "method_name", rpc_fn, true|false }
# in app/controllers/src/*.c or lib/*/src/*.c.
#
# Exit codes:
#   0  — every registered method returned anything other than -32601
#   1  — at least one method returned -32601 (or the RPC port is down)
#
# Usage: ./tools/rpc_smoke.sh [rpc_tool]

set -uo pipefail

cd "$(dirname "$0")/.."

RPC_TOOL="${1:-./zclassic-cli}"

if [ ! -x "$RPC_TOOL" ]; then
    echo "FAIL: $RPC_TOOL not executable — build the CLI first"
    exit 1
fi

# Sanity probe — if the node isn't up, fail fast.
if ! "$RPC_TOOL" getblockcount >/dev/null 2>&1; then
    echo "FAIL: RPC port not reachable via $RPC_TOOL — start the node first"
    exit 1
fi

# Extract registered method names from registrar arrays. The pattern is
# stable across all 22 register_*_rpc_commands functions:
#     { "<category>", "<method>", rpc_<fn>, <bool> },
methods=$(grep -hE '^\s*\{\s*"[a-z][a-z0-9_-]*"\s*,\s*"[a-zA-Z_][a-zA-Z0-9_]*"\s*,\s*rpc_[a-zA-Z_][a-zA-Z0-9_]*' \
    app/controllers/src/*.c lib/*/src/*.c 2>/dev/null \
    | sed -E 's/^\s*\{\s*"[^"]+"\s*,\s*"([^"]+)".*/\1/' \
    | sort -u)

if [ -z "$methods" ]; then
    echo "FAIL: no registered RPC methods found via source scan"
    exit 1
fi

total=0
not_found=()
errored=()

# Methods with required arguments — skip the probe (we only check
# whether the method is REGISTERED, not whether it succeeds with empty
# params). A -32602 (Invalid params) means dispatch worked, so we treat
# anything except -32601 as success.
while IFS= read -r m; do
    total=$((total + 1))
    out=$("$RPC_TOOL" "$m" 2>&1 || true)
    if echo "$out" | grep -q -- '-32601\|Method not found'; then
        not_found+=("$m")
    elif echo "$out" | grep -q -- 'error code:.*-32601'; then
        not_found+=("$m")
    fi
done <<< "$methods"

echo "  Probed $total registered RPC methods"

if [ ${#not_found[@]} -gt 0 ]; then
    echo ""
    echo "FAIL: ${#not_found[@]} methods returned -32601 (Method not found):"
    for m in "${not_found[@]}"; do
        echo "      - $m"
    done
    echo ""
    echo "      These are registered in source but unreachable via $RPC_TOOL."
    echo "      Check for: dispatcher allowlist filters, category-based"
    echo "      auth gates, or registration ordering bugs."
    exit 1
fi

echo "  OK: every registered RPC method is reachable"
