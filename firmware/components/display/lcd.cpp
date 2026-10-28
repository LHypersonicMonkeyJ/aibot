#include "lcd.hpp"

#include <algorithm>
#include <atomic>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace display {
namespace {

constexpr const char* TAG = "lcd";
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kBandPixels = kWidth * kBandRows;
constexpr int kNumBuffers = 2;

constexpr ledc_mode_t kBlMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBlTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kBlResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kBlMaxDuty = (1u << 10) - 1;

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
uint16_t* g_buf[kNumBuffers] = {};
SemaphoreHandle_t g_done = nullptr;   // given once per finished band transfer
SemaphoreHandle_t g_frame = nullptr;  // serializes whole frames
bool g_ready = false;
int g_brightness = 0;

// Kconfig leaves unset bools undefined, so map each to true/false.
#ifdef CONFIG_AIBOT_LCD_INVERT
constexpr bool kInvert = true;
#else
constexpr bool kInvert = false;
#endif
#ifdef CONFIG_AIBOT_LCD_MIRROR_X
constexpr bool kMirrorX = true;
#else
constexpr bool kMirrorX = false;
#endif
#ifdef CONFIG_AIBOT_LCD_MIRROR_Y
constexpr bool kMirrorY = true;
#else
constexpr bool kMirrorY = false;
#endif
#ifdef CONFIG_AIBOT_LCD_SWAP_RB
constexpr bool kSwapRb = true;
#else
constexpr bool kSwapRb = false;
#endif

PanelFlags g_flags = {kInvert, kMirrorX, kMirrorY, kSwapRb};
std::atomic<bool> g_swap_rb{kSwapRb};

bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_done, &woken);
    return woken == pdTRUE;
}

esp_err_t backlight_init() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = kBlMode;
    timer.duty_resolution = kBlResolution;
    timer.timer_num = kBlTimer;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    ledc_channel_config_t channel = {};
    channel.gpio_num = CONFIG_AIBOT_LCD_BL_GPIO;
    channel.speed_mode = kBlMode;
    channel.channel = kBlChannel;
    channel.timer_sel = kBlTimer;
    channel.duty = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    // BL may feed the backlight LEDs directly, so source as much current as
    // the pin allows (about 40 mA).
    return gpio_set_drive_capability(static_cast<gpio_num_t>(CONFIG_AIBOT_LCD_BL_GPIO),
                                     GPIO_DRIVE_CAP_3);
}

esp_err_t apply_flags(const PanelFlags& f) {
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(g_panel, f.invert), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(g_panel, f.mirror_x, f.mirror_y), TAG, "mirror");
    g_swap_rb.store(f.swap_rb);
    return ESP_OK;
}

}  // namespace

esp_err_t lcd_init() {
    if (g_ready) return ESP_OK;

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_AIBOT_LCD_MOSI_GPIO;
    bus.miso_io_num = -1;
    bus.sclk_io_num = CONFIG_AIBOT_LCD_SCLK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kBandPixels * static_cast<int>(sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = static_cast<gpio_num_t>(CONFIG_AIBOT_LCD_CS_GPIO);
    io_cfg.dc_gpio_num = static_cast<gpio_num_t>(CONFIG_AIBOT_LCD_DC_GPIO);
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = CONFIG_AIBOT_LCD_SPI_MHZ * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_done;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &g_io),
        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_AIBOT_LCD_RST_GPIO);
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(g_io, &panel_cfg, &g_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(apply_flags(g_flags), TAG, "flags");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(g_panel, true), TAG, "display on");

    for (auto& buf : g_buf) {
        buf = static_cast<uint16_t*>(
            heap_caps_malloc(kBandPixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!buf) return ESP_ERR_NO_MEM;
    }
    g_done = xSemaphoreCreateCounting(kNumBuffers, 0);
    g_frame = xSemaphoreCreateMutex();
    if (!g_done || !g_frame) return ESP_ERR_NO_MEM;

    g_ready = true;
    ESP_LOGI(TAG, "GC9A01 on MOSI=%d CLK=%d CS=%d DC=%d RST=%d BL=%d at %d MHz",
             CONFIG_AIBOT_LCD_MOSI_GPIO, CONFIG_AIBOT_LCD_SCLK_GPIO, CONFIG_AIBOT_LCD_CS_GPIO,
             CONFIG_AIBOT_LCD_DC_GPIO, CONFIG_AIBOT_LCD_RST_GPIO, CONFIG_AIBOT_LCD_BL_GPIO,
             CONFIG_AIBOT_LCD_SPI_MHZ);
    return ESP_OK;
}

bool lcd_ready() { return g_ready; }

esp_err_t lcd_draw(BandFn fn, void* ctx) {
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_frame, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    int in_flight = 0;
    for (int band = 0, y = 0; y < kHeight; ++band, y += kBandRows) {
        // Transfers finish in order, so once both buffers are queued, the
        // next completion frees the buffer we are about to reuse.
        if (in_flight == kNumBuffers) {
            xSemaphoreTake(g_done, portMAX_DELAY);
            --in_flight;
        }
        const int rows = std::min(kBandRows, kHeight - y);
        uint16_t* buf = g_buf[band % kNumBuffers];
        fn(buf, y, rows, ctx);
        err = esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, buf);
        if (err != ESP_OK) break;
        ++in_flight;
    }
    while (in_flight-- > 0) xSemaphoreTake(g_done, portMAX_DELAY);

    xSemaphoreGive(g_frame);
    return err;
}

esp_err_t lcd_set_brightness(int percent) {
    percent = std::clamp(percent, 0, 100);
    const uint32_t duty = kBlMaxDuty * static_cast<uint32_t>(percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(kBlMode, kBlChannel, duty), TAG, "duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(kBlMode, kBlChannel), TAG, "update duty");
    g_brightness = percent;
    return ESP_OK;
}

int lcd_brightness() { return g_brightness; }

esp_err_t lcd_set_flags(const PanelFlags& flags) {
    if (!g_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_frame, portMAX_DELAY);
    const esp_err_t err = apply_flags(flags);
    if (err == ESP_OK) g_flags = flags;
    xSemaphoreGive(g_frame);
    return err;
}

PanelFlags lcd_flags() { return g_flags; }

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    if (g_swap_rb.load(std::memory_order_relaxed)) std::swap(r, b);
    const uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>((c >> 8) | (c << 8));  // SPI sends high byte first
}

}  // namespace display
