# Human Call Transfer — Manual Verification Runbook

**What this proves:** during a live call, an event sent over the WebSocket
transfers the caller to a **human phone number**, dialed **through Asterisk**,
and bridges the two parties together — with the AI dropped.

**What it does NOT prove:** actual voice audio quality (the test legs are silent
Asterisk channels). For audible proof, use real phones + a working trunk, or two
softphones. Everything else — signaling, the trigger, the second-leg dial, and
the audio bridge — is exercised fully.

> Environment: Windows + Docker Desktop, run from `C:\Users\Lenovo\SIP-NEW\SipTrunking`.
> All commands are PowerShell. The media-server's API is only reachable from
> inside the container (Docker host-networking), so we call it via `docker exec`.

---

## The flow being demonstrated

```
 Phone caller ──► Asterisk ──► media-server ──(wss)──► trigger server (stand-in AI)
                                                            │ after 8s sends:
                                                            │ {"event":"humanTransfer","humanNumber":"9142436879"}
                                                            ▼
        media-server: ringback to caller, close AI socket, then
        dial sip:9142436879 THROUGH ASTERISK as a 2nd leg ──► human answers
                                                            ▼
        media-server bridges  caller  ⇄  human   (AI is gone)
```

---

## 1. Prerequisites (one-time)

**a) Containers running** (`asterisk` must be *healthy*):
```powershell
docker ps --format "table {{.Names}}\t{{.Status}}"
```
If not running:
```powershell
docker compose -f docker-compose.local.yml up -d
```

**b) media-server image includes the transfer feature.** If you just pulled the
code, rebuild it:
```powershell
docker compose -f docker-compose.local.yml build media-server
docker compose -f docker-compose.local.yml up -d media-server
```

**c) Trigger server dependencies** (one-time):
```powershell
py -m pip install -r human_transfer_trigger\requirements.txt
```

**d) Local networking fix (CRITICAL for a stable local demo).** On this dev box
Asterisk and the media-server talk over `127.0.0.1`. Asterisk must treat that as
"local", otherwise it advertises the public IP (`14.139.238.134`), in-dialog
signalling/RTP don't loop back, and the call drops after ~0.5s with
`BYE cause=408 "Request Timeout"`. Ensure `asterisk\config\pjsip.conf` has
`127.0.0.0/8` in `local_net`:
```ini
local_net=127.0.0.0/8,172.31.0.0/16,172.16.0.0/12,10.0.0.0/8,192.168.0.0/16,172.236.72.0/24
```
This is a **transport** setting — it needs an Asterisk restart (not just reload):
```powershell
docker exec asterisk asterisk -rx "core restart now"
```
(Harmless to keep in production — `127.x` really is local. Not needed once both
services run on properly routable IPs.)

---

## 2. Start the trigger server (the stand-in "AI")

The media-server only connects over **TLS (wss)**, so the trigger runs with a
self-signed cert (`human_transfer_trigger\cert.pem` / `key.pem`, already present).

In **Terminal 1**:
```powershell
cd human_transfer_trigger
py -m uvicorn main:app --host 0.0.0.0 --port 8090 --ssl-keyfile key.pem --ssl-certfile cert.pem
```
Leave it running. Confirm it's reachable from the container (in any terminal):
```powershell
docker exec media-server curl -sk https://host.docker.internal:8090/health
```
Expect: `{"status":"ok"}`

---

## 3. Enable the local auto-answer test numbers

So both call legs answer without a real PSTN trunk. Add this block directly under
the `[internal]` line in `asterisk\config\extensions.conf`:

```ini
exten => 8000000001,1,NoOp(TEST caller leg)
 same => n,Answer()
 same => n,Wait(120)
 same => n,Hangup()
exten => 9142436879,1,NoOp(TEST human leg)
 same => n,Answer()
 same => n,Wait(120)
 same => n,Hangup()
```
Reload and verify:
```powershell
docker exec asterisk asterisk -rx "dialplan reload"
docker exec asterisk asterisk -rx "dialplan show internal" | Select-String "8000000001|9142436879"
```

