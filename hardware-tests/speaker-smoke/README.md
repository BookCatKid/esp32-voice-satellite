# Known-good speaker smoke test

This standalone Arduino/PlatformIO project preserves the exact configuration
verified on the target Seeed XIAO ESP32-S3 and MAX98357A-compatible amplifier:

- XIAO D6 / GPIO43: I2S data out
- XIAO D8 / GPIO7: I2S bit clock
- XIAO D9 / GPIO8: I2S word select
- 22.05 kHz, signed 16-bit stereo I2S
- APLL clock source
- 16% source amplitude

It says “Audio test” once after boot. Treat this as a hardware fallback, not as
the production firmware.

```bash
pio run -d hardware-tests/speaker-smoke -t upload
```
