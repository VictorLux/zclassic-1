#!/bin/bash
# Copy Let's Encrypt certs into zclassic23 data dir
set -e

DATADIR="$HOME/.zclassic23"
mkdir -p "$DATADIR/ssl"

sudo cp /etc/letsencrypt/live/zclnet.net/fullchain.pem "$DATADIR/ssl/"
sudo cp /etc/letsencrypt/live/zclnet.net/privkey.pem "$DATADIR/ssl/"
sudo chown "$(whoami):$(whoami)" "$DATADIR/ssl/"*.pem
chmod 600 "$DATADIR/ssl/privkey.pem"
chmod 644 "$DATADIR/ssl/fullchain.pem"

echo "SSL certs installed:"
ls -la "$DATADIR/ssl/"

# Allow binary to bind port 443/80 without root
cd ~/zclassic23
if [ -f ./zclassic23 ]; then
    sudo setcap 'cap_net_bind_service=+ep' ./zclassic23
    echo "Port binding capability set on ./zclassic23"
fi

# Stop nginx so it doesn't conflict
sudo systemctl stop nginx 2>/dev/null && echo "nginx stopped" || true
sudo systemctl disable nginx 2>/dev/null && echo "nginx disabled" || true

echo "Done. Rebuild and restart zclassic23."
