#!/usr/bin/env bash
# Tear down a provider tunnel. Usage: sudo ./down.sh <slug>
# Idempotent: safe to run even if partially up.
set -uo pipefail

SLUG="${1:-}"
[[ -z "$SLUG" ]] && { echo "usage: $0 <slug>"; exit 1; }

DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$DIR/$SLUG.env"
[[ -f "$ENV_FILE" ]] || { echo "missing $ENV_FILE"; exit 1; }
# shellcheck disable=SC1090
source "$ENV_FILE"

: "${SLUG:?}" "${PROTO:?}" "${GATEWAY:?}" "${ROUTES:?}"

IFACE="vpn_${SLUG}"

# Remove specific routes first
for cidr in $ROUTES; do
  ip route del "$cidr" via "$GATEWAY" dev "$IFACE" 2>/dev/null \
    && echo "[route] removed $cidr" || true
done

# Remove any stray default via this gateway
while ip route show default | grep -q "via $GATEWAY"; do
  ip route del default via "$GATEWAY" 2>/dev/null || break
done

case "$PROTO" in
  softether)
    if command -v vpncmd >/dev/null; then
      # Only disconnect the session. Do NOT delete the account or NIC —
      # deleting the NIC forces a new MAC on the next up, and repeated MAC
      # changes get the session quarantined by the provider's L2 switch.
      vpncmd localhost /CLIENT /CMD AccountDisconnect "$SLUG" >/dev/null 2>&1 || true
    fi
    ;;
esac

# Final assertion: nothing left pointing at this gateway
if ip route | grep -q " $GATEWAY "; then
  echo "[warn] routes still reference $GATEWAY:"
  ip route | grep " $GATEWAY "
fi

echo "[ok] $SLUG tunnel down"
