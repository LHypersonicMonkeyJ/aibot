#include "i2c_bus.hpp"

#include "sdkconfig.h"

namespace motion {
namespace {
i2c_master_bus_handle_t g_bus = nullptr;
}

esp_err_t i2c_bus_init() {
    if (g_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = -1;
    cfg.sda_io_num = static_cast<gpio_num_t>(CONFIG_AIBOT_I2C_SDA_GPIO);
    cfg.scl_io_num = static_cast<gpio_num_t>(CONFIG_AIBOT_I2C_SCL_GPIO);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    // PCA9685 breakouts have their own pull-ups; these just help if they don't.
    cfg.flags.enable_internal_pullup = 1;
    return i2c_new_master_bus(&cfg, &g_bus);
}

i2c_master_bus_handle_t i2c_bus() { return g_bus; }

size_t i2c_scan(uint8_t* out, size_t max) {
    if (!g_bus) return 0;
    size_t n = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_master_probe(g_bus, addr, 20) == ESP_OK && n < max) out[n++] = addr;
    }
    return n;
}

}  // namespace motion
