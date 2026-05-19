#!/usr/bin/env bash
# Lag-SLO observability: the mirror service writes lag-related atomics
# that gate the redundancy guarantees (concurrent catchup, severity
# ladder, sd_notify heartbeat). Every read of those atomics that could
# influence routing or severity MUST be paired with an EV_LAG_SLO_BREACH
# / EV_MIRROR_CONCURRENT_CATCHUP emit somewhere in the same file, so
# downstream consumers (Prometheus, node_health, MCP) can react to the
# state change instead of silently observing it.
#
# Concrete rule for this gate: legacy_mirror_sync_service.c must contain
# at least one EV_LAG_SLO_BREACH emission AND at least one
# EV_MIRROR_CONCURRENT_CATCHUP emission. If a refactor removes either,
# the redundancy guarantee falls back to "silent failure" and this gate
# is what prevents that regression from shipping unnoticed.

set -euo pipefail

cd "$(dirname "$0")/../.."

LMS_FILE="app/services/src/legacy_mirror_sync_service.c"

if [ ! -f "$LMS_FILE" ]; then
    echo "FAIL: $LMS_FILE not found"
    exit 1
fi

if ! grep -q "EV_LAG_SLO_BREACH" "$LMS_FILE"; then
    echo "$LMS_FILE: missing EV_LAG_SLO_BREACH emission"
    echo ""
    echo "FAIL: legacy_mirror_sync_service.c must emit EV_LAG_SLO_BREACH"
    echo "      so node_health, sd_notify, and Prometheus can react to"
    echo "      lag SLO breaches. Add a paired emit when crossing the"
    echo "      breach_blocks / critical_blocks thresholds."
    exit 1
fi

if ! grep -q "EV_MIRROR_CONCURRENT_CATCHUP" "$LMS_FILE"; then
    echo "$LMS_FILE: missing EV_MIRROR_CONCURRENT_CATCHUP emission"
    echo ""
    echo "FAIL: legacy_mirror_sync_service.c must emit EV_MIRROR_CONCURRENT_CATCHUP"
    echo "      so dashboards can show 'redundancy engaged'. Add a paired"
    echo "      emit when applied>0 with lag >= breach_blocks."
    exit 1
fi

# The chain_advance_coordinator must honor the mirror_lag_sla_breach_blocks
# field. If a refactor drops the field from cac_plan_input or removes
# the concurrent-redundancy override, this gate fails.
CAC_FILE="app/services/src/chain_advance_coordinator.c"
if [ ! -f "$CAC_FILE" ]; then
    echo "FAIL: $CAC_FILE not found"
    exit 1
fi

if ! grep -q "mirror_lag_sla_breach_blocks" "$CAC_FILE"; then
    echo "$CAC_FILE: missing mirror_lag_sla_breach_blocks check"
    echo ""
    echo "FAIL: chain_advance_coordinator must honor"
    echo "      in->mirror_lag_sla_breach_blocks in mirror_fallback_allowed()."
    echo "      Without it, the mirror is gated strictly behind local"
    echo "      retries — exactly the bug we shipped this gate to prevent."
    exit 1
fi

echo "  OK: lag SLO emit + concurrent-redundancy override present"
