"""
Human-transfer trigger server.

This is a minimal FastAPI WebSocket server that plays the role of the "AI agent"
endpoint that the SIP media server (sip_outbound) connects to. Its only job for
this test is to keep the audio stream alive for a few seconds and then send a
`humanTransfer` event down the socket so the media server transfers the live
caller to a human phone number through Asterisk.

Protocol (see SipTrunking/README.md section 3):

  SIP service -> us (we RECEIVE):
    start         : sent once on connect, defines the stream
    media         : 20ms PCM frame captured from the caller's line
    dtmf          : keypad digit
    playedStream  : playback checkpoint ack

  us -> SIP service (we SEND):
    playAudio     : audio to play to the caller
    clearAudio    : flush playback buffer
    hangup        : end the call
    sendDtmf      : emit DTMF toward the provider
    humanTransfer : hand the call off to a human

How the media server reads the transfer event (see websocket_handler.cpp):
  {"event": "humanTransfer", "humanNumber": "9142436879"}  -> dial + bridge a real phone via Asterisk
  {"event": "humanTransfer", "humanId": "agent-123"}        -> legacy rupture WebSocket switchover

Run:
    pip install -r requirements.txt
    uvicorn main:app --host 0.0.0.0 --port 8090

Point the media server at it by passing this as `websocket_url` in /initiate-call, e.g.
    ws://<this-host>:8090/voice
    ws://<this-host>:8090/voice?delay=10&number=9142436879
"""

import asyncio
import base64
import math
import os
import struct
import time

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

app = FastAPI(title="Human Transfer Trigger")

# --- Defaults (overridable per-connection via query string) ---------------
DEFAULT_DELAY_SEC = float(os.getenv("TRANSFER_DELAY_SEC", "7"))
DEFAULT_HUMAN_NUMBER = os.getenv("HUMAN_NUMBER", "9142436879")
# "number" -> send {"humanNumber": ...} (dial a real phone via Asterisk)
# "rupture" -> send {"humanId": ...}    (legacy rupture WebSocket switchover)
DEFAULT_MODE = os.getenv("TRANSFER_MODE", "number")


@app.get("/health")
async def health():
    return {"status": "ok"}


async def _trigger_transfer(ws: WebSocket, delay: float, mode: str, number: str):
    """After `delay` seconds, send the transfer event once."""
    try:
        await asyncio.sleep(delay)
        if mode == "rupture":
            event = {"event": "humanTransfer", "humanId": number, "name": "human"}
        else:
            event = {"event": "humanTransfer", "humanNumber": number, "name": "human"}
        await ws.send_json(event)
        print(f"[transfer] sent -> {event}", flush=True)
    except asyncio.CancelledError:
        pass
    except Exception as exc:  # connection may already be gone
        print(f"[transfer] failed to send transfer event: {exc}", flush=True)


@app.websocket("/voice")
async def voice(ws: WebSocket):
    # Per-connection config from query string, falling back to env defaults.
    qp = ws.query_params
    delay = float(qp.get("delay", DEFAULT_DELAY_SEC))
    number = qp.get("number", DEFAULT_HUMAN_NUMBER)
    mode = qp.get("mode", DEFAULT_MODE)

    await ws.accept()
    print(
        f"[ws] connected (delay={delay}s mode={mode} number={number}) "
        f"query={dict(qp)}",
        flush=True,
    )

    # Fire the one-shot transfer trigger in the background.
    transfer_task = asyncio.create_task(_trigger_transfer(ws, delay, mode, number))

    media_frames = 0
    started_logged = False
    last_log = time.monotonic()

    try:
        while True:
            msg = await ws.receive_json()
            event = msg.get("event")

            if event == "start":
                fmt = msg.get("start", {}).get("mediaFormat", {})
                print(
                    f"[ws] start: streamId={msg.get('start', {}).get('streamId')} "
                    f"format={fmt}",
                    flush=True,
                )
                started_logged = True

            elif event == "media":
                media_frames += 1
                # Throttle: log a heartbeat ~once per second instead of every 20ms frame.
                now = time.monotonic()
                if now - last_log >= 1.0:
                    print(f"[ws] inbound media frames so far: {media_frames}", flush=True)
                    last_log = now

            elif event == "dtmf":
                print(f"[ws] dtmf: {msg.get('dtmf')}", flush=True)

            elif event == "playedStream":
                print(f"[ws] playedStream: {msg.get('name')}", flush=True)

            else:
                print(f"[ws] other event: {msg}", flush=True)

    except WebSocketDisconnect as exc:
        print(f"[ws] disconnected: code={exc.code}", flush=True)
    except Exception as exc:
        print(f"[ws] error: {exc}", flush=True)
    finally:
        transfer_task.cancel()
        print(f"[ws] closed. total inbound media frames={media_frames}", flush=True)


