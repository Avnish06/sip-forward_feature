# SIP Telephony Integration Documentation

This document specifies the integration details for the SIP Trunking service, including API endpoints, WebSocket protocol for media streaming, and specific event flows for features like human transfer and DTMF.

## 1. REST API

### Initiate a Call
To start an outbound call, send a POST request to the load balancer (or SIP container).

**Endpoint**: `POST /****-****`  
**Content-Type**: `application/json`

**Request Body:**
```json
{
    "phone_number": "919876543210",
    "websocket_url": "wss://your-ai-agent.com/voice",
    "webhook_url": "https://your-backend.com/status",
    "trunk_name": "twilio-trunk-1",
    "sample_rate": 8000
}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `phone_number` | String | Required | Destination number (E.164 format recommended). |
| `websocket_url` | String | Required | The wss endpoint the SIP service will connect to for streaming audio. |
| `webhook_url` | String | Optional | URL to receive call lifecycle events (answered, completed, etc.). |
| `trunk_name` | String | Required | The configuration ID of the SIP trunk to use. |
| `sample_rate` | Integer | `8000` | Audio sampling rate in Hz. Supports `8000` or `16000`. |

## 2. Audio & Media Specifications

The system relies on a WebSocket connection to stream full-duplex audio between the Telephony provider and the AI Agent.

| Feature | Specification | Notes |
|---------|---------------|-------|
| **Encoding** | `audio/x-l16` (PCM) <br> `audio/x-mulaw` | **PCM (Linear 16-bit)** is the primary format used by the SIP service. We also support receiving Mulaw, which is internally converted. |
| **Sample Rate** | 8000 Hz or 16000 Hz | Must match the `sample_rate` provided in the initiate call request. |
| **Bit Depth** | 16-bit | Signed Little Endian. |
| **Frame Duration**| **20 ms** | The system processes audio in 20 millisecond chunks. |
| **Packet Size** | **160 bytes** (at 8000Hz PCM)<br>**320 bytes** (at 16000Hz PCM) | Calculated as: `SampleRate * Duration * BitDepth/8`. <br>e.g. $8000 \times 0.02 \times 2 = 320$ bytes. |
| **Audio Queue** | Max 3000 frames | Internal buffer cap. At 20ms/frame, this represents ~60 seconds of buffered audio. Overflows are dropped to maintain realtime latency. |

## 3. WebSocket Protocol

The SIP Service acts as a **WebSocket Client**. Upon call answer, it connects to the provided `websocket_url`.

### Client -> Server (SIP Service Sending)

**1. `start`**
Sent immediately on connection to define the stream parameters.
```json
{
  "event": "start",
  "start": {
    "streamId": "MZ...",
    "mediaFormat": {
      "encoding": "audio/x-l16",
      "sampleRate": 8000
    }
  }
}
```

**2. `media`**
Contains the raw audio captured from the phone line (user's voice).
```json
{
  "event": "media",
  "sequenceNumber": "101",
  "media": {
    "track": "inbound",
    "chunk": "1",
    "contentType": "audio/x-l16",
    "timestamp": "1712345678000",
    "payload": "base64_encoded_pcm_bytes"
  },
  "streamId": "MZ..."
}
```

**3. `dtmf`**
Sent when the user presses a keypad digit (0-9, *, #).
```json
{
  "event": "dtmf",
  "dtmf": {
    "digit": "5",
    "streamId": "MZ..."
  }
}
```

**4. `playedStream`**
Sent to acknowledge that a specific checkpoint has been reached in playback (audio queue emptied).
```json
{
  "event": "playedStream",
  "streamId": "uuid-from-checkpoint",
  "name": "checkpoint-name"
}
```

### Server -> Client (SIP Service Receiving)

**1. `playAudio`**
Send audio for the SIP service to play to the user.
```json
{
  "event": "playAudio",
  "media": {
    "contentType": "audio/x-l16",
    "sampleRate": 8000,
    "payload": "base64_encoded_audio"
  }
}
```

**2. `clearAudio`**
Interrupts the current speaker by clearing the playback buffer immediately.
```json
{ "event": "clearAudio" }
```

**3. `hangup`**
Terminates the call.
```json
{
  "event": "hangup",
  "hangupType": "completed"
}
```

**4. `sendDtmf`**
Instructs the SIP service to output DTMF tones (e.g., navigating an IVR).
```json
{
  "event": "sendDtmf",
  "text": "123"
}
```

**5. `humanTransfer`**
Initiates the handoff procedure to a human agent. Two variants:

```json
// (a) Phone-number transfer: dial a real human via Asterisk and bridge the caller to them.
{
  "event": "humanTransfer",
  "humanNumber": "9142436879"
}

