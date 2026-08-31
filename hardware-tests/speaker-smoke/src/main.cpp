#include <Arduino.h>
#include <driver/i2s.h>
#include "audio_data.h"

namespace {
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr int kDataPin = D6;        // XIAO header D6 = ESP32-S3 GPIO43
constexpr int kBclkPin = D8;        // XIAO header D8 = ESP32-S3 GPIO7
constexpr int kWordSelectPin = D9;  // XIAO header D9 = ESP32-S3 GPIO8
constexpr uint32_t kSampleRate = 22050;

static_assert(kDataPin == D6, "I2S data must use the XIAO D6 header pin");
static_assert(kBclkPin == D8, "I2S BCLK must use the XIAO D8 header pin");
static_assert(kWordSelectPin == D9, "I2S WS must use the XIAO D9 header pin");

void installI2s() {
  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = kSampleRate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = true,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  const i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = kBclkPin,
      .ws_io_num = kWordSelectPin,
      .data_out_num = kDataPin,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  i2s_driver_install(kI2sPort, &config, 0, nullptr);
  i2s_set_pin(kI2sPort, &pins);
  i2s_set_clk(kI2sPort, kSampleRate, I2S_BITS_PER_SAMPLE_16BIT,
              I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(kI2sPort);
}

void playOnceQuietly() {
  constexpr size_t kFramesPerBlock = 128;
  int16_t stereo[kFramesPerBlock * 2];
  const auto *mono = reinterpret_cast<const int16_t *>(audio_raw);
  const size_t sampleCount = audio_raw_len / sizeof(int16_t);

  for (size_t offset = 0; offset < sampleCount; offset += kFramesPerBlock) {
    const size_t count = min(kFramesPerBlock, sampleCount - offset);
    for (size_t i = 0; i < count; ++i) {
      // The sample itself is already attenuated to 16% (about -16 dB).
      stereo[2 * i] = mono[offset + i];
      stereo[2 * i + 1] = mono[offset + i];
    }
    size_t bytesWritten = 0;
    i2s_write(kI2sPort, stereo, count * 2 * sizeof(int16_t),
              &bytesWritten, portMAX_DELAY);
  }

  delay(30);
  i2s_zero_dma_buffer(kI2sPort);
}
}  // namespace

void setup() {
  installI2s();
  delay(100);
  playOnceQuietly();
}

void loop() { delay(1000); }
