#!/usr/bin/env python3
import asyncio
import collections
import io
import json
import math
import os
import re
import struct
import time
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

import numpy as np
import miniaudio
from openwakeword.model import Model as WakeWordModel


SAMPLE_RATE = 16000
FRAME_SAMPLES = 320
FRAME_BYTES = FRAME_SAMPLES * 2
FRAME_MS = 20

AUDIO_PORT = int(os.getenv("AUDIO_PORT", "8765"))
HTTP_PORT = int(os.getenv("HTTP_PORT", "8766"))
SATELLITE_TOKEN = os.environ["SATELLITE_TOKEN"]
STT_BASE = os.getenv("STT_BASE", "http://stt-guard:8890/v1").rstrip("/")
OPENWEBUI_BASE = os.getenv("OPENWEBUI_BASE", "http://open-webui:8080").rstrip("/")
OPENWEBUI_API_KEY = os.environ.get("OPENWEBUI_API_KEY", "")
OPENWEBUI_MODEL = os.getenv("OPENWEBUI_MODEL", "voice-assistant-ui")
OPENWEBUI_CHAT_ID = os.environ.get("OPENWEBUI_CHAT_ID", "")
OPENWEBUI_TOOL_IDS = ["server:9router-web-tools", "server:mcp:homeassistant"]
OPENWEBUI_CONTEXT_MESSAGES = int(os.getenv("OPENWEBUI_CONTEXT_MESSAGES", "10"))
WAKEWORD_MODEL = os.getenv("WAKEWORD_MODEL", "hey_jarvis")
WAKEWORD_LABEL = os.getenv("WAKEWORD_LABEL", "Hey Jarvis")
WAKEWORD_THRESHOLD = float(os.getenv("WAKEWORD_THRESHOLD", "0.50"))
WAKEWORD_ARM_SECONDS = float(os.getenv("WAKEWORD_ARM_SECONDS", "8"))
SPEAKER_VOLUME_DEFAULT = max(0, min(100, int(os.getenv("SPEAKER_VOLUME_DEFAULT", "1"))))

_wake_words = [re.escape(part) for part in WAKEWORD_LABEL.split() if part]
WAKEWORD_PREFIX_RE = re.compile(
    r"^\s*" + r"\s+".join(_wake_words) + r"\b[\s,.:;!?\-]*",
    re.IGNORECASE,
) if _wake_words else None

DASHBOARD = Path(__file__).with_name("dashboard.html").read_bytes()
STARTED = time.time()

status = {
    "connected": False,
    "peer": None,
    "connected_at": None,
    "last_frame_at": None,
    "bytes_received": 0,
    "dbfs": -120.0,
    "peak_dbfs": -120.0,
    "noise_dbfs": -72.0,
    "vad_threshold_dbfs": -60.0,
    "speaking": False,
    "queue_depth": 0,
    "processing": False,
    "agent_ready": False,
    "wake_ready": False,
    "wake_model": WAKEWORD_MODEL,
    "wake_label": WAKEWORD_LABEL,
    "wake_score": 0.0,
    "wake_hits": 0,
    "ignored_utterances": 0,
    "last_wake_at": None,
    "wake_error": "",
    "utterances": 0,
    "last_transcript": "",
    "last_response": "",
    "last_language": "",
    "last_error": "",
    "last_utterance_ms": 0,
    "latency": {"stt_ms": None, "agent_ms": None, "tts_ms": None, "total_ms": None},
    "history": [],
    "last_audio_version": 0,
    "speaker_connected": False,
    "speaker_volume": SPEAKER_VOLUME_DEFAULT,
    "speaker_playing": False,
    "speaker_playback_ms": 0,
    "speaker_bytes_sent": 0,
}

last_tts_audio = b""
utterance_queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=3)
wake_model = None
wake_armed_until = 0.0
speaker_writer = None
speaker_send_lock = None
speaker_playing_until = 0.0


def extract_agent_text(output):
    """Return the final assistant text from Open WebUI's Responses-style output."""
    for item in reversed(output or []):
        if not isinstance(item, dict) or item.get("type") != "message" or item.get("role") != "assistant":
            continue
        parts = item.get("content") or []
        texts = []
        for part in parts:
            if not isinstance(part, dict):
                continue
            if part.get("type") in ("output_text", "text") and part.get("text"):
                texts.append(str(part["text"]))
        text = "".join(texts).strip()
        if text:
            return text
    return ""


