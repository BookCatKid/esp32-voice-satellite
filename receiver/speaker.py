"""Reliable, paced speaker playback over the satellite WebSocket."""

import asyncio
import time

from audio import SATELLITE_FORMAT, apply_volume


class SatelliteSpeaker:
    def __init__(self, volume: int = 16):
        self._volume = self._clamp_volume(volume)
        self._socket = None
        self._supported = False
        self._send_lock = asyncio.Lock()
        self._playback_task = None
        self._play_id = None
        self._serial = 0
        self._playing = False
        self._playing_until = 0.0
        self._playback_ms = 0
        self._bytes_sent = 0

    @staticmethod
    def _clamp_volume(value: int) -> int:
        return max(0, min(100, int(value)))

    @property
    def connected(self) -> bool:
        return bool(self._supported and self._socket is not None and not self._socket.closed)

    @property
    def playing(self) -> bool:
        return self._playing or time.monotonic() < self._playing_until

    @property
    def playing_until(self) -> float:
        return self._playing_until

    def snapshot(self) -> dict:
        return {
            "speaker_connected": self.connected,
            "speaker_volume": self._volume,
            "speaker_playing": self.playing,
            "speaker_playback_ms": self._playback_ms,
            "speaker_bytes_sent": self._bytes_sent,
        }

    async def bind(self, socket, supported: bool) -> None:
        if socket is not self._socket:
            await self.cancel()
        self._socket = socket
        self._supported = supported

    async def unbind(self, socket) -> None:
        if socket is not self._socket:
            return
        await self.cancel()
        self._socket = None
        self._supported = False
        self._playing = False
        self._playing_until = 0.0

    async def send_json(self, payload: dict) -> bool:
        socket = self._socket
        if socket is None or socket.closed:
            return False
        try:
            async with self._send_lock:
                if socket is not self._socket or socket.closed:
                    return False
                await socket.send_json(payload)
            return True
        except (ConnectionError, RuntimeError):
            return False

    async def _send_bytes(self, payload: bytes) -> bool:
        socket = self._socket
        if socket is None or socket.closed:
            return False
        try:
            async with self._send_lock:
                if socket is not self._socket or socket.closed:
                    return False
                await socket.send_bytes(payload)
            return True
        except (ConnectionError, RuntimeError):
            return False

    async def set_volume(self, value: int) -> int:
        self._volume = self._clamp_volume(value)
        await self.send_json({"type": "volume", "value": self._volume})
        return self._volume

    async def cancel(self) -> None:
        task = self._playback_task
        if task is None or task.done() or task is asyncio.current_task():
            return
        play_id = self._play_id
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        if play_id:
            await self.send_json({"type": "playback.cancel", "id": play_id})
        if self._play_id == play_id:
            self._play_id = None

    async def play(self, pcm: bytes) -> bool:
        if not pcm or not self.connected:
            return False
        await self.cancel()
        pcm = apply_volume(pcm, self._volume)
        self._playback_ms = len(pcm) * 1000 // (SATELLITE_FORMAT.sample_rate * 2)
        self._serial += 1
        play_id = f"play-{self._serial}"
        self._play_id = play_id
        self._playback_task = asyncio.create_task(self._stream(play_id, pcm))
        return True

    async def _stream(self, play_id: str, pcm: bytes) -> None:
        try:
            started = await self.send_json({
                "type": "playback.start",
                "id": play_id,
                "audio": {
                    "codec": "pcm_s16le",
                    "sample_rate": SATELLITE_FORMAT.sample_rate,
                    "channels": 1,
                    "frame_ms": SATELLITE_FORMAT.frame_ms,
                },
                "bytes": len(pcm),
                "duration_ms": self._playback_ms,
            })
            if not started:
                return
            self._playing = True
            self._playing_until = time.monotonic() + self._playback_ms / 1000.0 + 0.25
            for offset in range(0, len(pcm), SATELLITE_FORMAT.frame_bytes):
                frame = pcm[offset:offset + SATELLITE_FORMAT.frame_bytes]
                if not await self._send_bytes(frame):
                    return
                self._bytes_sent += len(frame)
                await asyncio.sleep(SATELLITE_FORMAT.frame_ms / 1000.0)
            await self.send_json({"type": "playback.end", "id": play_id})
        finally:
            self._playing = False
            if self._play_id == play_id and not asyncio.current_task().cancelled():
                self._play_id = None

    def acknowledge_done(self) -> None:
        self._playing = False
        self._playing_until = 0.0
