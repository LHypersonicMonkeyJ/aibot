#include "pca9685.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace motion {
namespace {

constexpr const char* TAG = "pca9685";

// Nominal internal oscillator. Real chips are off by a few percent; measure
// the output with a scope and adjust if servo centering matters.
constexpr uint32_t kOscHz = 25'000'000;
constexpr int kTimeoutMs = 50;

constexpr uint8_t kMode1 = 0x00;
constexpr uint8_t kMode2 = 0x01;
constexpr uint8_t kLed0OnL = 0x06;
constexpr uint8_t kAllLedOnL = 0xFA;
constexpr uint8_t kPrescale = 0xFE;

constexpr uint8_t kMode1Ai = 0x20;     // register auto-increment
constexpr uint8_t kMode1Sleep = 0x10;
constexpr uint8_t kMode2OutDrv = 0x04; // totem-pole outputs
constexpr uint16_t kFullBit = 0x1000;  // bit 12 of ON/OFF: full on / full off

}  // namespace

esp_err_t Pca9685::init(i2c_master_bus_handle_t bus, uint8_t address, uint32_t pwm_hz) {
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, address, kTimeoutMs), TAG, "no device at 0x%02x", address);

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = address;
    cfg.scl_speed_hz = 400'000;
    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &dev), TAG, "add device");
    dev_ = dev;

    // prescale = round(osc / (4096 * f)) - 1
    const uint32_t div = 4096 * pwm_hz;
    prescale_ = static_cast<uint8_t>((kOscHz + div / 2) / div - 1);
    actual_hz_ = kOscHz / (4096 * (prescale_ + 1u));

    esp_err_t err = ESP_OK;
    // Prescale can only be written while asleep.
    if (err == ESP_OK) err = write_reg(kMode1, kMode1Sleep | kMode1Ai);
    if (err == ESP_OK) err = write_reg(kPrescale, prescale_);
    if (err == ESP_OK) err = write_reg(kMode2, kMode2OutDrv);
    if (err == ESP_OK) {
        const uint8_t all_off[] = {kAllLedOnL, 0, 0, 0, kFullBit >> 8};
        err = i2c_master_transmit(dev_, all_off, sizeof(all_off), kTimeoutMs);
    }
    if (err == ESP_OK) err = write_reg(kMode1, kMode1Ai);  // wake
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1));  // oscillator needs 500 us to start

    ESP_LOGI(TAG, "ready at 0x%02x, prescale %u, %lu Hz", address, prescale_,
             static_cast<unsigned long>(actual_hz_));
    return ESP_OK;
}

esp_err_t Pca9685::set_pulse_us(int channel, uint32_t us) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (channel < 0 || channel >= kChannels) return ESP_ERR_INVALID_ARG;
    // ticks = us / period * 4096, with period = 4096 * (prescale + 1) / osc
    uint64_t ticks = static_cast<uint64_t>(us) * kOscHz / (1'000'000ull * (prescale_ + 1u));
    if (ticks > 4095) ticks = 4095;
    return write_channel(channel, 0, static_cast<uint16_t>(ticks));
}

esp_err_t Pca9685::set_off(int channel) {
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (channel < 0 || channel >= kChannels) return ESP_ERR_INVALID_ARG;
    return write_channel(channel, 0, kFullBit);
}

esp_err_t Pca9685::write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[] = {reg, value};
    return i2c_master_transmit(dev_, buf, sizeof(buf), kTimeoutMs);
}

esp_err_t Pca9685::write_channel(int channel, uint16_t on, uint16_t off) {
    const uint8_t buf[] = {
        static_cast<uint8_t>(kLed0OnL + 4 * channel),
        static_cast<uint8_t>(on & 0xFF), static_cast<uint8_t>(on >> 8),
        static_cast<uint8_t>(off & 0xFF), static_cast<uint8_t>(off >> 8),
    };
    return i2c_master_transmit(dev_, buf, sizeof(buf), kTimeoutMs);
}

}  // namespace motion
