# Tunneled-provider trunks

Some SIP providers (BSNL, enterprise carriers, MPLS-only ITSPs) don't expose
their SIP server on the public internet. They require a site-to-site tunnel
(SoftEther, IPsec, WireGuard) to reach a private address like `10.191.5.1`.

This directory isolates all tunnel concerns from the application. The Go
loadbalancer, the C++ `sip_outbound`, and the Asterisk dialplan know nothing
about tunnels — they just see another trunk in the DB.

## Invariants (do not break)

1. The machine's **default route stays on the public NIC**. A tunnel only ever
   carries `/32` or narrow `/29` routes listed in its `.env`.
2. Every tunneled trunk uses `direct_media='no'` in `ps_endpoints`. Media is
   anchored through Asterisk so RTP rides the tunnel.
3. Secrets (VPN + SIP passwords) live in `secrets/<slug>.secret`, gitignored.
   Never commit them to the `.env`.
4. App services (`sip_outbound`, `loadbalancer`) get no tunnel logic added.

## Files

| File | Purpose |
|---|---|
| `<slug>.env` | Per-provider config: concentrator, vhub, customer IP, routes, SIP params. Committed. |
| `secrets/<slug>.secret` | `VPN_PASSWORD`, `SIP_PASSWORD` for the provider. Gitignored. |
| `up.sh <slug>` | Brings up `vpn_<slug>`, adds only the listed routes, rolls back on default-route hijack. |
| `down.sh <slug>` | Tears down cleanly and idempotently. |
| `status.sh` | Prints a one-line health row per tunnel (link / routes / SIP OPTIONS probe). |
| `trunk_template.sql` | 4-row INSERT template for Asterisk PJSIP realtime. |

## Onboarding a new tunneled provider

### 1. Gather provider info

- VPN concentrator address + vhub/tunnel id + auth username/password
- Private IP the provider assigns you + mask + gateway
- Subnets you're allowed to route to (SIP server + media range)
- SIP auth username, `From` domain, and password
- Confirm provider has **whitelisted your public IP** — without this the
  concentrator rejects the handshake.

### 2. Create config

```bash
cp bsnl.env <slug>.env         # edit for the new provider
```

For **password auth** (`VPN_AUTH=standard` in `.env`):

```bash
cat > secrets/<slug>.secret <<EOF
VPN_PASSWORD='...'
SIP_PASSWORD='...'
EOF
chmod 600 secrets/<slug>.secret
```

For **certificate auth** (`VPN_AUTH=cert` in `.env`, as BSNL uses):

```bash
mkdir -p secrets/<slug>
cp /path/to/client.crt secrets/<slug>/client.crt
cp /path/to/client.key secrets/<slug>/client.key
chmod 600 secrets/<slug>/*

cat > secrets/<slug>.secret <<EOF
VPN_CERT=$(pwd)/secrets/<slug>/client.crt
VPN_KEY=$(pwd)/secrets/<slug>/client.key
SIP_PASSWORD='...'
EOF
chmod 600 secrets/<slug>.secret
```

### 3. Bring up the tunnel

```bash
sudo ./up.sh <slug>
./status.sh
```

`up.sh` snapshots the default route before touching anything. If the tunnel
pushes a default route, it auto-rolls-back and exits non-zero.

### 4. Sanity-check routing isolation

```bash
ip route get <provider-sip-ip>   # must go via vpn_<slug>, source = customer IP
ip route get 8.8.8.8              # must still go via public NIC, unchanged
```

### 5. Insert the trunk in Asterisk

```bash
SLUG=<slug>
# shellcheck disable=SC1090
source ./$SLUG.env
source ./secrets/$SLUG.secret

sed \
  -e "s|TRUNK_NAME|$TRUNK_NAME|g" \
  -e "s|SIP_USERNAME|$SIP_USERNAME|g" \
  -e "s|SIP_PASSWORD|$SIP_PASSWORD|g" \
  -e "s|SIP_FROM_DOMAIN|$SIP_FROM_DOMAIN|g" \
  -e "s|SIP_SERVER|$SIP_SERVER|g" \
  -e "s|CALLER_ID|$CALLER_ID|g" \
  trunk_template.sql | \
docker compose -f ../global-asterisk-server-docker-compose.yml exec -T db \
  mariadb -uasterisk -p'f3mb0yTw1nkyB@!!69' asterisk

docker compose -f ../global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip reload"

docker compose -f ../global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip show registrations"      # must show Registered
```

### 6. Test a call

```bash
curl -X POST http://localhost:8000/initiate-call \
  -H 'Content-Type: application/json' \
  -d '{
    "phone_number": "919xxxxxxxxx",
    "websocket_url": "wss://your-test-agent/voice",
    "webhook_url": "https://your-backend/status",
    "trunk_name": "<trunk_name>",
    "sample_rate": 8000
  }'
```

Trace SIP if needed:

```bash
docker compose -f ../global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip set logger on"
```

## Removing a tunneled provider

```bash
SLUG=<slug>
source ./$SLUG.env

# 1. Remove trunk from Asterisk
docker compose -f ../global-asterisk-server-docker-compose.yml exec -T db \
  mariadb -uasterisk -p'f3mb0yTw1nkyB@!!69' asterisk <<SQL
DELETE FROM trunk_webhooks     WHERE trunk_id='$TRUNK_NAME';
DELETE FROM ps_registrations   WHERE id='${TRUNK_NAME}_registration';
DELETE FROM ps_aors            WHERE id='${TRUNK_NAME}_aor';
DELETE FROM ps_auths           WHERE id='${TRUNK_NAME}_auth';
DELETE FROM ps_endpoints       WHERE id='$TRUNK_NAME';
SQL
docker compose -f ../global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip reload"

# 2. Tear down the tunnel
sudo ./down.sh $SLUG

# 3. Verify nothing lingers
./status.sh
ip route | grep "$GATEWAY"    # should be empty
```

## Supported protocols

Set `PROTO=` in the `.env`. Currently implemented:

- `softether`

Planned (add a `case` branch in `up.sh`/`down.sh` when a real provider needs it):

- `wireguard`
- `openvpn`
- `ipsec`

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `up.sh` rolls back saying "default route changed" | SoftEther pushed a DHCP default. Disable SecureNAT on the vhub side or configure the account without DHCP. |
| `pjsip show registrations` shows `Rejected` 401/403 | Wrong SIP password or realm. Try setting `realm` on `ps_auths` to the provider's domain instead of empty. |
| Call connects but no audio at all | `direct_media` still `'yes'`. Anchored media is mandatory for tunneled trunks — fix the endpoint row. |
| Call connects, one-way audio | Firewall dropping RTP on the tunnel interface, or codec mismatch. Keep `allow='ulaw,alaw'`. |
| `status.sh` shows `SIP_PROBE=fail` after days of uptime | Tunnel silently died. `vpncmd localhost /CLIENT /CMD AccountStatusGet <slug>` and reconnect. |
| New trunk's registration stays `Registering` forever | Source IP not whitelisted by provider, or `SIP_SERVER` unreachable. Re-run `status.sh`. |
| New trunk's registration never appears in `pjsip show registrations` | `max_retries` on `ps_registrations` is `-1` (means "don't retry"). Set to `10`. |
| Outbound `403 Forbidden` without a `401` challenge | Trunk not activated on provider side, or PAI/RPID leaking internal identity. Set `send_pai='no'`, `send_rpid='no'` on the endpoint. |
