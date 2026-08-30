#pragma once

/* INMP441 wiring. Fill these in with ESP32-S3 GPIO numbers. */
#define MIC_I2S_BCLK_GPIO   7  // INMP441 SCK / BCLK
#define MIC_I2S_WS_GPIO     8  // INMP441 WS / LRCLK
#define MIC_I2S_DATA_GPIO   44  // INMP441 SD -> ESP32 input

/* MAX98357A speaker amp. BCLK/WS are shared with the microphone. */
#define SPEAKER_I2S_BCLK_GPIO MIC_I2S_BCLK_GPIO  // D8 / GPIO7
#define SPEAKER_I2S_WS_GPIO   MIC_I2S_WS_GPIO    // D9 / GPIO8
#define SPEAKER_I2S_DATA_GPIO 43                 // D6 / GPIO43 -> MAX98357A DIN

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_FRAME_SAMPLES 320  // 20 ms at 16 kHz
