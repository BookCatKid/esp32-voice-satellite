#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "voice_satellite_config.h"
#include "voice_satellite_secrets.h"

static const char *TAG = "voice-satellite";
static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static bool s_use_right_channel = false;
static volatile int s_speaker_volume = 1;
static volatile bool s_speaker_playing = false;

static bool recv_line(int sock, char *line, size_t capacity);

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void connect_wifi(void)
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID=%s", WIFI_SSID);
    s_wifi_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi not connected yet; background reconnect will continue");
    }
}

static esp_err_t init_microphone(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = AUDIO_FRAME_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan), TAG, "allocate I2S duplex");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_DATA_GPIO,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "configure I2S RX");

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_I2S_BCLK_GPIO,
            .ws = SPEAKER_I2S_WS_GPIO,
            .dout = SPEAKER_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    /* The microphone needs 32 BCLKs per channel. Keep that shared physical
     * frame width, but let the TX peripheral place 16-bit speaker samples in
     * those 32-bit slots instead of hand-packing PCM16 into int32_t words. */
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx_cfg.slot_cfg.ws_width = 32;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &tx_cfg), TAG, "configure I2S TX");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable I2S TX");
    ESP_LOGI(TAG, "INMP441: 16 kHz, BCLK=GPIO%d WS=GPIO%d SD=GPIO%d", MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_DATA_GPIO);
    ESP_LOGI(TAG, "MAX98357A: 16 kHz, BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d", SPEAKER_I2S_BCLK_GPIO, SPEAKER_I2S_WS_GPIO, SPEAKER_I2S_DATA_GPIO);
    return ESP_OK;
}

static void choose_mic_channel(void)
{
    enum { WORDS = AUDIO_FRAME_SAMPLES * 2 };
    int32_t raw[WORDS];
    uint64_t left_energy = 0, right_energy = 0;
    for (int n = 0; n < 8; n++) {
        size_t bytes = 0;
        if (i2s_channel_read(s_rx_chan, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(1000)) != ESP_OK) continue;
        size_t frames = bytes / (sizeof(int32_t) * 2);
        for (size_t i = 0; i < frames; i++) {
            int32_t l = raw[i * 2] >> 8;
            int32_t r = raw[i * 2 + 1] >> 8;
            left_energy += (uint64_t)llabs(l);
            right_energy += (uint64_t)llabs(r);
        }
    }
    s_use_right_channel = right_energy > left_energy;
    ESP_LOGI(TAG, "Detected active microphone I2S channel: %s", s_use_right_channel ? "right" : "left");
}

static int connect_receiver_mode(const char *mode)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SATELLITE_SERVER_PORT);
    if (inet_pton(AF_INET, SATELLITE_SERVER_HOST, &addr.sin_addr) != 1) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return -1;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    char auth[128];
    int auth_len = mode && mode[0]
        ? snprintf(auth, sizeof(auth), "AUTH %s %s\n", SATELLITE_TOKEN, mode)
        : snprintf(auth, sizeof(auth), "AUTH %s\n", SATELLITE_TOKEN);
    if (send(sock, auth, auth_len, 0) != auth_len) {
        close(sock);
        return -1;
    }
    char reply[16] = {0};
    if (!recv_line(sock, reply, sizeof(reply)) || strcmp(reply, "OK") != 0) {
        close(sock);
        return -1;
    }
    if (mode && mode[0]) {
        /* Speaker downlink is normally idle between responses. */
        struct timeval no_recv_timeout = {.tv_sec = 0, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &no_recv_timeout, sizeof(no_recv_timeout));
    }
    timeout.tv_sec = 10;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ESP_LOGI(TAG, "Connected to receiver %s:%d", SATELLITE_SERVER_HOST, SATELLITE_SERVER_PORT);
    return sock;
}

static int connect_receiver(void)
{
    return connect_receiver_mode(NULL);
}

static bool send_all(int sock, const uint8_t *data, size_t len)
{
    while (len) {
        int sent = send(sock, data, len, 0);
        if (sent <= 0) return false;
        data += sent;
        len -= sent;
    }
    return true;
}

static bool recv_all(int sock, uint8_t *data, size_t len)
{
    while (len) {
        int got = recv(sock, data, len, 0);
        if (got <= 0) return false;
        data += got;
        len -= got;
    }
    return true;
}

static bool recv_line(int sock, char *line, size_t capacity)
{
    if (capacity < 2) return false;
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        int got = recv(sock, &c, 1, 0);
        if (got <= 0) return false;
        if (c == '\n') {
            line[used] = '\0';
            return true;
        }
        if (c != '\r') line[used++] = c;
    }
    line[capacity - 1] = '\0';
    return false;
}

