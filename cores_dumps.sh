#!/bin/bash
set -e

# Ensure we’re root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (sudo $0)"
  exit 1
fi

CORE_DIR="/cores"
CORE_PATTERN="${CORE_DIR}/core.%e.%p.%t"

echo "[INFO] Setting core dump pattern to: $CORE_PATTERN"
sysctl -w kernel.core_pattern="$CORE_PATTERN"

mkdir -p "$CORE_DIR"
chmod 777 "$CORE_DIR"

# Persist the setting
if ! grep -q "kernel.core_pattern" /etc/sysctl.conf; then
  echo "kernel.core_pattern=$CORE_PATTERN" >> /etc/sysctl.conf
else
  sed -i "s|^kernel\.core_pattern=.*|kernel.core_pattern=$CORE_PATTERN|" /etc/sysctl.conf
fi

sysctl -p
echo "[INFO] Core dump configuration complete."