> ⚠️ Remove this block before committing or deploying — leaving `9142436879`
> in a dialplan facing the real trunk would hijack real calls to that number.

---

## 4. Run the test

**Terminal 2 — watch the media-server live:**
```powershell
docker logs -f media-server
```

**Terminal 3 — place the call** (JSON is read from a file; inline JSON gets
mangled by PowerShell's quoting):
```powershell
docker cp .\human_transfer_trigger\test_call_body.json media-server:/tmp/b.json
docker exec media-server curl -s -X POST http://localhost:8086/initiate-call -d "@/tmp/b.json"
```
Immediate response should be:
```json
{"call_id":"...","message":"Call initiated with direct RTP","status":"success", ...}
```

---

## 5. Success criteria ✅

Within ~10 seconds, **Terminal 2** must show this sequence:

```
Call state: CONFIRMED                                  <- caller answered
WebSocket connected                                    <- connected to trigger (AI)
Start event sent successfully
Media successfully connected
Received human Transfer event                          <- trigger fired (after 8s)
Call being transferred to human phone number: 9142436879
Starting human (phone) transfer to: 9142436879
[transferToHuman] Dialing human 9142436879 ... (trunk=testtrunk)   <- 2nd leg via Asterisk
[transferToHuman] Human leg initiated. Total calls: 2
[HumanTransfer] Caller bridged to human. Audio now flows caller <-> human.   <- ✅ PASS
```

The line **`Caller bridged to human`** is the definitive proof.

While bridged, you can also confirm two live legs:
```powershell
docker exec media-server curl -s http://localhost:8086/get-call-count
```
Expect: `"current_calls":2`

### Confirming the human leg from Asterisk's side (independent proof)

The media-server log says it bridged — to cross-check from Asterisk itself, list
live channels *during* the bridge (you have ~100s before `Wait(120)` ends):
```powershell
docker exec asterisk asterisk -rx "core show channels verbose"
docker exec asterisk asterisk -rx "core show channels verbose" | Select-String "9142436879"
```
Expected during the bridge — **two** channels, both `Up`:
```
PJSIP/india_media_dev-...  exten:9142436879  state:Up   <- the HUMAN leg (call was transferred)
PJSIP/india_media_dev-...  exten:8000000001  state:Up   <- the caller leg
```
Run it right after placing the call (only `8000000001` shows — talking to the AI),
then again ~10s later (the `9142436879` leg appears). **That transition is the proof
the call was transferred to the human.**

---

## 6. Cleanup after the demo

Remove the test block from `asterisk\config\extensions.conf`, then:
```powershell
docker exec asterisk asterisk -rx "dialplan reload"
```
Stop the trigger server with `Ctrl+C` in Terminal 1.

---

## Appendix — regenerate the TLS cert (only if missing)

Run in Git Bash (the `MSYS_NO_PATHCONV=1` avoids a Windows path-mangling bug):
```bash
cd human_transfer_trigger
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
  -days 365 -nodes -subj "/CN=host.docker.internal"
```

## Common gotchas

| Symptom | Cause / fix |
|---|---|
| `parse error ... '{p'` | PowerShell ate the inline JSON quotes — use the `-d "@/tmp/b.json"` file method |
| `-H : term not recognized` | You used bash `\` line-continuations — put the command on one line |
| `Connection failed, retrying` (WS) | You used `ws://` not `wss://` — the media-server is TLS-only |
| `http 000` to `127.0.0.1:8086` | Host can't reach the container (host-networking) — call via `docker exec` |
| Caller dials but no transfer | Trigger server not running, or test extensions not reloaded |
| Call bridges then drops ~0.5s later, `BYE cause=408` | `127.0.0.0/8` missing from `local_net` (step 1d) — Asterisk advertised the public IP; add it and `core restart now` |
| Call returns empty / `unexpected end of input` after restart | media-server not finished re-registering yet — wait for `registration success` in its logs before placing the call |
