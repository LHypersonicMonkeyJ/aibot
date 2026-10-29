#include "audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace audio {
namespace {

constexpr const char* TAG = "audio";
constexpr size_t kChunk = kSampleRate / 50;     // 20 ms per I2S write
constexpr size_t kPrebuffer = kSampleRate / 10;  // wait for 100 ms before starting a stream
constexpr float kFadeMs = 5;                     // tone fade in/out, avoids clicks

i2s_chan_handle_t g_tx = nullptr;
bool g_ready = false;

// Single-producer, single-consumer ring buffer. head_ is written only by the
// writer, tail_ only by the playback task; both count samples ever written or
// read, so the fill level is head_ - tail_.
int16_t* g_ring = nullptr;
std::atomic<size_t> g_head{0}, g_tail{0};
std::atomic<bool> g_clear{false};

std::atomic<bool> g_streaming{false};  // between the first write and end_stream()
std::atomic<bool> g_playing{false};
std::atomic<float> g_level{0};
std::atomic<uint32_t> g_dropouts{0};
std::atomic<int> g_volume{CONFIG_AIBOT_AUDIO_VOLUME};

// Volume as a Q15 gain. Squared so the percentage feels even to the ear.
int32_t gain_q15() {
    const int v = g_volume.load();
    return (v * v * 32768) / (100 * 100);
}

void playback_task(void*) {
    static int16_t chunk[kChunk];
    bool started = false;  // current stream has passed the prebuffer
    bool starved = false;  // already counted this dropout

    for (;;) {
        if (g_clear.exchange(false)) {
            g_tail.store(g_head.load());
            started = false;
        }
        const size_t head = g_head.load(std::memory_order_acquire);
        size_t tail = g_tail.load(std::memory_order_relaxed);
        const size_t avail = head - tail;
        const bool streaming = g_streaming.load();

        // Hold a new stream back until there's a cushion, unless it already ended
        // (a short tone or clip).
        if (!started && avail > 0 && (avail >= kPrebuffer || !streaming)) started = true;

        size_t n = 0;
        if (started) {
            n = std::min(avail, kChunk);
            for (size_t i = 0; i < n; ++i) chunk[i] = g_ring[(tail + i) % kBufferSamples];
            g_tail.store(tail + n, std::memory_order_release);
        }
        std::fill(chunk + n, chunk + kChunk, 0);

        if (started && n < kChunk) {
            if (streaming && !starved) {
                g_dropouts.fetch_add(1);
                starved = true;
            }
            if (n == 0) started = false;  // wait for the prebuffer again
        }
        if (n == kChunk) starved = false;

        // Volume, and loudness for the face.
        const int32_t gain = gain_q15();
        float sum = 0;
        for (size_t i = 0; i < kChunk; ++i) {
            const float x = chunk[i] / 32768.0f;
            sum += x * x;
            chunk[i] = static_cast<int16_t>((chunk[i] * gain) >> 15);
        }
        g_level.store(std::sqrt(sum / kChunk));
        g_playing.store(n > 0);

        // Blocks until DMA has room, which paces this loop at real time.
        size_t written = 0;
        i2s_channel_write(g_tx, chunk, sizeof(chunk), &written, portMAX_DELAY);
    }
}

}  // namespace

esp_err_t init() {
    if (g_ready) return ESP_OK;

    g_ring = static_cast<int16_t*>(
        heap_caps_malloc(kBufferSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!g_ring) return ESP_ERR_NO_MEM;

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 4;
    chan.dma_frame_num = kChunk;
    chan.auto_clear = true;  // silence, not repeats, if the task ever stalls
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &g_tx, nullptr), TAG, "channel");

    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = kSampleRate;
    cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // Mono data sent in both slots: the MAX98357A plays (left + right) / 2
    // by default, so this gives full volume.
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_AIBOT_I2S_BCLK_GPIO);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_AIBOT_I2S_LRC_GPIO);
    cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_AIBOT_I2S_DIN_GPIO);
    cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx, &cfg), TAG, "std mode");
    // Weakest drive: full-strength edges ring on jumper wires and the amp
    // miscounts BCLK, giving noise instead of sound.
    for (int pin : {CONFIG_AIBOT_I2S_BCLK_GPIO, CONFIG_AIBOT_I2S_LRC_GPIO, CONFIG_AIBOT_I2S_DIN_GPIO}) {
        gpio_set_drive_capability(static_cast<gpio_num_t>(pin), GPIO_DRIVE_CAP_0);
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx), TAG, "enable");

    g_ready = true;
    ESP_LOGI(TAG, "MAX98357A on BCLK=%d LRC=%d DIN=%d, %d Hz", CONFIG_AIBOT_I2S_BCLK_GPIO,
             CONFIG_AIBOT_I2S_LRC_GPIO, CONFIG_AIBOT_I2S_DIN_GPIO, kSampleRate);
    return ESP_OK;
}

bool ready() { return g_ready; }

void start(int priority) {
    if (!g_ready) return;
    xTaskCreate(playback_task, "audio", 4096, nullptr, priority, nullptr);
}

size_t write(const int16_t* samples, size_t n) {
    if (!g_ready) return 0;
    const size_t head = g_head.load(std::memory_order_relaxed);
    const size_t free = kBufferSamples - (head - g_tail.load(std::memory_order_acquire));
    n = std::min(n, free);
    for (size_t i = 0; i < n; ++i) g_ring[(head + i) % kBufferSamples] = samples[i];
    g_head.store(head + n, std::memory_order_release);
    if (n > 0) g_streaming.store(true);
    return n;
}

void end_stream() { g_streaming.store(false); }

void clear() {
    g_streaming.store(false);
    g_clear.store(true);
}

size_t tone(float hz, uint32_t ms) {
    const size_t total = static_cast<size_t>(kSampleRate) * ms / 1000;
    const size_t fade = static_cast<size_t>(kSampleRate * kFadeMs / 1000);
    int16_t block[64];
    size_t queued = 0;
    for (size_t i = 0; i < total; i += 64) {
        const size_t n = std::min<size_t>(64, total - i);
        for (size_t k = 0; k < n; ++k) {
            const size_t j = i + k;
            float env = 1;
            if (j < fade) env = static_cast<float>(j) / fade;
            if (total - j < fade) env = static_cast<float>(total - j) / fade;
            const float phase = 2 * 3.14159265f * hz * j / kSampleRate;
            block[k] = static_cast<int16_t>(std::sin(phase) * env * 0.5f * 32767);
        }
        const size_t w = write(block, n);
        queued += w;
        if (w < n) break;
    }
    end_stream();
    return queued;
}

size_t buffered() { return g_head.load() - g_tail.load(); }
bool playing() { return g_playing.load(); }
float level() { return g_level.load(); }
uint32_t dropouts() { return g_dropouts.load(); }

void set_volume(int percent) { g_volume.store(std::clamp(percent, 0, 100)); }
int volume() { return g_volume.load(); }

}  // namespace audio
