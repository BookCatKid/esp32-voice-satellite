#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "voice_satellite_config.h"
#include "voice_satellite_secrets.h"

static const char *TAG = "voice-satellite";

#define WIFI_CONNECTED_BIT BIT0
#define WS_READY_BIT        BIT1
#define WS_OPCODE_TEXT      0x1

static i2s_chan_handle_t s_rx_chan;
static EventGroupHandle_t s_events;
static esp_websocket_client_handle_t s_ws;
static bool s_use_right_channel = false;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WS_READY_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

static void connect_wifi(void)
{
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID=%s", WIFI_SSID);
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
    /* Latency matters more than a few milliwatts for an always-on satellite. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi not connected yet; background reconnect will continue");
    }
}

static esp_err_t init_audio(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = AUDIO_FRAME_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan), TAG, "allocate I2S RX");

    i2s_std_config_t rx_cfg = {
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
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &rx_cfg), TAG, "configure I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable I2S RX");

    ESP_LOGI(TAG, "INMP441: %d Hz BCLK=GPIO%d WS=GPIO%d SD=GPIO%d",
             AUDIO_SAMPLE_RATE_HZ, MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_DATA_GPIO);
    return ESP_OK;
}

static void choose_mic_channel(void)
{
    enum { WORDS = AUDIO_FRAME_SAMPLES * 2 };
    int32_t raw[WORDS];
    uint64_t left_energy = 0, right_energy = 0;

    for (int n = 0; n < 8; n++) {
        size_t bytes = 0;
        if (i2s_channel_read(s_rx_chan, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(1000)) != ESP_OK) {
            continue;
        }
        size_t frames = bytes / (sizeof(int32_t) * 2);
        for (size_t i = 0; i < frames; i++) {
            int32_t l = raw[i * 2] >> 8;
            int32_t r = raw[i * 2 + 1] >> 8;
            left_energy += (uint64_t)llabs(l);
            right_energy += (uint64_t)llabs(r);
        }
    }

    s_use_right_channel = right_energy > left_energy;
    ESP_LOGI(TAG, "Detected active microphone I2S channel: %s",
             s_use_right_channel ? "right" : "left");
}

static bool websocket_send_json(cJSON *root)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        cJSON_Delete(root);
        return false;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return false;
    int len = (int)strlen(text);
    int sent = esp_websocket_client_send_text(s_ws, text, len, pdMS_TO_TICKS(1000));
    cJSON_free(text);
    return sent == len;
}

static void send_hello(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[40];
    snprintf(device_id, sizeof(device_id), "xiao-s3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "protocol", 1);

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "id", device_id);
    cJSON_AddStringToObject(device, "firmware", FIRMWARE_VERSION);

    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("microphone"));

    cJSON *audio = cJSON_AddObjectToObject(root, "audio_in");
    cJSON_AddStringToObject(audio, "codec", "pcm_s16le");
    cJSON_AddNumberToObject(audio, "sample_rate", AUDIO_SAMPLE_RATE_HZ);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_ms", AUDIO_FRAME_MS);

    if (!websocket_send_json(root)) {
        ESP_LOGW(TAG, "Failed to send WebSocket hello");
    }
}

static void send_stats(void)
{
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stats");
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
    websocket_send_json(root);
}

static void handle_control_json(const char *data, size_t len)
{
    char *text = malloc(len + 1);
    if (!text) return;
    memcpy(text, data, len);
    text[len] = '\0';

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "ready") == 0) {
        const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
        if (cJSON_IsNumber(protocol) && protocol->valueint == 1) {
            xEventGroupSetBits(s_events, WS_READY_BIT);
            ESP_LOGI(TAG, "WebSocket protocol ready (%s)", SATELLITE_WS_SUBPROTOCOL);
        }
    }

    cJSON_Delete(root);
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *event = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket transport connected");
        xEventGroupClearBits(s_events, WS_READY_BIT);
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        xEventGroupClearBits(s_events, WS_READY_BIT);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!event) break;
        if (event->op_code == WS_OPCODE_TEXT) {
            handle_control_json(event->data_ptr, event->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (event) {
            ESP_LOGW(TAG, "WebSocket error: transport_errno=%d handshake_status=%d",
                     event->error_handle.esp_transport_sock_errno,
                     event->error_handle.esp_ws_handshake_status_code);
        }
        break;
    default:
        break;
    }
}

static void start_websocket(void)
{
    char headers[192];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", SATELLITE_TOKEN);

    esp_websocket_client_config_t cfg = {
        .uri = SATELLITE_SERVER_URI,
        .subprotocol = SATELLITE_WS_SUBPROTOCOL,
        .headers = headers,
        .buffer_size = 2048,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 1000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 20,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .enable_close_reconnect = true,
        .task_stack = 8192,
    };

    s_ws = esp_websocket_client_init(&cfg);
    ESP_ERROR_CHECK(s_ws ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    ESP_LOGI(TAG, "WebSocket client started: %s", SATELLITE_SERVER_URI);
}


static void microphone_task(void *arg)
{
    enum { RAW_WORDS = AUDIO_FRAME_SAMPLES * 2 };
    int32_t raw[RAW_WORDS];
    int16_t pcm[AUDIO_FRAME_SAMPLES];
    int64_t last_meter = 0;
    int64_t last_stats = 0;

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & (WIFI_CONNECTED_BIT | WS_READY_BIT)) != (WIFI_CONNECTED_BIT | WS_READY_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
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
            int32_t a = abs((int)sample16);
            if (a > peak) peak = a;
            sum_sq += (double)sample16 * sample16;
            pcm[i] = (int16_t)sample16;
        }

        int sent = esp_websocket_client_send_bin(
            s_ws, (const char *)pcm, sizeof(pcm), pdMS_TO_TICKS(1000));
        if (sent != sizeof(pcm)) {
            ESP_LOGW(TAG, "Mic WebSocket send failed (%d/%u)", sent, (unsigned)sizeof(pcm));
            xEventGroupClearBits(s_events, WS_READY_BIT);
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (now - last_meter >= 1000000) {
            double rms = sqrt(sum_sq / frames);
            double dbfs = 20.0 * log10(fmax(rms, 1.0) / 32767.0);
            ESP_LOGI(TAG, "mic=%s level=%.1f dBFS peak=%d ws=ready",
                     s_use_right_channel ? "right" : "left", dbfs, peak);
            last_meter = now;
        }
        if (now - last_stats >= 10000000) {
            send_stats();
            last_stats = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 voice satellite %s booting", FIRMWARE_VERSION);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_events ? ESP_OK : ESP_ERR_NO_MEM);

    connect_wifi();
    ESP_ERROR_CHECK(init_audio());
    choose_mic_channel();
    start_websocket();

    xTaskCreatePinnedToCore(microphone_task, "microphone", 7168, NULL, 7, NULL, 1);
}