def now_ms():
    return int(time.time() * 1000)


def strip_wake_phrase(text: str):
    if not WAKEWORD_PREFIX_RE:
        return text.strip()
    return WAKEWORD_PREFIX_RE.sub("", text, count=1).strip()


def frame_level(frame: bytes):
    samples = struct.unpack("<%dh" % FRAME_SAMPLES, frame)
    peak = max(abs(x) for x in samples)
    sum_sq = sum(x * x for x in samples)
    rms = math.sqrt(sum_sq / len(samples))
    dbfs = 20.0 * math.log10(max(rms, 1.0) / 32767.0)
    peak_dbfs = 20.0 * math.log10(max(peak, 1) / 32767.0)
    return dbfs, peak_dbfs


def pcm_to_wav(pcm: bytes):
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return out.getvalue()


def multipart_wav(wav_bytes: bytes):
    boundary = "----voice-satellite-" + uuid.uuid4().hex
    b = boundary.encode()
    body = bytearray()
    body += b"--" + b + b"\r\n"
    body += b'Content-Disposition: form-data; name="model"\r\n\r\n'
    body += b"groq/whisper-large-v3-turbo\r\n"
    body += b"--" + b + b"\r\n"
    body += b'Content-Disposition: form-data; name="file"; filename="satellite.wav"\r\n'
    body += b"Content-Type: audio/wav\r\n\r\n"
    body += wav_bytes + b"\r\n"
    body += b"--" + b + b"--\r\n"
    return bytes(body), boundary


def http_json(url, payload, headers=None, timeout=60):
    data = json.dumps(payload, ensure_ascii=False).encode()
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=data, headers=req_headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def http_get_json(url, headers=None, timeout=30):
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def do_stt(wav_bytes: bytes):
    body, boundary = multipart_wav(wav_bytes)
    req = urllib.request.Request(
        STT_BASE + "/audio/transcriptions",
        data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read()), dict(r.headers.items())


def do_agent(text: str):
    if not OPENWEBUI_API_KEY:
        raise RuntimeError("OPENWEBUI_API_KEY is not configured")
    if not OPENWEBUI_CHAT_ID:
        raise RuntimeError("OPENWEBUI_CHAT_ID is not configured")

    headers = {"Authorization": "Bearer " + OPENWEBUI_API_KEY}
    chat_data = http_get_json(
        OPENWEBUI_BASE + "/api/v1/chats/" + OPENWEBUI_CHAT_ID,
        headers,
        20,
    )
    history = ((chat_data.get("chat") or {}).get("history") or {})
    stored = history.get("messages") or {}
    parent_id = history.get("currentId")

    # Follow only the current branch backwards. Sending a small recent window
    # gives spoken follow-ups useful context without growing the voice prompt
    # forever.
    chain = []
    cursor = parent_id
    seen = set()
    while cursor and cursor not in seen and cursor in stored:
        seen.add(cursor)
        msg = stored[cursor]
        if msg.get("role") in ("user", "assistant") and isinstance(msg.get("content"), str):
            chain.append({"role": msg["role"], "content": msg["content"]})
        cursor = msg.get("parentId")
    chain.reverse()
    chain = chain[-OPENWEBUI_CONTEXT_MESSAGES:]

    assistant_id = str(uuid.uuid4())
    user_id = str(uuid.uuid4())
    messages = chain + [{"role": "user", "content": text}]

    payload = {
        "model": OPENWEBUI_MODEL,
        "messages": messages,
        "stream": True,
        "chat_id": OPENWEBUI_CHAT_ID,
        "id": assistant_id,
        "parent_id": parent_id,
        "user_message": {
            "id": user_id,
            "parentId": parent_id,
            "role": "user",
            "content": text,
        },
        "tool_ids": OPENWEBUI_TOOL_IDS,
        "features": {},
        "background_tasks": {},
    }
    data = http_json(
        OPENWEBUI_BASE + "/api/chat/completions",
        payload,
        headers,
        30,
    )
    # Saved-chat agent requests may return either a task descriptor or JSON
    # null after the server has already started/completed the background tool
    # loop. The authoritative result is the assistant message in this chat, so
    # always poll that message unless the HTTP request itself raised an error.
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        time.sleep(0.10)
        current = http_get_json(
            OPENWEBUI_BASE + "/api/v1/chats/" + OPENWEBUI_CHAT_ID,
            headers,
            20,
        )
        messages_by_id = ((((current.get("chat") or {}).get("history") or {}).get("messages")) or {})
        message = messages_by_id.get(assistant_id)
        if not message:
            continue
        if message.get("error"):
            error = message["error"]
            if isinstance(error, dict):
                error = error.get("content") or error.get("detail") or json.dumps(error)
            raise RuntimeError(str(error))
        if not message.get("done"):
            continue
        response = str(message.get("content") or "").strip()
        if not response:
            response = extract_agent_text(message.get("output"))
        if not response:
            raise RuntimeError("Open WebUI agent completed without assistant text")
        status["agent_ready"] = True
        return response, {
            "task_ids": (data.get("task_ids") or []) if isinstance(data, dict) else [],
            "chat_id": OPENWEBUI_CHAT_ID,
            "model": message.get("model"),
        }

    raise TimeoutError("Open WebUI agent timed out")


