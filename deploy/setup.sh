#!/bin/bash
# One-time deployment setup for ZClassic23.
# Run as root: sudo bash deploy/setup.sh
#
# Enables:
#   - passwordless setcap for port 443 binding
#   - systemd user linger service
set -e

# Detect the user who invoked sudo (or current user)
TARGET_USER="${SUDO_USER:-$(whoami)}"
TARGET_HOME=$(eval echo "~$TARGET_USER")
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO_DIR/build/zclassic23"
SERVICE_SRC="$REPO_DIR/deploy/zclassic23.service"
SERVICE_DIR="$TARGET_HOME/.config/systemd/user"

echo "Setting up ZClassic23 for user: $TARGET_USER"
echo "Repo: $REPO_DIR"

# Allow setcap without password for deploy target
echo "$TARGET_USER ALL=(root) NOPASSWD: /usr/sbin/setcap cap_net_bind_service=+ep $BINARY" \
    > /etc/sudoers.d/zclassic23-setcap
chmod 440 /etc/sudoers.d/zclassic23-setcap

# Enable linger (service runs without login session)
loginctl enable-linger "$TARGET_USER"

# Install service file
mkdir -p "$SERVICE_DIR"
install -m 644 "$SERVICE_SRC" "$SERVICE_DIR/zclassic23.service"
su - "$TARGET_USER" -c "systemctl --user daemon-reload && systemctl --user enable zclassic23"

# Set capability on current binary if it exists
if [ -f "$BINARY" ]; then
    setcap 'cap_net_bind_service=+ep' "$BINARY"
fi

echo "Done. Use 'make deploy' to build and restart."
