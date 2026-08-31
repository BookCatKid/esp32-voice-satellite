"""PCM format conversion for the satellite wire and inference pipeline."""

import io
import math
import wave
from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class PcmFormat:
    sample_rate: int
    frame_ms: int = 20

    @property
    def frame_samples(self) -> int:
        return self.sample_rate * self.frame_ms // 1000

    @property
    def frame_bytes(self) -> int:
        return self.frame_samples * 2


PIPELINE_FORMAT = PcmFormat(16000)
SATELLITE_FORMAT = PcmFormat(22050)


def frame_level(frame: bytes, fmt: PcmFormat = PIPELINE_FORMAT) -> tuple[float, float]:
    if len(frame) != fmt.frame_bytes:
        raise ValueError(f"expected {fmt.frame_bytes} bytes, got {len(frame)}")
    samples = np.frombuffer(frame, dtype="<i2").astype(np.float64)
    peak = float(np.max(np.abs(samples)))
    rms = math.sqrt(float(np.mean(samples * samples)))
    dbfs = 20.0 * math.log10(max(rms, 1.0) / 32767.0)
    peak_dbfs = 20.0 * math.log10(max(peak, 1.0) / 32767.0)
    return dbfs, peak_dbfs


def pcm_to_wav(pcm: bytes, fmt: PcmFormat = PIPELINE_FORMAT) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(fmt.sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()


def resample_frame(
    frame: bytes,
    source: PcmFormat = SATELLITE_FORMAT,
    target: PcmFormat = PIPELINE_FORMAT,
) -> bytes:
    """Resample one same-duration PCM16 frame with deterministic linear interpolation."""
    if source.frame_ms != target.frame_ms:
        raise ValueError("source and target frames must have the same duration")
    if len(frame) != source.frame_bytes:
        raise ValueError(f"expected {source.frame_bytes} bytes, got {len(frame)}")
    samples = np.frombuffer(frame, dtype="<i2").astype(np.float64)
    positions = np.arange(target.frame_samples, dtype=np.float64)
    positions *= source.sample_rate / target.sample_rate
    output = np.rint(np.interp(positions, np.arange(source.frame_samples), samples))
    np.clip(output, -32768, 32767, out=output)
    return output.astype("<i2").tobytes()


def decode_tts(audio_bytes: bytes, fmt: PcmFormat = SATELLITE_FORMAT) -> bytes:
    import miniaudio

    decoded = miniaudio.decode(
        audio_bytes,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=fmt.sample_rate,
    )
    return bytes(decoded.samples)


def apply_volume(pcm: bytes, percent: int) -> bytes:
    percent = max(0, min(100, int(percent)))
    if not pcm or percent == 100:
        return pcm
    if percent == 0:
        return bytes(len(pcm))
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.float64)
    samples = np.rint(samples * (percent / 100.0))
    np.clip(samples, -32768, 32767, out=samples)
    return samples.astype("<i2").tobytes()


def diagnostic_tone(
    peak: int,
    duration_ms: int = 600,
    frequency_hz: float = 440.0,
    fmt: PcmFormat = SATELLITE_FORMAT,
) -> bytes:
    peak = max(0, min(4096, int(peak)))
    count = max(1, fmt.sample_rate * duration_ms // 1000)
    if peak == 0:
        return bytes(count * 2)
    time_axis = np.arange(count, dtype=np.float64) / fmt.sample_rate
    samples = np.rint(np.sin(2.0 * np.pi * frequency_hz * time_axis) * peak)
    return samples.astype("<i2").tobytes()