def do_tts(text: str):
    req = urllib.request.Request(
        STT_BASE + "/audio/speech",
        data=json.dumps({"input": text, "model": "edge-tts/en-US-AriaNeural", "voice": "alloy"}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read(), r.headers.get("X-TTS-Language", "")


def tts_to_pcm(audio_bytes: bytes):
    """Decode TTS output to the ESP32 speaker's native 16 kHz mono PCM16."""
    decoded = miniaudio.decode(
        audio_bytes,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=SAMPLE_RATE,
    )
    return bytes(decoded.samples)


async def speaker_send(command: bytes, payload: bytes = b""):
    """Send one atomic control/playback message to the authenticated speaker socket."""
    global speaker_writer
    if speaker_writer is None or speaker_send_lock is None:
        return False
    async with speaker_send_lock:
        writer = speaker_writer
        if writer is None:
            return False
        try:
            writer.write(command)
            if payload:
                writer.write(payload)
            await writer.drain()
            return True
        except (ConnectionError, BrokenPipeError):
            if speaker_writer is writer:
                speaker_writer = None
                status["speaker_connected"] = False
            return False


async def set_speaker_volume(volume: int):
    volume = max(0, min(100, int(volume)))
    status["speaker_volume"] = volume
    # Volume is applied to PCM on the Pi before transmission.  Keep the
    # satellite-side gain at unity so a firmware-side volume bug cannot make
    # the dashboard slider ineffective (or cause us to attenuate twice).
    await speaker_send(b"VOLUME 100\n")
    return volume


def apply_speaker_volume(pcm: bytes, volume: int):
    """Apply a perceptual-ish software gain curve to little-endian PCM16.

    Doing this before the audio leaves the Pi makes the volume control
    authoritative even if the ESP32 ignores its VOLUME control message.
    A quadratic curve gives useful control at the very quiet end where the
    MAX98357A + small speaker combination is otherwise surprisingly loud.
    """
    volume = max(0, min(100, int(volume)))
    if not pcm or volume >= 100:
        return pcm
    if volume == 0:
        return bytes(len(pcm))

    gain = (volume / 100.0) ** 2
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.int32)
    samples = np.rint(samples * gain)
    np.clip(samples, -32768, 32767, out=samples)
    return samples.astype("<i2").tobytes()


def make_diagnostic_tone(peak: int, duration_ms: int = 600, frequency_hz: float = 440.0):
    """Generate a deliberately tiny PCM16 sine for speaker-path diagnostics.

    This bypasses the dashboard volume curve so we can prove what exact sample
    magnitude reaches the ESP32.  Keep this capped very low; it is a temporary
    diagnostic endpoint, not a user-facing speaker-test feature.
    """
    peak = max(0, min(64, int(peak)))
    count = max(1, int(SAMPLE_RATE * duration_ms / 1000))
    if peak == 0:
        return bytes(count * 2)
    t = np.arange(count, dtype=np.float64) / SAMPLE_RATE
    samples = np.rint(np.sin(2.0 * np.pi * frequency_hz * t) * peak)
    return samples.astype("<i2").tobytes()


async def play_on_satellite(pcm: bytes):
    global speaker_playing_until
    if not pcm:
        return False
    pcm = apply_speaker_volume(pcm, status["speaker_volume"])
    duration_ms = len(pcm) * 1000 // (SAMPLE_RATE * 2)
    sent = await speaker_send(f"PLAY {len(pcm)}\n".encode(), pcm)
    if sent:
        status["speaker_playback_ms"] = duration_ms
        status["speaker_bytes_sent"] += len(pcm)
        speaker_playing_until = time.monotonic() + duration_ms / 1000.0 + 0.20
        status["speaker_playing"] = True
    return sent


def add_history(transcript, response, started_ms, stt_ms, agent_ms, tts_ms):
    item = {
        "time": int(time.time()),
        "transcript": transcript,
        "response": response,
        "stt_ms": round(stt_ms),
        "agent_ms": round(agent_ms),
        "tts_ms": round(tts_ms) if tts_ms is not None else None,
        "total_ms": now_ms() - started_ms,
    }
    status["history"] = ([item] + status["history"])[:12]


async def process_utterances():
    global last_tts_audio
    while True:
        pcm = await utterance_queue.get()
        status["queue_depth"] = utterance_queue.qsize()
        status["processing"] = True
        started = now_ms()
        try:
            wav_bytes = pcm_to_wav(pcm)
            t0 = time.perf_counter()
            stt_data, stt_headers = await asyncio.to_thread(do_stt, wav_bytes)
            stt_ms = (time.perf_counter() - t0) * 1000
            transcript = str(stt_data.get("text") or "").strip()
            language = stt_headers.get("X-STT-Language", stt_headers.get("x-stt-language", ""))
            status["last_transcript"] = transcript
            status["last_language"] = language
            if not transcript:
                status["latency"] = {"stt_ms": round(stt_ms), "agent_ms": None, "tts_ms": None, "total_ms": now_ms() - started}
                continue

            command = strip_wake_phrase(transcript)
            # A wake-only utterance arms the short follow-up window but should
            # not become an LLM request by itself.
            if not command:
                status["latency"] = {
                    "stt_ms": round(stt_ms),
                    "agent_ms": None,
                    "tts_ms": None,
                    "total_ms": now_ms() - started,
                }
                continue

            t1 = time.perf_counter()
            response, _ = await asyncio.to_thread(do_agent, command)
            agent_ms = (time.perf_counter() - t1) * 1000
            status["last_response"] = response

            tts_ms = None
            if response:
                t2 = time.perf_counter()
                try:
                    last_tts_audio, _ = await asyncio.to_thread(do_tts, response)
                    speaker_pcm = await asyncio.to_thread(tts_to_pcm, last_tts_audio)
                    await play_on_satellite(speaker_pcm)
                    status["last_audio_version"] += 1
                    tts_ms = (time.perf_counter() - t2) * 1000
                except Exception as exc:
                    status["last_error"] = "TTS: " + str(exc)[:300]

            total_ms = now_ms() - started
            status["latency"] = {
                "stt_ms": round(stt_ms),
                "agent_ms": round(agent_ms),
                "tts_ms": round(tts_ms) if tts_ms is not None else None,
                "total_ms": total_ms,
            }
            status["last_error"] = ""
            add_history(transcript, response, started, stt_ms, agent_ms, tts_ms)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")[:500]
            status["last_error"] = f"HTTP {exc.code}: {detail}"
        except Exception as exc:
            status["last_error"] = str(exc)[:500]
        finally:
            utterance_queue.task_done()
            status["queue_depth"] = utterance_queue.qsize()
            status["processing"] = False


async def handle_audio(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    global wake_armed_until, speaker_writer, speaker_playing_until
    peer = writer.get_extra_info("peername")
    is_speaker_connection = False
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=5)
        expected_mic = ("AUTH " + SATELLITE_TOKEN + "\n").encode()
        expected_speaker = ("AUTH " + SATELLITE_TOKEN + " SPEAKER\n").encode()
        if line not in (expected_mic, expected_speaker):
            writer.write(b"ERR auth\n")
            await writer.drain()
            return
        writer.write(b"OK\n")
        await writer.drain()

        if line == expected_speaker:
            is_speaker_connection = True
            old_writer = speaker_writer
            speaker_writer = writer
            status["speaker_connected"] = True
            if old_writer is not None and old_writer is not writer:
                old_writer.close()
            await set_speaker_volume(status["speaker_volume"])
            # The ESP32 doesn't need to send application data on this socket;
            # this read simply lets us detect when it disconnects.
            await reader.read()
            return

        status.update({
            "connected": True,
            "peer": peer[0] if peer else None,
            "connected_at": int(time.time()),
            "last_error": "",
        })

        pre_roll = collections.deque(maxlen=15)
        speech_frames = []
        speaking = False
        utterance_authorized = False
        voiced_run = 0
        silence_ms = 0
        noise_db = status.get("noise_dbfs", -72.0)
        wake_bytes = bytearray()
        wake_hit_cooldown_until = 0.0

        if wake_model is not None:
            wake_model.reset()

        while True:
            frame = await reader.readexactly(FRAME_BYTES)
            dbfs, peak_dbfs = frame_level(frame)
            status["last_frame_at"] = time.time()
            status["bytes_received"] += len(frame)
            status["dbfs"] = round(dbfs, 1)
            status["peak_dbfs"] = round(peak_dbfs, 1)

            # Do not let the satellite hear its own TTS and recursively trigger
            # another request. Keep the transport flowing, but suspend wake/VAD
            # while speaker playback is in progress plus a short tail.
            if time.monotonic() < speaker_playing_until:
                status["speaker_playing"] = True
                status["speaking"] = False
                pre_roll.clear()
                speech_frames = []
                voiced_run = 0
                silence_ms = 0
                continue
            status["speaker_playing"] = False

            # openWakeWord works naturally in 80 ms / 1280-sample chunks.
            # The ESP stream arrives in 20 ms frames, so aggregate four.
            if wake_model is not None:
                wake_bytes.extend(frame)
                while len(wake_bytes) >= 2560:
                    wake_chunk = bytes(wake_bytes[:2560])
                    del wake_bytes[:2560]
                    predictions = wake_model.predict(np.frombuffer(wake_chunk, dtype=np.int16))
                    score = float(predictions.get(WAKEWORD_MODEL, 0.0))
                    status["wake_score"] = round(score, 4)
                    now_mono = time.monotonic()
                    if score >= WAKEWORD_THRESHOLD and now_mono >= wake_hit_cooldown_until:
                        wake_armed_until = now_mono + WAKEWORD_ARM_SECONDS
                        wake_hit_cooldown_until = now_mono + 2.5
                        status["wake_hits"] += 1
                        status["last_wake_at"] = int(time.time())
                        if speaking:
                            utterance_authorized = True

            if not speaking and dbfs < noise_db + 6:
                noise_db = noise_db * 0.985 + dbfs * 0.015
            threshold = min(-38.0, max(-60.0, noise_db + 10.0))
            status["noise_dbfs"] = round(noise_db, 1)
            status["vad_threshold_dbfs"] = round(threshold, 1)
            pre_roll.append(frame)

            if not speaking:
                if dbfs > threshold:
                    voiced_run += 1
                else:
                    voiced_run = max(0, voiced_run - 1)
                if voiced_run >= 3:
                    speaking = True
                    status["speaking"] = True
                    speech_frames = list(pre_roll)
                    utterance_authorized = time.monotonic() < wake_armed_until
                    silence_ms = 0
            else:
                speech_frames.append(frame)
                end_threshold = min(-42.0, max(-64.0, noise_db + 6.0))
                if dbfs < end_threshold:
                    silence_ms += FRAME_MS
                else:
                    silence_ms = 0

                duration_ms = len(speech_frames) * FRAME_MS
                if silence_ms >= 700 or duration_ms >= 15000:
                    speaking = False
                    status["speaking"] = False
                    voiced_run = 0
                    # Keep only ~200 ms of trailing silence.
                    trim = max(0, (silence_ms - 200) // FRAME_MS)
                    if trim:
                        speech_frames = speech_frames[:-trim]
                    pcm = b"".join(speech_frames)
                    duration_ms = len(pcm) * 1000 // (SAMPLE_RATE * 2)
                    status["last_utterance_ms"] = duration_ms
                    status["utterances"] += 1
                    if duration_ms >= 250:
                        if wake_model is not None and utterance_authorized:
                            try:
                                utterance_queue.put_nowait(pcm)
                            except asyncio.QueueFull:
                                status["last_error"] = "Utterance queue full; dropped audio"
                        else:
                            status["ignored_utterances"] += 1
                    status["queue_depth"] = utterance_queue.qsize()
                    speech_frames = []
                    utterance_authorized = False
    except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
        pass
    except Exception as exc:
        status["last_error"] = "audio socket: " + str(exc)[:300]
    finally:
        if is_speaker_connection:
            if speaker_writer is writer:
                speaker_writer = None
                status["speaker_connected"] = False
        else:
            status["connected"] = False
            status["speaking"] = False
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


def json_bytes(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()


async def http_reply(writer, code, body, content_type="text/plain; charset=utf-8", extra=None):
    reasons = {200: "OK", 404: "Not Found", 405: "Method Not Allowed", 503: "Service Unavailable"}
    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(body)),
        "Cache-Control": "no-store",
        "Connection": "close",
    }
    if extra:
        headers.update(extra)
    head = f"HTTP/1.1 {code} {reasons.get(code, 'OK')}\r\n" + "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"
    writer.write(head.encode() + body)
    await writer.drain()


async def handle_http(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=5)
        if not line:
            return
        parts = line.decode("latin1").strip().split()
        if len(parts) < 2:
            return
        method, path = parts[0], parts[1].split("?", 1)[0]
        while True:
            h = await reader.readline()
            if h in (b"\r\n", b"\n", b""):
                break

        if method == "POST" and path.startswith("/api/volume/"):
            try:
                volume = int(path.rsplit("/", 1)[-1])
                if not 0 <= volume <= 100:
                    raise ValueError
                volume = await set_speaker_volume(volume)
                await http_reply(writer, 200, json_bytes({"ok": True, "volume": volume}), "application/json")
            except ValueError:
                await http_reply(writer, 400, b"volume must be 0..100")
        elif method == "POST" and path.startswith("/api/diagnostic-tone/"):
            try:
                peak = int(path.rsplit("/", 1)[-1])
                if not 0 <= peak <= 64:
                    raise ValueError
                pcm = make_diagnostic_tone(peak)
                sent = await speaker_send(f"PLAY {len(pcm)}\n".encode(), pcm)
                await http_reply(
                    writer,
                    200 if sent else 503,
                    json_bytes({"ok": bool(sent), "peak": peak, "samples": len(pcm) // 2}),
                    "application/json",
                )
            except ValueError:
                await http_reply(writer, 400, b"diagnostic peak must be 0..64")
        elif method != "GET":
            await http_reply(writer, 405, b"method not allowed")
        elif path == "/":
            await http_reply(writer, 200, DASHBOARD, "text/html; charset=utf-8")
        elif path == "/health":
            await http_reply(writer, 200, b"ok\n")
        elif path == "/api/status":
            payload = dict(status)
            payload["uptime_s"] = int(time.time() - STARTED)
            payload["wake_armed"] = time.monotonic() < wake_armed_until
            await http_reply(writer, 200, json_bytes(payload), "application/json")
        elif path == "/api/audio":
            if last_tts_audio:
                await http_reply(writer, 200, last_tts_audio, "audio/mpeg")
            else:
                await http_reply(writer, 404, b"no audio")
        else:
            await http_reply(writer, 404, b"not found")
    except Exception:
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def main():
    global wake_model, speaker_send_lock
    speaker_send_lock = asyncio.Lock()
    try:
        wake_model = WakeWordModel(
            wakeword_models=[WAKEWORD_MODEL],
            inference_framework="onnx",
        )
        status["wake_ready"] = True
        status["wake_error"] = ""
    except Exception as exc:
        # Fail closed: without a working wake detector, no utterance is ever
        # queued for STT/agent processing.
        wake_model = None
        status["wake_ready"] = False
        status["wake_error"] = str(exc)[:300]

    # Validate the service chat eagerly so configuration problems are visible
    # on the dashboard before the first spoken request.
    try:
        if not OPENWEBUI_CHAT_ID:
            raise RuntimeError("OPENWEBUI_CHAT_ID is not configured")
        await asyncio.to_thread(
            http_get_json,
            OPENWEBUI_BASE + "/api/v1/chats/" + OPENWEBUI_CHAT_ID,
            {"Authorization": "Bearer " + OPENWEBUI_API_KEY},
            20,
        )
        status["agent_ready"] = True
    except Exception as exc:
        status["agent_ready"] = False
        status["last_error"] = "Open WebUI agent: " + str(exc)[:300]
    asyncio.create_task(process_utterances())
    audio = await asyncio.start_server(handle_audio, "0.0.0.0", AUDIO_PORT)
    http = await asyncio.start_server(handle_http, "0.0.0.0", HTTP_PORT)
    print(f"voice-satellite receiver: audio={AUDIO_PORT} dashboard={HTTP_PORT}", flush=True)
    async with audio, http:
        await asyncio.gather(audio.serve_forever(), http.serve_forever())


if __name__ == "__main__":
    asyncio.run(main())
