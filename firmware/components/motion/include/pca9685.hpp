#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace motion {

// 16-channel, 12-bit PWM controller. All outputs start fully off, so servos
// stay limp until something commands them.
class Pca9685 {
public:
    static constexpr int kChannels = 16;

    esp_err_t init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t pwm_hz);
    bool ready() const { return dev_ != nullptr; }

    // Output a pulse of `us` microseconds each PWM period.
    esp_err_t set_pulse_us(int channel, uint32_t us);
    // Stop pulsing the channel (hobby servos go limp).
    esp_err_t set_off(int channel);

    uint32_t actual_hz() const { return actual_hz_; }

private:
    esp_err_t write_reg(uint8_t reg, uint8_t value);
    esp_err_t write_channel(int channel, uint16_t on, uint16_t off);

    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t prescale_ = 0;
    uint32_t actual_hz_ = 0;
};

}  // namespace motion
