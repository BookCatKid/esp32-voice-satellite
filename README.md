# ESP32-S3 Voice Satellite

A small ESP32-S3 voice satellite for the existing Pi 5 voice-assistant stack. The XIAO captures an INMP441 microphone and plays assistant speech through a MAX98357A. Both share a stable, crystal-sourced 22.05 kHz I2S clock; the Pi resamples microphone audio to 16 kHz for wake word, VAD and STT.

## Hardware

| Function | Module pin | XIAO pin | GPIO |
| --- | --- | --- | ---: |
| Microphone BCLK | INMP441 SCK | D8 | 7 |
| Microphone LRCLK | INMP441 WS | D9 | 8 |
| Microphone data | INMP441 SD | D7 | 44 |
| Speaker BCLK | MAX98357A BCLK | D8 | 7 |
| Speaker LRCLK | MAX98357A LRC | D9 | 8 |
| Speaker data | MAX98357A DIN | D6 | 43 |

The INMP441 channel is detected automatically at boot. The tested module is active on the left I2S slot.

## Transport

There is no private byte-stream protocol. Firmware and receiver communicate over one RFC 6455 WebSocket:

```text
ESP32-S3                                    Pi 5
   │                                          │
   ├── HTTP Upgrade + Bearer auth ───────────>│
   ├── subprotocol voice-satellite.v1 ───────>│
   ├── JSON hello ───────────────────────────>│
   │<──────────────────────────── JSON ready ─┤
   │                                          │
   ├── binary PCM16 mic frames ──────────────>│ wake/VAD → STT → agent
   │                                          │
   │<──────────── JSON playback.start + PCM ──┤ TTS
   │<──────────────────── JSON playback.end ──┤
   └── JSON stats / standard ping-pong ──────>│
```

The receiver serves everything from port `8766`:

- `/` — dashboard
- `/health` — health JSON
- `/api/status` — live satellite/pipeline state
- `/api/audio` — last generated TTS audio
- `/api/protocol` — machine-readable transport description
- `/ws/satellite` — authenticated WebSocket media/control endpoint

See [PROTOCOL.md](PROTOCOL.md) for the small application message schema. WebSocket itself provides message boundaries, fragmentation, ping/pong, reconnect behavior and TCP backpressure; we do not layer our own lengths or line commands on top.

## Code layout

- `main/audio_io.*` owns duplex I2S configuration and PCM conversion. The
  firmware orchestrator does not know about DMA layout or microphone slots.
- `receiver/audio.py` owns the satellite and inference PCM formats, resampling,
  metering, TTS decoding and software gain.
- `receiver/speaker.py` owns serialized WebSocket playback, pacing,
  cancellation and speaker state.
- `receiver/server.py` owns wake/VAD and the STT → agent → TTS workflow.

## Audio pipeline

```text
INMP441
  → ESP32-S3 I2S
  → PCM16 / 22.05 kHz / mono
  → WebSocket
  → Pi resampler (16 kHz)
  → Pi openWakeWord (Hey Jarvis)
  → VAD
  → existing stt-guard / Groq Whisper
  → Open WebUI Voice Assistant workspace
  → existing Home Assistant + web tools
  → Edge TTS
  → PCM16 WebSocket downlink
  → ESP32-S3 I2S
  → MAX98357A
```

Microphone uplink is muted while the local speaker is playing. That prevents the assistant from recursively hearing itself until proper echo cancellation is added.

## Configuration

Private values live in `main/voice_satellite_secrets.h`, which is gitignored. It contains the Wi-Fi credentials, satellite bearer token and receiver WebSocket URI. Do not commit it.

The receiver uses `/home/pi5/docker/voice-satellite/.env` on the Pi for its bearer token and Open WebUI service credential.

## Build

PlatformIO uses ESP-IDF 6 for the Seeed XIAO ESP32-S3:

```bash
pio run -e xiao_esp32s3
```

Managed components are pinned by `main/idf_component.yml` and `dependencies.lock`; `managed_components/` is generated and ignored.

When the board is connected:

```bash
pio run -e xiao_esp32s3 -t upload
pio device monitor -p /dev/cu.usbmodem101 -b 115200
```

The exact standalone speaker configuration verified on the target hardware is
preserved in `hardware-tests/speaker-smoke/`. Keep it as a known-good fallback
when changing the production duplex audio path.

## Pi receiver

The deployed service is mirrored in `receiver/` and runs as the `voice-satellite` Docker container on the Pi. Rebuild with:

```bash
cd /home/pi5/docker/voice-satellite
docker compose up -d --build
```

The dashboard is available on LAN/Tailscale through port `8766`.
