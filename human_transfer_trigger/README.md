# Human Transfer (phone number, bridged via Asterisk)

Transfer a live AI call to a **real human phone number** mid-call. A few seconds
into the call the AI side sends a `humanTransfer` event; the media server then
dials the human through Asterisk, plays ringback to the caller, and bridges
caller ↔ human audio once the human answers.

This is different from the legacy "rupture" human transfer (a WebSocket
switchover to `wss://rupture2.*/ws`). That path still works and is unchanged —
it fires when the event carries `humanId` instead of `humanNumber`.

## The event contract

The AI/agent WebSocket server sends one of these down the socket to the media
server (handled in `sip_outbound/src/websocket_handler.cpp`):

```jsonc
// NEW: dial + bridge a real phone via Asterisk
{ "event": "humanTransfer", "humanNumber": "9142436879" }

// LEGACY: switch the audio socket over to the rupture human-agent server
{ "event": "humanTransfer", "humanId": "agent-123" }
```

If both are present, `humanNumber` wins.

## End-to-end flow (phone-number transfer)

```
Phone ──RTP──► Asterisk ──► media-server ──WS──► AI agent (FastAPI here)
                                                      │  after N seconds:
                                                      │  {"event":"humanTransfer","humanNumber":"9142436879"}
                                                      ▼
   media-server: clearAudio + ringback to caller, close AI WebSocket
                                                      │
                                  CallManager::transferToHumanNumber()
                                                      │  new outbound INVITE (X-Trunk = caller's trunk)
                                                      ▼
                          Asterisk ──► human phone (9142436879) rings
                                                      │  human answers (CONFIRMED + media active)
                                                      ▼
                  MyCall::bridgeWithPeer(): splice caller ⇄ human in the conf bridge,
                                            detach the AI/ringback media port
                                                      ▼
                              Caller and human are now talking directly.
```

Teardown is mutual: if either the caller or the human hangs up, the other leg is
hung up too (`bridged_leg` / `bridge_peer` pointers in `MyCall`).

## Files involved

| Layer | File | What changed |
|-------|------|--------------|
| Trigger (new) | `human_transfer_trigger/main.py` | FastAPI WS server; sends the transfer event after a delay |
| Event routing | `sip_outbound/src/websocket_handler.cpp` | `humanNumber` branch → `performHumanNumberTransfer()` |
| Media port | `sip_outbound/src/audio_port.cpp` | `startHumanNumberTransfer()` → CallManager |
| Orchestration | `sip_outbound/src/call_manager.cpp` | `transferToHumanNumber()` dials the human leg |
| Bridge logic | `sip_outbound/src/call.cpp` | bridge-leg lifecycle + `bridgeWithPeer()` / `detachWsMediaForBridge()` |

## Run the trigger server

```bash
cd human_transfer_trigger
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8090
```

Config (env or per-connection query string):

| Setting | Env var | Query param | Default |
|---------|---------|-------------|---------|
| Seconds before transfer | `TRANSFER_DELAY_SEC` | `delay` | `7` |
| Human phone number | `HUMAN_NUMBER` | `number` | `9142436879` |
| Mode (`number` / `rupture`) | `TRANSFER_MODE` | `mode` | `number` |

## Trigger a test call

Point the media server at this server as the `websocket_url`:

```bash
curl -X POST http://127.0.0.1:8086/initiate-call \
  -H 'Content-Type: application/json' \
  -d '{
    "phone_number": "<caller-number>",
    "websocket_url": "ws://<this-host>:8090/voice?delay=8&number=9142436879",
    "webhook_url": "https://example.com/status",
    "trunk_name": "<your-trunk>",
    "sample_rate": 8000
  }'
```

Watch `media-server` logs for: `Received human Transfer event` →
`[transferToHuman] Dialing human ...` → `[HumanTransfer] Caller bridged to human`.

## Rebuild the media server after the C++ changes

```bash
docker compose -f docker-compose.local.yml build media-server
docker compose -f docker-compose.local.yml up -d media-server
```

## Known limitations / follow-ups

- **No fallback to AI.** If the human is busy / doesn't answer, the bridge leg's
  disconnect tears the caller down. A nicer behavior is to reconnect the AI or
  play a "could not connect" message — not implemented yet.
- **Recording.** After the bridge, the caller's WS media port is detached, so the
  existing recorder (which lives on that port) stops capturing. Recording the
  bridged conversation needs a separate tap.
- **Caller-ID to the human.** The human leg currently reuses the same From/Contact
  as a normal outbound call (`+919484952308`). Adjust in
  `CallManager::transferToHumanNumber` if the human should see the original
  caller's number.
