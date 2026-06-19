#!/usr/bin/env bash
# Bring up a provider tunnel. Usage: sudo ./up.sh <slug>
# SAFETY: snapshots the default route and rolls back if the tunnel hijacks it.
set -euo pipefail

SLUG="${1:-}"
[[ -z "$SLUG" ]] && { echo "usage: $0 <slug>"; exit 1; }

DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$DIR/$SLUG.env"
SECRET_FILE="$DIR/secrets/$SLUG.secret"

[[ -f "$ENV_FILE" ]]    || { echo "missing $ENV_FILE"; exit 1; }
[[ -f "$SECRET_FILE" ]] || { echo "missing $SECRET_FILE"; exit 1; }

# shellcheck disable=SC1090
source "$ENV_FILE"
# shellcheck disable=SC1090
source "$SECRET_FILE"

: "${SLUG:?}" "${PROTO:?}" "${CUSTOMER_IP:?}" "${CUSTOMER_CIDR:?}" "${GATEWAY:?}" "${ROUTES:?}"

IFACE="vpn_${SLUG}"

# Snapshot default route BEFORE any changes
DEFAULT_BEFORE="$(ip route show default | head -1 || true)"
echo "[pre] default route: ${DEFAULT_BEFORE:-<none>}"

rollback() {
  echo "[rollback] tearing down $IFACE"
  "$DIR/down.sh" "$SLUG" || true
  exit 1
}
trap rollback ERR

case "$PROTO" in
  softether)
    command -v vpncmd >/dev/null || { echo "vpncmd not in PATH (install SoftEther client)"; exit 1; }
    : "${CONCENTRATOR:?}" "${VHUB:?}" "${VPN_USERNAME:?}"
    VPN_AUTH="${VPN_AUTH:-standard}"

    vpncmd localhost /CLIENT /CMD NicCreate "$SLUG" 2>/dev/null || true
    vpncmd localhost /CLIENT /CMD AccountCreate "$SLUG" \
      /SERVER:"$CONCENTRATOR" /HUB:"$VHUB" \
      /USERNAME:"$VPN_USERNAME" /NICNAME:"$SLUG" 2>/dev/null || true

    case "$VPN_AUTH" in
      standard)
        : "${VPN_PASSWORD:?VPN_PASSWORD required for VPN_AUTH=standard}"
        vpncmd localhost /CLIENT /CMD AccountPasswordSet "$SLUG" \
          /PASSWORD:"$VPN_PASSWORD" /TYPE:standard >/dev/null
        ;;
      cert)
        : "${VPN_CERT:?VPN_CERT path required for VPN_AUTH=cert}"
        : "${VPN_KEY:?VPN_KEY path required for VPN_AUTH=cert}"
        [[ -r "$VPN_CERT" ]] || { echo "cannot read VPN_CERT: $VPN_CERT"; exit 1; }
        [[ -r "$VPN_KEY"  ]] || { echo "cannot read VPN_KEY: $VPN_KEY"; exit 1; }
        vpncmd localhost /CLIENT /CMD AccountCertSet "$SLUG" \
          /LOADCERT:"$VPN_CERT" /LOADKEY:"$VPN_KEY" >/dev/null
        ;;
      *)
        echo "unknown VPN_AUTH=$VPN_AUTH (use 'standard' or 'cert')"; exit 1
        ;;
    esac

    # Connect only if not already established — makes the script safe to re-run anytime
    if ! vpncmd localhost /CLIENT /CMD AccountStatusGet "$SLUG" 2>/dev/null | grep -q "Session Established"; then
      vpncmd localhost /CLIENT /CMD AccountConnect "$SLUG" >/dev/null
    fi
    ;;
  wireguard|openvpn|ipsec)
    echo "PROTO=$PROTO not implemented yet"; exit 1
    ;;
  *)
    echo "unknown PROTO=$PROTO"; exit 1
    ;;
esac

# Wait up to 10s for the interface to appear
for i in {1..10}; do
  ip link show "$IFACE" &>/dev/null && break
  sleep 1
done
ip link show "$IFACE" &>/dev/null || { echo "interface $IFACE never came up"; exit 1; }

# Static IP + up
ip addr replace "${CUSTOMER_IP}/${CUSTOMER_CIDR}" dev "$IFACE"
ip link set "$IFACE" up

# Add ONLY the listed routes — never 0.0.0.0/0
for cidr in $ROUTES; do
  ip route replace "$cidr" via "$GATEWAY" dev "$IFACE"
  echo "[route] $cidr via $GATEWAY dev $IFACE"
done

# CRITICAL: default route must be unchanged
DEFAULT_AFTER="$(ip route show default | head -1 || true)"
if [[ "${DEFAULT_BEFORE:-}" != "${DEFAULT_AFTER:-}" ]]; then
  echo "[FATAL] default route changed:"
  echo "  before: ${DEFAULT_BEFORE:-<none>}"
  echo "  after:  ${DEFAULT_AFTER:-<none>}"
  rollback
fi

# Belt & suspenders: purge any stray default via this gateway
while ip route show default | grep -q "via $GATEWAY"; do
  ip route del default via "$GATEWAY" || break
done

trap - ERR
echo "[ok] $SLUG tunnel up on $IFACE, routes=$ROUTES"
