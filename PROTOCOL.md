# Voice Satellite WebSocket protocol

The application protocol is deliberately small. The ESP32-S3 is an input-only microphone satellite; all inference and speech processing live on the Pi.

## Endpoint and authentication

Connect to `/ws/satellite` using:

- WebSocket subprotocol: `voice-satellite.v1`
- HTTP header: `Authorization: Bearer <satellite token>`

A missing or invalid token is rejected before the WebSocket upgrade.

## Session negotiation

Immediately after the upgrade, the satellite sends a JSON text message:

```json
{
  "type": "hello",
  "protocol": 1,
  "device": {"id": "xiao-s3-…", "firmware": "0.2.0"},
  "capabilities": ["microphone"],
  "audio_in": {
    "codec": "pcm_s16le",
    "sample_rate": 16000,
    "channels": 1,
    "frame_ms": 20
  }
}
```

The receiver validates the format and replies with `ready` plus wake-word metadata. Unsupported versions or formats are closed rather than guessed.

## Binary media

Binary messages flow only from the satellite to the receiver and contain microphone `pcm_s16le`, 16 kHz, mono audio.

The sender uses 20 ms (320-sample / 640-byte) messages for low latency. Receivers use WebSocket message boundaries rather than adding another length header.

## Control events

| Direction | Type | Purpose |
| --- | --- | --- |
| ESP → Pi | `hello` | negotiate protocol and microphone format |
| Pi → ESP | `ready` | accept session and report wake-word mode |
| ESP → Pi | `stats` | RSSI / firmware telemetry |

Normal WebSocket ping/pong is enabled by both implementations, so no application heartbeat is required.

## Compatibility

Breaking message changes require a new numeric `protocol` and, for a transport-level break, a new WebSocket subprotocol name. Unknown JSON fields should be ignored. Unknown event types may be ignored safely.
