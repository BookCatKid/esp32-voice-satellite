#pragma once

#define FIRMWARE_VERSION "0.3.0"
#define SATELLITE_WS_SUBPROTOCOL "voice-satellite.v1"

/* INMP441 microphone. */
#define MIC_I2S_BCLK_GPIO   7   /* D8 / SCK */
#define MIC_I2S_WS_GPIO     8   /* D9 / WS */
#define MIC_I2S_DATA_GPIO   44  /* D7 / SD */

/* MAX98357A speaker amp. BCLK/WS are shared with the microphone. */
#define SPEAKER_I2S_BCLK_GPIO MIC_I2S_BCLK_GPIO
#define SPEAKER_I2S_WS_GPIO   MIC_I2S_WS_GPIO
#define SPEAKER_I2S_DATA_GPIO 43  /* D6 / DIN */

#define AUDIO_SAMPLE_RATE_HZ 22050
#define AUDIO_FRAME_MS       20
#define AUDIO_FRAME_SAMPLES  441
#define AUDIO_FRAME_BYTES    (AUDIO_FRAME_SAMPLES * 2)
#define SPEAKER_JITTER_FRAMES 8
