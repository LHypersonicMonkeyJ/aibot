#include "servo_driver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace motion {

int ServoDriver::find_joint(const char* s, size_t len) {
    if (len == 1 && s[0] >= '0' && s[0] < static_cast<char>('0' + kNumJoints)) return s[0] - '0';
    for (size_t i = 0; i < kNumJoints; ++i) {
        if (std::strlen(kJoints[i].name) == len && std::strncmp(kJoints[i].name, s, len) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

esp_err_t ServoDriver::set_angle(size_t joint, float deg, JointCommandResult* out) {
    if (joint >= kNumJoints || !std::isfinite(deg)) return ESP_ERR_INVALID_ARG;
    const JointConfig& j = kJoints[joint];

    const float applied = std::clamp(deg, j.min_deg, j.max_deg);
    const long pulse = std::lround(static_cast<float>(j.center_us) + applied * j.us_per_deg);
    const uint32_t us = static_cast<uint32_t>(std::clamp<long>(pulse, kMinPulseUs, kMaxPulseUs));

    const esp_err_t err = pwm_.set_pulse_us(j.channel, us);
    if (err == ESP_OK && out) *out = {applied, us, applied != deg};
    return err;
}

esp_err_t ServoDriver::relax(size_t joint) {
    if (joint >= kNumJoints) return ESP_ERR_INVALID_ARG;
    return pwm_.set_off(kJoints[joint].channel);
}

esp_err_t ServoDriver::relax_all() {
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < kNumJoints; ++i) {
        const esp_err_t err = relax(i);
        if (first_err == ESP_OK) first_err = err;
    }
    return first_err;
}

esp_err_t ServoDriver::set_pulse_raw(int channel, uint32_t us, uint32_t* applied_us) {
    const uint32_t clamped = std::clamp(us, kMinPulseUs, kMaxPulseUs);
    const esp_err_t err = pwm_.set_pulse_us(channel, clamped);
    if (err == ESP_OK && applied_us) *applied_us = clamped;
    return err;
}

}  // namespace motion
