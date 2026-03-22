#!/bin/bash
# One-time deployment setup for ZClassic23.
# Run once with: sudo bash deploy/setup.sh
#
# After this, 'make deploy' works without sudo ever again.
set -e

TARGET_USER="${SUDO_USER:-$(whoami)}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_DIR/zclassic23"
SERVICE_DIR="$(eval echo ~$TARGET_USER)/.config/systemd/user"

echo "Setting up ZClassic23 for user: $TARGET_USER"
echo "Binary: $BINARY"

# Passwordless setcap — this is the one sudo you'll ever need
echo "$TARGET_USER ALL=(root) NOPASSWD: /usr/sbin/setcap cap_net_bind_service=+ep $BINARY" \
    > /etc/sudoers.d/zclassic23-setcap
chmod 440 /etc/sudoers.d/zclassic23-setcap

# Enable linger (service survives logout) — may already be enabled
loginctl enable-linger "$TARGET_USER" 2>/dev/null || true

# Set capability on current binary if it exists
if [ -f "$BINARY" ]; then
    setcap 'cap_net_bind_service=+ep' "$BINARY"
fi

# Install service file (best-effort — user may need to run systemctl themselves)
mkdir -p "$SERVICE_DIR"
install -m 644 "$REPO_DIR/deploy/zclassic23.service" "$SERVICE_DIR/zclassic23.service"
su - "$TARGET_USER" -c "systemctl --user daemon-reload && systemctl --user enable zclassic23" 2>/dev/null || \
    echo "Note: run 'systemctl --user daemon-reload && systemctl --user enable zclassic23' as $TARGET_USER"

echo "Done. 'make deploy' will now work without sudo."
