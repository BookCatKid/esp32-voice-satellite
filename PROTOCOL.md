# Voice Satellite WebSocket protocol

The application protocol is deliberately small. Transport semantics are delegated to RFC 6455 rather than reimplemented.

## Endpoint and authentication

Connect to `/ws/satellite` using:

- WebSocket subprotocol: `voice-satellite.v1`
- HTTP header: `Authorization: Bearer <satellite token>`

A missing/invalid token is rejected before the WebSocket upgrade.

## Session negotiation

Immediately after the upgrade, the satellite sends a JSON text message:

```json
{
  "type": "hello",
  "protocol": 1,
  "device": {"id": "xiao-s3-…", "firmware": "0.2.0"},
  "capabilities": ["microphone", "speaker"],
  "audio_in": {
    "codec": "pcm_s16le",
    "sample_rate": 16000,
    "channels": 1,
    "frame_ms": 20
  }
}
```

The receiver validates the format and replies with `ready`, including the negotiated speaker format, volume and wake-word mode. Unsupported versions/formats are closed rather than guessed.

## Binary media

Binary messages are audio; direction determines the stream:

- satellite → receiver: microphone `pcm_s16le`, 16 kHz, mono
- receiver → satellite: speaker `pcm_s16le`, 16 kHz, mono

The current sender uses 20 ms (320-sample / 640-byte) messages for low latency. Receivers should use WebSocket message boundaries rather than inventing a second length header.

Speaker binary messages only occur between `playback.start` and `playback.end` JSON events. The satellite may acknowledge completion with `playback.done`.

## Control events

JSON text messages are extensible objects identified by `type`. Current messages are:

| Direction | Type | Purpose |
| --- | --- | --- |
| ESP → Pi | `hello` | negotiate protocol/capabilities |
| Pi → ESP | `ready` | accept session and report output format |
| ESP → Pi | `stats` | RSSI / firmware telemetry |
| Pi → ESP | `volume` | set logical speaker volume |
| Pi → ESP | `playback.start` | begin speaker PCM stream |
| Pi → ESP | `playback.end` | finish speaker PCM stream |
| Pi → ESP | `playback.cancel` | discard pending speaker audio |
| ESP → Pi | `playback.done` | speaker queue drained |

Normal WebSocket ping/pong is enabled by both implementations, so no application heartbeat is required.

## Compatibility

Breaking message changes require a new numeric `protocol` and, for a transport-level break, a new WebSocket subprotocol name. Unknown JSON fields should be ignored. Unknown event types may be ignored safely.