// (b) Rupture transfer: switch the audio WebSocket over to the human-agent server.
{
  "event": "humanTransfer",
  "humanId": "agent-123"
}
```

If both fields are present, `humanNumber` takes precedence. See
[`human_transfer_trigger/README.md`](human_transfer_trigger/README.md) for the
phone-number flow and a FastAPI server that triggers it.

## 4. Workflows

### Human Transfer Flow — phone number (bridged via Asterisk)
1. **Trigger**: AI sends `humanTransfer` with a `humanNumber`.
2. **Audio Interrupt + Ringback**: SIP service clears the AI audio queue and plays ringback (queue music) to the caller.
3. **Close AI socket**: the AI WebSocket is closed (no reconnect; transfer-in-progress flag prevents call teardown).
4. **Dial human**: `CallManager::transferToHumanNumber` places a new outbound INVITE to `sip:<humanNumber>@asterisk` using the caller's trunk (`X-Trunk`).
5. **Bridge**: when the human answers and media goes active, the caller's audio is spliced to the human leg in PJSIP's conference bridge and the AI/ringback media port is detached.
6. **Teardown**: a hangup on either leg tears down the other.

### Human Transfer Flow — rupture (WebSocket switchover)
1. **Trigger**: AI sends `humanTransfer` event with a `humanId`.
2. **Audio Interrupt**: SIP service immediately clears audio queue and stops playing AI audio.
3. **Ringing**: SIP service locally generates a "ringing" tone to play to the user.
4. **Switchover**:
   - SIP service closes the current WebSocket connection (`close status: normal`).
   - SIP service connects to the human agent WebSocket URL:
     `wss://rupture2.vocallabs.ai/ws?callId=...`
5. **Resume**: Once connected to the new socket, bi-directional audio resumes between user and human agent.

### DTMF Flow
- **Incoming (User -> AI)**:
  1. PJSIP detects tone on the RTP stream.
  2. SIP service converts to JSON `dtmf` event.
  3. JSON sent to WebSocket.
- **Outgoing (AI -> User)**:
  1. AI sends `sendDtmf` JSON event: `{"text": "9"}`.
  2. SIP service parses the digit.
  3. SIP service injects the corresponding DTMF tone into the RTP stream towards the telephony provider.



# FD analysis
The 6 New File Descriptors Per Call:

1. Media Transport Sockets (2 FDs)
When a call is established, PJMEDIA dynamically allocates a pair of UDP ports for the audio stream:

    FD 29: UDP port 4022 — This is the RTP (Real-time Transport Protocol) socket handling the actual voice media.

    FD 31: UDP port 4023 — This is the associated RTCP (RTP Control Protocol) socket used for statistics and QoS.

2. Asynchronous Event & Timer Loop (3 FDs)
Your application creates a dedicated event loop to manage the asynchronous tasks, timeouts, and media polling for the active session without blocking the main threads:

    FD 37: eventfd — Used for signaling events or waking up the call's thread.

    FD 39: timerfd — A dedicated timer, likely handling SIP session timers (like Session-Expires), retransmissions, or RTCP scheduling.

    FD 38: eventpoll — An epoll instance specifically grouped to monitor the call's event and timer FDs (it is explicitly monitoring FDs 37, 39, and likely the media sockets).

3. API / Webhook Connection (1 FD)

    There is a net increase of 1 active TCP HTTPS connection to an external IP (routing through Cloudflare). In the 1_call state, you have an extra established connection (like FD 40 or FD 45) compared to the idle state. This is highly characteristic of your sip_outbound app firing an HTTP request to log a CDR (Call Detail Record), authenticate the outbound route, or trigger a webhook mid-call.