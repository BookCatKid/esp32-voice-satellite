#include "audio_io.h"

#include <math.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "voice_satellite_config.h"


static const char *TAG = "satellite-audio";
static i2s_chan_handle_t s_rx_channel;
static i2s_chan_handle_t s_tx_channel;
static bool s_use_right_channel;


esp_err_t audio_io_init(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = 8;
    channel.dma_frame_num = AUDIO_FRAME_SAMPLES;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel, &s_tx_channel, &s_rx_channel),
        TAG,
        "allocate duplex I2S");

    i2s_std_config_t microphone = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_DATA_GPIO,
            .invert_flags = {0},
        },
    };
    microphone.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;
    microphone.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_channel, &microphone),
        TAG,
        "configure microphone");

    i2s_std_config_t speaker = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_I2S_BCLK_GPIO,
            .ws = SPEAKER_I2S_WS_GPIO,
            .dout = SPEAKER_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    speaker.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;
    speaker.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    speaker.slot_cfg.ws_width = 32;
    speaker.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_channel, &speaker),
        TAG,
        "configure speaker");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "enable microphone");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG, "enable speaker");

    ESP_LOGI(TAG, "shared clock: %d Hz BCLK=GPIO%d WS=GPIO%d",
             AUDIO_SAMPLE_RATE_HZ, MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO);
    ESP_LOGI(TAG, "microphone SD=GPIO%d; speaker DIN=GPIO%d",
             MIC_I2S_DATA_GPIO, SPEAKER_I2S_DATA_GPIO);
    return ESP_OK;
}


void audio_io_detect_microphone_channel(void)
{
    int32_t raw[AUDIO_FRAME_SAMPLES * 2];
    uint64_t left_energy = 0;
    uint64_t right_energy = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        size_t bytes = 0;
        if (i2s_channel_read(
                s_rx_channel,
                raw,
                sizeof(raw),
                &bytes,
                pdMS_TO_TICKS(1000)) != ESP_OK) {
            continue;
        }
        const size_t frames = bytes / (sizeof(int32_t) * 2);
        for (size_t i = 0; i < frames; ++i) {
            left_energy += (uint64_t)llabs(raw[i * 2] >> 8);
            right_energy += (uint64_t)llabs(raw[i * 2 + 1] >> 8);
        }
    }

    s_use_right_channel = right_energy > left_energy;
    ESP_LOGI(TAG, "active microphone slot: %s", audio_io_microphone_channel());
}


const char *audio_io_microphone_channel(void)
{
    return s_use_right_channel ? "right" : "left";
}


esp_err_t audio_io_read_microphone(
    int16_t *pcm,
    size_t samples,
    bool mute,
    audio_level_t *level)
{
    if (!pcm || samples != AUDIO_FRAME_SAMPLES) return ESP_ERR_INVALID_ARG;

    int32_t raw[AUDIO_FRAME_SAMPLES * 2];
    size_t bytes = 0;
    ESP_RETURN_ON_ERROR(
        i2s_channel_read(
            s_rx_channel,
            raw,
            sizeof(raw),
            &bytes,
            pdMS_TO_TICKS(1000)),
        TAG,
        "read microphone");
    if (bytes != sizeof(raw)) return ESP_ERR_INVALID_SIZE;

    double sum_sq = 0.0;
    int16_t peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = (raw[i * 2 + (s_use_right_channel ? 1 : 0)] >> 16);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        const int32_t magnitude = abs((int)sample);
        if (magnitude > peak) peak = magnitude;
        sum_sq += (double)sample * sample;
        pcm[i] = mute ? 0 : (int16_t)sample;
    }

    if (level) {
        const double rms = sqrt(sum_sq / samples);
        level->dbfs = 20.0 * log10(fmax(rms, 1.0) / 32767.0);
        level->peak = peak;
    }
    return ESP_OK;
}


esp_err_t audio_io_write_speaker(const int16_t *mono, size_t samples)
{
    if (!mono || samples > AUDIO_FRAME_SAMPLES) return ESP_ERR_INVALID_ARG;

    int16_t stereo[AUDIO_FRAME_SAMPLES * 2];
    for (size_t i = 0; i < samples; ++i) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }

    const size_t expected = samples * 2 * sizeof(int16_t);
    size_t written = 0;
    ESP_RETURN_ON_ERROR(
        i2s_channel_write(
            s_tx_channel,
            stereo,
            expected,
            &written,
            pdMS_TO_TICKS(1000)),
        TAG,
        "write speaker");
    return written == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
