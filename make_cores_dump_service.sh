#!/bin/bash
set -e

SERVICE_NAME="cores-dump-setup"
SCRIPT_PATH="/usr/local/bin/cores_dumps.sh"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

# --- STEP 1: Check for root ---
if [ "$EUID" -ne 0 ]; then
  echo "[ERROR] Please run this script as root (sudo $0)"
  exit 1
fi

# --- STEP 2: Ensure cores_dumps.sh exists ---
if [ ! -f "./cores_dumps.sh" ]; then
  echo "[ERROR] cores_dumps.sh not found in current directory."
  echo "Please run this script from the folder where cores_dumps.sh is located."
  exit 1
fi

# --- STEP 3: Move cores_dumps.sh to /usr/local/bin ---
echo "[INFO] Installing cores_dumps.sh to ${SCRIPT_PATH}..."
cp ./cores_dumps.sh "$SCRIPT_PATH"
chmod +x "$SCRIPT_PATH"

# --- STEP 4: Create systemd unit file ---
echo "[INFO] Creating systemd service at ${SERVICE_FILE}..."
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Setup core dump configuration for containers
After=local-fs.target

[Service]
Type=oneshot
ExecStart=${SCRIPT_PATH}
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

# --- STEP 5: Enable and start the service ---
echo "[INFO] Enabling and starting ${SERVICE_NAME}.service..."
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}.service"
systemctl start "${SERVICE_NAME}.service"

# --- STEP 6: Verify status ---
echo "[INFO] Checking service status..."
if systemctl is-active --quiet "${SERVICE_NAME}.service"; then
  echo "[SUCCESS] ${SERVICE_NAME}.service is active and ran successfully."
else
  echo "[WARNING] ${SERVICE_NAME}.service did not start correctly. Check logs below:"
  systemctl status "${SERVICE_NAME}.service" --no-pager
fi

echo "[INFO] To verify after reboot, run:"
echo "       sudo systemctl status ${SERVICE_NAME}.service"
echo "       or check journal logs via: sudo journalctl -u ${SERVICE_NAME}.service"
