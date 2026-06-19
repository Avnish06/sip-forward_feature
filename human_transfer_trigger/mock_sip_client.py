"""Mock of the SIP media server: connects to the trigger, sends `start`, and
verifies it receives a humanTransfer event. Validates the trigger server."""
import asyncio
import json
import sys
import websockets


async def main():
    uri = "ws://127.0.0.1:8090/voice?delay=3&number=9142436879"
    print(f"[mock] connecting to {uri}")
    async with websockets.connect(uri) as ws:
        # The real SIP service sends `start` on connect.
        await ws.send(json.dumps({
            "event": "start",
            "start": {"streamId": "MZmock", "mediaFormat": {"encoding": "audio/x-l16", "sampleRate": 8000}},
        }))
        print("[mock] sent start; waiting for humanTransfer...")

        # Trickle a few fake media frames so the server logs inbound media too.
        async def feed():
            seq = 0
            while True:
                seq += 1
                await ws.send(json.dumps({"event": "media", "sequenceNumber": str(seq),
                                          "media": {"track": "inbound", "payload": ""}}))
                await asyncio.sleep(0.02)
        feeder = asyncio.create_task(feed())

        try:
            while True:
                msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
                print(f"[mock] received: {msg}")
                if msg.get("event") == "humanTransfer":
                    feeder.cancel()
                    if msg.get("humanNumber") == "9142436879":
                        print("[mock] PASS: got humanTransfer with humanNumber=9142436879")
                        return 0
                    print("[mock] FAIL: humanTransfer missing/incorrect humanNumber")
                    return 1
        except asyncio.TimeoutError:
            print("[mock] FAIL: timed out waiting for humanTransfer")
            return 1


sys.exit(asyncio.run(main()))
