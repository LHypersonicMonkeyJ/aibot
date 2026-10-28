#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"

namespace motion {

// Creates the shared I2C master bus on the Kconfig SDA/SCL pins.
esp_err_t i2c_bus_init();
i2c_master_bus_handle_t i2c_bus();

// Probes every 7-bit address. Writes up to `max` responding addresses into
// `out` and returns how many responded.
size_t i2c_scan(uint8_t* out, size_t max);

}  // namespace motion
