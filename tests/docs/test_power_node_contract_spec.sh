#!/usr/bin/env bash
set -euo pipefail

spec="docs/specs/POWER_NODE_CONTRACT.md"

if [[ ! -f "$spec" ]]; then
  echo "missing $spec" >&2
  exit 1
fi

required=(
  "node_state_api"
  "service_registry"
  "onion gateway"
  "ZClassicDNS"
  "MCP surface"
  "permissions"
  "event expectations"
  "app/include/app/services.h"
  "tools/qedc_mcp/controllers/application_ctrl.h"
  "tools/qedc_mcp/models/event.h"
)

for token in "${required[@]}"; do
  if ! grep -Fq "$token" "$spec"; then
    echo "missing token in $spec: $token" >&2
    exit 1
  fi
done
