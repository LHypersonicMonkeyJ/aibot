// M7 serial commands: I2C scan, per-joint servo angles, raw pulses, relax.

#include "servo_commands.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_log.h"
#include "i2c_bus.hpp"
#include "pca9685.hpp"
#include "sdkconfig.h"
#include "serial_protocol.hpp"
#include "servo_driver.hpp"

namespace {

constexpr const char* TAG = "servo_cmd";
constexpr uint32_t kServoHz = 50;

motion::Pca9685 g_pwm;
motion::ServoDriver g_servos(g_pwm);

// Splits off the first space-separated token.
std::string_view next_token(std::string_view& args) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    const size_t end = args.find(' ');
    const std::string_view tok = args.substr(0, end);
    args.remove_prefix(end == std::string_view::npos ? args.size() : end);
    return tok;
}

bool parse_float(std::string_view tok, float* out) {
    if (tok.empty()) return false;
    const std::string s(tok);
    char* end = nullptr;
    *out = std::strtof(s.c_str(), &end);
    return *end == '\0' && std::isfinite(*out);
}

bool parse_uint(std::string_view tok, uint32_t* out) {
    if (tok.empty()) return false;
    const std::string s(tok);
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    *out = static_cast<uint32_t>(v);
    return *end == '\0';
}

// Formats an angle with one decimal place without needing float printf.
const char* fmt_deg(float deg, char (&buf)[16]) {
    const long tenths = std::lround(deg * 10.0f);
    const long mag = tenths < 0 ? -tenths : tenths;
    std::snprintf(buf, sizeof(buf), "%s%ld.%ld", tenths < 0 ? "-" : "", mag / 10, mag % 10);
    return buf;
}

void reply_error(const char* msg) { comm::sendf("{\"type\":\"error\",\"msg\":\"%s\"}", msg); }

bool require_pwm() {
    if (g_servos.ready()) return true;
    // Retry, so the board can be wired up without rebooting.
    if (g_pwm.init(motion::i2c_bus(), CONFIG_AIBOT_PCA9685_ADDR, kServoHz) == ESP_OK) return true;
    reply_error("pca9685 not found, check wiring and run i2cscan");
    return false;
}

void cmd_i2cscan(std::string_view) {
    uint8_t found[16];
    const size_t n = motion::i2c_scan(found, sizeof(found));
    char list[16 * 4 + 1] = "";
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        pos += std::snprintf(list + pos, sizeof(list) - pos, "%s%u", i ? "," : "", found[i]);
    }
    comm::sendf("{\"type\":\"i2cscan\",\"found\":[%s]}", list);
}

// servo <joint> <deg>   joint is a name (head_yaw) or index (0-2)
void cmd_servo(std::string_view args) {
    const std::string_view joint_tok = next_token(args);
    const std::string_view deg_tok = next_token(args);
    const int joint = motion::ServoDriver::find_joint(joint_tok.data(), joint_tok.size());
    float deg = 0;
    if (joint < 0 || !parse_float(deg_tok, &deg)) {
        reply_error("usage: servo <body_yaw|head_yaw|head_pitch|0-2> <deg>");
        return;
    }
    if (!require_pwm()) return;

    motion::JointCommandResult r{};
    if (g_servos.set_angle(joint, deg, &r) != ESP_OK) {
        reply_error("i2c write failed");
        return;
    }
    char buf[16];
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"servo\",\"joint\":\"%s\",\"deg\":%s,\"pulse_us\":%lu,\"clamped\":%s}",
                motion::kJoints[joint].name, fmt_deg(r.applied_deg, buf),
                static_cast<unsigned long>(r.pulse_us), r.clamped ? "true" : "false");
}

// pulse <channel 0-15> <us>   raw pulse for calibration, ignores joint limits
void cmd_pulse(std::string_view args) {
    uint32_t channel = 0, us = 0;
    if (!parse_uint(next_token(args), &channel) || !parse_uint(next_token(args), &us) ||
        channel >= motion::Pca9685::kChannels) {
        reply_error("usage: pulse <channel 0-15> <us>");
        return;
    }
    if (!require_pwm()) return;

    uint32_t applied = 0;
    if (g_servos.set_pulse_raw(static_cast<int>(channel), us, &applied) != ESP_OK) {
        reply_error("i2c write failed");
        return;
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"pulse\",\"channel\":%lu,\"pulse_us\":%lu}",
                static_cast<unsigned long>(channel), static_cast<unsigned long>(applied));
}

// relax [joint]   stop pulses so the servo goes limp; no argument relaxes all
void cmd_relax(std::string_view args) {
    const std::string_view joint_tok = next_token(args);
    int joint = -1;
    if (!joint_tok.empty()) {
        joint = motion::ServoDriver::find_joint(joint_tok.data(), joint_tok.size());
        if (joint < 0) {
            reply_error("usage: relax [body_yaw|head_yaw|head_pitch|0-2]");
            return;
        }
    }
    if (!require_pwm()) return;

    const esp_err_t err = joint < 0 ? g_servos.relax_all() : g_servos.relax(joint);
    if (err != ESP_OK) {
        reply_error("i2c write failed");
        return;
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"relax\",\"joint\":\"%s\"}",
                joint < 0 ? "all" : motion::kJoints[joint].name);
}

}  // namespace

void servo_init() {
    esp_err_t err = motion::i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "I2C on SDA=%d SCL=%d", CONFIG_AIBOT_I2C_SDA_GPIO, CONFIG_AIBOT_I2C_SCL_GPIO);
    err = g_pwm.init(motion::i2c_bus(), CONFIG_AIBOT_PCA9685_ADDR, kServoHz);
    if (err != ESP_OK) {
        // Keep running: telemetry and i2cscan still work without the board.
        ESP_LOGW(TAG, "PCA9685 not available, servo commands disabled");
    }
}

bool servo_ready() { return g_servos.ready(); }

void register_servo_commands() {
    comm::register_command("i2cscan", cmd_i2cscan);
    comm::register_command("servo", cmd_servo);
    comm::register_command("pulse", cmd_pulse);
    comm::register_command("relax", cmd_relax);
}
