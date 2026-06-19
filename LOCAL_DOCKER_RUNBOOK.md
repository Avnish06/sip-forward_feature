# Local Docker runbook — Synchrovox (Tata) inbound + outbound

Two containers, exactly as designed:

- **`asterisk` stack** (`global-asterisk-server-docker-compose.yml`) → `asterisk` + `db` (MariaDB) + `loadbalancer` + `redis-sip`. Holds trunk credentials, registers to the provider, routes calls.
- **`media-server` stack** (`media-server-docker-compose.yml`) → `sip_outbound`. Registers INTO Asterisk, exposes the **call-trigger HTTP API on `:8086`**, bridges RTP ↔ AI WebSocket.

Trunk data (already corrected, valid JSON, in `sip_outbound/trun_operations_bulk/`):
- `synchrovox_outbound.json` — `synchrovoxai_new.com` / caller-id `00919240292847` → trunk id **`synchrovox_out_did`**
- `synchrovox_inbound.json` — `9262102714_synchrovox.com` / DID `919262102714` → endpoint **`synchrovox_in`**

---

## ⚠️ Windows reality check (read first)

Both stacks use `network_mode: host`. On Windows this only behaves correctly under the **WSL2 backend** (Docker Desktop default) — host net binds to the WSL2 Linux VM, not Windows directly.

- **Run all `docker compose` commands from a WSL2 shell** (Ubuntu), not PowerShell, for predictable host-networking. Put the repo on the WSL2 filesystem (e.g. `~/SipTrunking`) for speed.
- **Outbound calls** have a good chance of working behind home NAT because `rtp_symmetric=yes` + your IP being whitelisted lets the provider return RTP through the NAT pinhole.
- **Inbound calls** (provider → you) need the provider to reach your public IP:5060 + RTP range — i.e. **router port-forwarding** (UDP 5060 + 4000-4999) to the WSL2 VM, or a public host. Without that, registration may show as up but calls won't arrive.
- `external_media_address` / `external_signaling_address` in `asterisk/config/pjsip.conf` are hardcoded to `34.131.62.40`. **Change both to your public IP** before expecting audio.

---

## 0. One-time prep

```bash
# in WSL2, repo root:
cd SipTrunking

# 1) set your whitelisted public IP in .env
#    PUBLIC_IP=<your public ip>      (curl ifconfig.me)
nano .env

# 2) point Asterisk's advertised address at your public IP
#    edit asterisk/config/pjsip.conf:
#      external_media_address=<your public ip>
#      external_signaling_address=<your public ip>
nano asterisk/config/pjsip.conf
```

## 1. Bring up the Asterisk stack

```bash
docker compose -f global-asterisk-server-docker-compose.yml up -d --build
docker compose -f global-asterisk-server-docker-compose.yml ps     # db healthy, asterisk up
docker compose -f global-asterisk-server-docker-compose.yml exec asterisk asterisk -rx "core show version"
```

The `db` init seeds the media-server endpoint `india_media_dev` (password `secret123`) from `pjsip_config.sql` — that's what `.env`'s `SIP_USERNAME`/`SIP_PASSWORD` match.

## 2. Bring up the media-server

```bash
docker compose -f media-server-docker-compose.yml up -d --build
docker compose -f media-server-docker-compose.yml logs -f sip_outbound
# look for: "[Database] Successfully connected to MySQL" and "Registration successful"
```

Confirm it registered into Asterisk:

```bash
docker compose -f global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip show contacts"          # india_media_dev should have a contact
curl -s http://localhost:8086/health
```

## 3. Create the two trunks (writes PJSIP realtime rows live)

```bash
cd sip_outbound/trun_operations_bulk
python3 register.py synchrovox_outbound.json
python3 register.py synchrovox_inbound.json
cd ../../
```

(Or hit the API directly — same effect — e.g.)
```bash
curl -X POST http://localhost:8086/add-provider -H 'Content-Type: application/json' \
  -d '{"provider_name":"synchrovox_out","username":"synchrovoxai_new.com","password":"1kdu9sr0a3w","domain":"20.193.182.13","port":5060,"registration":true}'
curl -X POST http://localhost:8086/add-did -H 'Content-Type: application/json' \
  -d '{"trunk_name":"synchrovox_out_did","did":"00919240292847","provider_id":"synchrovox_out","answer_webhook_url":"https://api.vocallabs.ai/sip-answer-webhook"}'
```

Verify the registrations to Tata went up:

```bash
docker compose -f global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip show registrations"     # synchrovox_out / synchrovox_in => Registered
curl -s http://localhost:8086/show-accounts
```

If they show `Rejected (401/403)`: the IP isn't actually whitelisted yet, or the realm needs to be the provider domain (see troubleshooting in `tunnels/README.md`).

## 4. Trigger an OUTBOUND call

`trunk_name` = the DID-row id you created (`synchrovox_out_did`).

```bash
curl -X POST http://localhost:8086/initiate-call -H 'Content-Type: application/json' \
  -d '{
    "phone_number": "9198XXXXXXXX",
    "websocket_url": "wss://your-ai-agent/voice",
    "webhook_url":   "https://your-backend/status",
    "trunk_name":    "synchrovox_out_did",
    "sample_rate":   8000
  }'
```

(`:8000/initiate-call` on the loadbalancer does the same, then proxies to a media container.)

Trace SIP while testing:
```bash
docker compose -f global-asterisk-server-docker-compose.yml exec asterisk \
  asterisk -rx "pjsip set logger on"
```

## 5. INBOUND flow (how it routes)

A call to DID `919262102714` arrives at Asterisk → `from-trunk` context → resolves
`9262102714_synchrovox.com` via `ps_registrations.contact_user` → endpoint `synchrovox_in`
→ `ps_provider_dids` → asks the LB `GET :8000/get-inbound-contact/asterisk` for a media
container → dials it. (Requires step-0 port-forwarding to actually receive the INVITE locally.)

---

## Teardown

```bash
docker compose -f media-server-docker-compose.yml down
docker compose -f global-asterisk-server-docker-compose.yml down          # keep DB volume
# add -v to also wipe the seeded DB:  ... down -v
```

## Quick troubleshooting

| Symptom | Cause / fix |
|---|---|
| media-server can't reach DB | `ASTERISK_IP` must resolve to where MariaDB 3306 is published; use host net in WSL2, `127.0.0.1`. |
| Registration `Rejected` | IP not whitelisted, wrong password, or set `ps_auths.realm` to the provider domain instead of empty. |
| Call connects, no audio | Fix `external_media_address` to your public IP; ensure RTP 4000-4999 reachable; keep `rtp_symmetric=yes`. |
| Inbound never arrives | No port-forward of UDP 5060 + RTP to the WSL2 VM (expected on home NAT). |
| `add-did` 400 | Missing `trunk_name`/`provider_id`/`answer_webhook_url`. |