async def _play_human_audio(ws: WebSocket, sample_rate: int):
    """Simulate the human speaking: stream a continuous 440 Hz tone back to the
    media server as 20ms `playAudio` frames (16-bit LE PCM, base64). In a real
    call the media server plays this to the caller and the recorder captures it
    as the agent/human stream. `HUMAN_PLAYBACK=off` disables this.
    """
    if os.getenv("HUMAN_PLAYBACK", "on").lower() in ("off", "0", "false", "no"):
        print("[human] playback disabled (HUMAN_PLAYBACK=off)", flush=True)
        return

    samples_per_frame = (sample_rate // 1000) * 20  # 160 @ 8k, 320 @ 16k
    phase = 0.0
    phase_step = 2.0 * math.pi * 440.0 / sample_rate
    amplitude = 8000  # ~ -12 dBFS, clearly audible if recorded
    sent = 0
    last_log = time.monotonic()
    try:
        while True:
            buf = bytearray()
            for _ in range(samples_per_frame):
                buf += struct.pack("<h", int(amplitude * math.sin(phase)))
                phase += phase_step
                if phase > 2.0 * math.pi:
                    phase -= 2.0 * math.pi
            await ws.send_json({
                "event": "playAudio",
                "media": {
                    "payload": base64.b64encode(bytes(buf)).decode("ascii"),
                    "contentType": "audio/x-l16",
                    "sampleRate": sample_rate,
                },
            })
            sent += 1
            now = time.monotonic()
            if now - last_log >= 1.0:
                print(f"[human] sent playAudio frames so far: {sent}", flush=True)
                last_log = now
            await asyncio.sleep(0.02)
    except asyncio.CancelledError:
        pass
    except Exception as exc:  # socket closed mid-send
        print(f"[human] playback stopped: {exc}", flush=True)


@app.websocket("/human")
async def human(ws: WebSocket):
    """Stand-in for the human-agent side of a rupture/`humanId` transfer.

    After the media server fires {"event":"humanTransfer","humanId":...} it closes
    the AI socket and reconnects here (when HUMAN_TRANSFER_WS_URL points at this
    route). It logs the inbound audio (caller -> human) AND streams synthetic
    audio back (human -> caller) so the full duplex path can be exercised.
    """
    qp = ws.query_params
    sample_rate = int(qp.get("sampleRate", os.getenv("SAMPLE_RATE", "8000")))
    await ws.accept()
    print(f"[human] connected query={dict(qp)}", flush=True)

    media_frames = 0
    last_log = time.monotonic()

    # Talk back: stream the "human voice" tone to the media server.
    play_task = asyncio.create_task(_play_human_audio(ws, sample_rate))

    try:
        while True:
            msg = await ws.receive_json()
            event = msg.get("event")

            if event == "start":
                fmt = msg.get("start", {}).get("mediaFormat", {})
                print(
                    f"[human] start: streamId={msg.get('start', {}).get('streamId')} "
                    f"format={fmt}",
                    flush=True,
                )
            elif event == "media":
                media_frames += 1
                now = time.monotonic()
                if now - last_log >= 1.0:
                    print(f"[human] inbound media frames so far: {media_frames}", flush=True)
                    last_log = now
            elif event == "dtmf":
                print(f"[human] dtmf: {msg.get('dtmf')}", flush=True)
            else:
                print(f"[human] other event: {msg}", flush=True)

    except WebSocketDisconnect as exc:
        print(f"[human] disconnected: code={exc.code}", flush=True)
    except Exception as exc:
        print(f"[human] error: {exc}", flush=True)
    finally:
        play_task.cancel()
        print(f"[human] closed. total inbound media frames={media_frames}", flush=True)
