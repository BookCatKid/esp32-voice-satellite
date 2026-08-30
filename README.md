# ESP32-S3 Voice Satellite

Private room voice-assistant stack built around a Seeed XIAO ESP32-S3,
INMP441 microphone, Raspberry Pi receiver, Open WebUI voice workspace, and an
I2S speaker amplifier.

This repository contains the whole satellite project:

- `main/` — ESP-IDF firmware for microphone capture, Wi-Fi streaming, and
  speaker downlink.
- `receiver/server.py` — Raspberry Pi audio receiver, wake-word/VAD pipeline,
  STT/agent/TTS integration, and speaker downlink.
- `receiver/dashboard.html` — standalone dashboard frontend served by the Pi
  backend.
- `receiver/docker-compose.yml` + `Dockerfile` — receiver deployment.
- `receiver/provision_openwebui.py` — least-privilege Open WebUI service-user
  and model-access provisioning.

Secrets are deliberately not stored in Git. See the `.example` files.

## Architecture

```text
INMP441
  │ 16 kHz I2S
  ▼
ESP32-S3 ── authenticated PCM/TCP ──► Raspberry Pi receiver
  ▲                                      │
  │                                      ├─ openWakeWord / VAD
  │                                      ├─ existing STT guard / Whisper
MAX98357A                                ├─ Open WebUI Voice Assistant
  ▲                                      │    ├─ Home Assistant MCP
  │ PCM/TCP                              │    └─ web/search tools
  └──────────────────────────────────────└─ TTS

Dashboard: receiver HTTP :8766
Audio + speaker protocol: TCP :8765
```

The dashboard shows connection state, live microphone level, wake-word/VAD
state, transcript/response, timing, errors, recent turns, and TTS playback.

## Current hardware mapping

### INMP441 microphone

| INMP441 | XIAO | ESP32-S3 GPIO |
| --- | --- | ---: |
| SCK / BCLK | D8 | 7 |
| WS / LRCLK | D9 | 8 |
| SD | D7 | 44 |
| VDD | 3V3 | — |
| GND | GND | — |

The firmware samples both I2S slots at startup and automatically chooses the
active microphone channel.

### MAX98357A speaker amp

| MAX98357A | XIAO | ESP32-S3 GPIO |
| --- | --- | ---: |
| BCLK | D8 | 7 |
| LRC / WS | D9 | 8 |
| DIN | D6 | 43 |

BCLK and WS are shared with the microphone.

## Firmware setup

The project uses PlatformIO with the ESP-IDF framework and the
`seeed_xiao_esp32s3` board definition.

```sh
cp main/voice_satellite_secrets.example.h main/voice_satellite_secrets.h
```

Fill in Wi-Fi, the Pi's LAN IPv4 address, and a long random satellite token.
The same token goes in the Pi receiver's `.env`.

Build:

```sh
pio run
```

Flash and monitor when the ESP32 is connected:

```sh
pio run -t upload
pio device monitor -b 115200
```

## Pi receiver setup

The deployed receiver expects the existing Docker network `ai-stack` and the
following services on that network:

- `stt-guard:8890` — existing VAD/STT/TTS gateway.
- `open-webui:8080` — Open WebUI.
- Open WebUI workspace `voice-assistant-ui`, based on `voice-assistant`.

Create the receiver env file:

```sh
cd receiver
cp .env.example .env
```

Generate a satellite token, for example:

```sh
openssl rand -hex 24
```

Put it in `receiver/.env` and in `main/voice_satellite_secrets.h`.

### Open WebUI service identity

The receiver uses a dedicated non-admin service account. Open WebUI's global
API-key setting does not need to be enabled. Run `provision_openwebui.py`
inside the Open WebUI backend environment and capture its stdout directly into
a protected file; it creates/updates only the service user, base-model metadata,
and read grants for the Voice Assistant workspace.

The resulting signed token belongs in `OPENWEBUI_API_KEY` in `receiver/.env`.

### Start the receiver

```sh
docker compose up -d --build
```

Then open:

```text
http://<pi-address>:8766
```

The TCP audio endpoint is authenticated with `SATELLITE_TOKEN`; the dashboard
never exposes that token or the Open WebUI credential.

## Current voice path

The receiver streams 16 kHz mono PCM from the ESP32, performs local
wake-word/VAD handling on the Pi, sends accepted utterances through the
existing speech-to-text service, invokes the same Open WebUI Voice Assistant
workspace used elsewhere in the setup, generates TTS, and sends PCM back to
the ESP32 speaker connection.

The firmware mutes upstream microphone PCM during local speaker playback to
avoid immediately feeding TTS back into the assistant. USB mic metering still
measures the physical signal while playback is active.

## Security / repository policy

Never commit:

- Wi-Fi SSIDs/passwords when they are private.
- `SATELLITE_TOKEN` production values.
- Open WebUI signed tokens/API credentials.
- generated `sdkconfig.*` or PlatformIO build artifacts.

Those paths are covered by `.gitignore`; keep using the checked-in example
files for documentation.
