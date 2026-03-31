#!/bin/bash
systemctl stop zclassic23
systemctl disable zclassic23
rm -f /etc/systemd/system/zclassic23.service
systemctl daemon-reload
echo "System-level zclassic23 service removed. User linger service is now the only one."
