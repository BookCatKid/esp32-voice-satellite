#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


typedef struct {
    double dbfs;
    int16_t peak;
} audio_level_t;

esp_err_t audio_io_init(void);
void audio_io_detect_microphone_channel(void);
const char *audio_io_microphone_channel(void);
esp_err_t audio_io_read_microphone(
    int16_t *pcm,
    size_t samples,
    bool mute,
    audio_level_t *level);
esp_err_t audio_io_write_speaker(const int16_t *mono, size_t samples);