static bool play_pcm_from_socket(int sock, size_t byte_count)
{
    enum { CHUNK_SAMPLES = 320 };
    int16_t input[CHUNK_SAMPLES];
    int16_t output[CHUNK_SAMPLES * 2];
    s_speaker_playing = true;
    int32_t playback_input_peak = 0;
    int32_t playback_output_peak = 0;
    size_t playback_samples = 0;

    while (byte_count > 0) {
        size_t samples = byte_count / sizeof(int16_t);
        if (samples > CHUNK_SAMPLES) samples = CHUNK_SAMPLES;
        size_t bytes = samples * sizeof(int16_t);
        if (!recv_all(sock, (uint8_t *)input, bytes)) {
            s_speaker_playing = false;
            return false;
        }
        int volume = s_speaker_volume;
        for (size_t i = 0; i < samples; ++i) {
            int32_t input_abs = input[i] < 0 ? -(int32_t)input[i] : (int32_t)input[i];
            if (input_abs > playback_input_peak) playback_input_peak = input_abs;
            int32_t scaled16 = ((int32_t)input[i] * volume) / 100;
            if (scaled16 > INT16_MAX) scaled16 = INT16_MAX;
            if (scaled16 < INT16_MIN) scaled16 = INT16_MIN;
            int32_t output_abs = scaled16 < 0 ? -scaled16 : scaled16;
            if (output_abs > playback_output_peak) playback_output_peak = output_abs;
            output[i * 2] = (int16_t)scaled16;
            output[i * 2 + 1] = (int16_t)scaled16;
        }
        playback_samples += samples;
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            s_tx_chan, output, samples * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || written != samples * 2 * sizeof(int16_t)) {
            ESP_LOGE(TAG, "Speaker write failed: %s", esp_err_to_name(err));
            s_speaker_playing = false;
            return false;
        }
        byte_count -= bytes;
    }
    ESP_LOGI(TAG,
             "Speaker PCM stats: samples=%u input_peak=%ld/32767 output_peak=%ld/32767 volume=%d%%",
             (unsigned)playback_samples,
             (long)playback_input_peak,
             (long)playback_output_peak,
             s_speaker_volume);
    s_speaker_playing = false;
    return true;
}

static void speaker_stream_task(void *arg)
{
    int sock = -1;
    char line[64];
    for (;;) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            if (sock >= 0) { close(sock); sock = -1; }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (sock < 0) {
            sock = connect_receiver_mode("SPEAKER");
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Speaker downlink connected; starting volume=%d%%", s_speaker_volume);
        }

        if (!recv_line(sock, line, sizeof(line))) {
            ESP_LOGW(TAG, "Speaker downlink lost");
            close(sock);
            sock = -1;
            s_speaker_playing = false;
            continue;
        }
        int volume = 0;
        unsigned long byte_count = 0;
        if (sscanf(line, "VOLUME %d", &volume) == 1) {
            if (volume < 0) volume = 0;
            if (volume > 100) volume = 100;
            s_speaker_volume = volume;
            ESP_LOGI(TAG, "Speaker volume=%d%%", volume);
        } else if (sscanf(line, "PLAY %lu", &byte_count) == 1) {
            ESP_LOGI(TAG, "Speaker playback=%lu bytes volume=%d%%", byte_count, s_speaker_volume);
            if ((byte_count & 1U) || !play_pcm_from_socket(sock, (size_t)byte_count)) {
                close(sock);
                sock = -1;
                s_speaker_playing = false;
            }
        } else {
            ESP_LOGW(TAG, "Unknown speaker command: %s", line);
        }
    }
}

static void audio_stream_task(void *arg)
{
    enum { RAW_WORDS = AUDIO_FRAME_SAMPLES * 2 };
    int32_t raw[RAW_WORDS];
    int16_t pcm[AUDIO_FRAME_SAMPLES];
    int sock = -1;
    int64_t last_meter = 0;

    for (;;) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            if (sock >= 0) { close(sock); sock = -1; }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (sock < 0) {
            sock = connect_receiver();
            if (sock < 0) {
                ESP_LOGW(TAG, "Receiver unavailable; retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) continue;
        size_t frames = bytes / (sizeof(int32_t) * 2);
        if (frames != AUDIO_FRAME_SAMPLES) continue;

        double sum_sq = 0.0;
        int16_t peak = 0;
        for (size_t i = 0; i < frames; i++) {
            int32_t sample24 = raw[i * 2 + (s_use_right_channel ? 1 : 0)] >> 8;
            int32_t sample16 = sample24 >> 8;
            if (sample16 > INT16_MAX) sample16 = INT16_MAX;
            if (sample16 < INT16_MIN) sample16 = INT16_MIN;
            /* Keep the local USB meter honest while speaker playback is active.
             * Only the PCM sent upstream is muted to prevent feedback; the meter
             * still measures the physical sound reaching the INMP441. */
            int32_t a = abs((int)sample16);
            if (a > peak) peak = a;
            sum_sq += (double)sample16 * sample16;
            pcm[i] = s_speaker_playing ? 0 : (int16_t)sample16;
        }

        if (!send_all(sock, (const uint8_t *)pcm, sizeof(pcm))) {
            ESP_LOGW(TAG, "Receiver connection lost");
            close(sock);
            sock = -1;
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_meter > 1000000) {
            double rms = sqrt(sum_sq / frames);
            double dbfs = 20.0 * log10(fmax(rms, 1.0) / 32767.0);
            ESP_LOGI(TAG, "streaming mic=%s level=%.1f dBFS peak=%d", s_use_right_channel ? "right" : "left", dbfs, peak);
            last_meter = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 voice satellite booting");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    connect_wifi();
    ESP_ERROR_CHECK(init_microphone());
    choose_mic_channel();
    xTaskCreatePinnedToCore(audio_stream_task, "audio_stream", 7168, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(speaker_stream_task, "speaker_stream", 6144, NULL, 7, NULL, 0);
}
