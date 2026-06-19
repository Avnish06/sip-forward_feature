#!/usr/bin/env bash
# Health table for all configured tunnels. No args.
set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"

printf "%-10s %-14s %-6s %-8s %-20s\n" SLUG IFACE LINK ROUTES SIP_PROBE
printf -- "-%.0s" {1..70}; echo

shopt -s nullglob
for env in "$DIR"/*.env; do
  # shellcheck disable=SC1090
  ( source "$env"
    : "${SLUG:?}" "${SIP_SERVER:?}"
    IFACE="vpn_${SLUG}"

    LINK="down"
    ip link show "$IFACE" &>/dev/null && LINK="up"

    ROUTES_OK="missing"
    if ip route show | grep -q "dev $IFACE"; then ROUTES_OK="ok"; fi

    SIP="fail"
    if ! command -v sipsak >/dev/null; then
      SIP="n/a"
    else
      sipsak -s "sip:${SIP_SERVER}" -v --timeout 2 >/dev/null 2>&1 && SIP="ok"
    fi

    printf "%-10s %-14s %-6s %-8s %-20s\n" "$SLUG" "$IFACE" "$LINK" "$ROUTES_OK" "$SIP"
  )
done
