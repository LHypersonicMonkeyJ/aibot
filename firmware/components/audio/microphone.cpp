#include "microphone.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace mic {
namespace {

constexpr const char* TAG = "mic";
constexpr float kFullScale24 = 8388608.0f;  // INMP441 samples are 24-bit
constexpr float kHighPass = 0.969f;         // low cut at about 80 Hz: DC drift and rumble
constexpr uint32_t kStartupBlocks = 15;     // first 300 ms after boot are junk; send silence

i2s_chan_handle_t g_rx = nullptr;
bool g_ready = false;
BlockHandler g_handler = nullptr;

std::atomic<bool> g_streaming{false};
std::atomic<int> g_gain_db{CONFIG_AIBOT_MIC_GAIN_DB};
std::atomic<float> g_peak{0};
std::atomic<uint32_t> g_overruns{0};

// Runs in the I2S interrupt when a DMA buffer (one block) is dropped.
bool IRAM_ATTR on_overflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    g_overruns.fetch_add(1);
    return false;
}

void mic_task(void*) {
    static int32_t raw[kBlockSamples];
    static int16_t out[kBlockSamples];
    float prev_in = 0, prev_out = 0;
    uint32_t blocks = 0;

    for (;;) {
        size_t bytes = 0;
        if (i2s_channel_read(g_rx, raw, sizeof(raw), &bytes, portMAX_DELAY) != ESP_OK) continue;
        const size_t n = bytes / sizeof(int32_t);

        const float gain = std::pow(10.0f, g_gain_db.load() / 20.0f);
        const bool settling = blocks < kStartupBlocks;
        float peak = 0;
        for (size_t i = 0; i < n; ++i) {
            const float x = (raw[i] >> 8) / kFullScale24;
            const float y = x - prev_in + kHighPass * prev_out;
            prev_in = x;
            prev_out = y;
            const float s = settling ? 0.0f : std::clamp(y * gain, -1.0f, 1.0f);
            peak = std::max(peak, std::fabs(s));
            out[i] = static_cast<int16_t>(s * 32767.0f);
        }
        if (peak > g_peak.load()) g_peak.store(peak);

        const uint32_t seq = blocks++ + g_overruns.load();
        if (g_streaming.load() && g_handler) g_handler(out, n, seq);
    }
}

}  // namespace

esp_err_t init() {
    if (g_ready) return ESP_OK;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;  // 120 ms of slack for slow serial writes
    chan.dma_frame_num = kBlockSamples;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, nullptr, &g_rx), TAG, "channel");

    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    // The INMP441 sends 24-bit samples left-aligned in 32-bit slots; with L/R
    // tied to GND it uses the left slot.
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_AIBOT_MIC_SCK_GPIO);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_AIBOT_MIC_WS_GPIO);
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din = static_cast<gpio_num_t>(CONFIG_AIBOT_MIC_SD_GPIO);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_rx, &cfg), TAG, "std mode");

    // Weakest drive on the clocks, as for the speaker (ringing on jumper wires).
    for (int pin : {CONFIG_AIBOT_MIC_SCK_GPIO, CONFIG_AIBOT_MIC_WS_GPIO}) {
        gpio_set_drive_capability(static_cast<gpio_num_t>(pin), GPIO_DRIVE_CAP_0);
    }
    // The INMP441 leaves SD floating outside its slot, and so does a missing mic.
    gpio_pulldown_en(static_cast<gpio_num_t>(CONFIG_AIBOT_MIC_SD_GPIO));

    i2s_event_callbacks_t cbs = {};
    cbs.on_recv_q_ovf = on_overflow;
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(g_rx, &cbs, nullptr), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_rx), TAG, "enable");

    g_ready = true;
    ESP_LOGI(TAG, "INMP441 on SCK=%d WS=%d SD=%d, %d Hz, gain %d dB", CONFIG_AIBOT_MIC_SCK_GPIO,
             CONFIG_AIBOT_MIC_WS_GPIO, CONFIG_AIBOT_MIC_SD_GPIO, kSampleRate, g_gain_db.load());
    return ESP_OK;
}

bool ready() { return g_ready; }

void start(int priority, BlockHandler handler) {
    if (!g_ready) return;
    g_handler = handler;
    xTaskCreate(mic_task, "mic", 4096, nullptr, priority, nullptr);
}

void set_streaming(bool on) { g_streaming.store(on); }
bool streaming() { return g_streaming.load(); }

void set_gain_db(int db) { g_gain_db.store(std::clamp(db, 0, 40)); }
int gain_db() { return g_gain_db.load(); }

float take_peak() { return g_peak.exchange(0); }
uint32_t overruns() { return g_overruns.load(); }

}  // namespace mic
