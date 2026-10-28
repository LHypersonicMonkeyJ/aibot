#pragma once

#include <cstddef>
#include <cstdint>

#include "pca9685.hpp"

namespace motion {

// One joint driven by one hobby servo. Angles are degrees from neutral.
struct JointConfig {
    const char* name;
    int channel;        // PCA9685 output
    float min_deg;      // joint limits, enforced before any pulse is sent
    float max_deg;
    uint32_t center_us; // pulse width at 0 deg (trim per servo)
    float us_per_deg;   // negative to reverse direction
};

// Order matches the sim's joint vector q = [body yaw, head yaw, head pitch].
// Limits mirror sim/robot_sim/joints.py and are placeholders until the
// mechanical design is done.
inline constexpr JointConfig kJoints[] = {
    {"body_yaw", 0, -90.0f, 90.0f, 1500, 2000.0f / 180.0f},
    {"head_yaw", 1, -60.0f, 60.0f, 1500, 2000.0f / 180.0f},
    {"head_pitch", 2, -20.0f, 30.0f, 1500, 2000.0f / 180.0f},
};
inline constexpr size_t kNumJoints = sizeof(kJoints) / sizeof(kJoints[0]);

// Absolute pulse range sent to any servo, whatever the joint config says.
inline constexpr uint32_t kMinPulseUs = 500;
inline constexpr uint32_t kMaxPulseUs = 2500;

struct JointCommandResult {
    float applied_deg;  // after clamping to joint limits
    uint32_t pulse_us;
    bool clamped;
};

class ServoDriver {
public:
    explicit ServoDriver(Pca9685& pwm) : pwm_(pwm) {}

    bool ready() const { return pwm_.ready(); }

    // Returns the joint index for a name or a digit string, or -1.
    static int find_joint(const char* name_or_index, size_t len);

    // Moves a joint straight to an angle (no trajectory yet; that's M8).
    esp_err_t set_angle(size_t joint, float deg, JointCommandResult* out);
    esp_err_t relax(size_t joint);
    esp_err_t relax_all();

    // Raw pulse on any channel, for calibration. Clamped to the absolute range.
    esp_err_t set_pulse_raw(int channel, uint32_t us, uint32_t* applied_us);

private:
    Pca9685& pwm_;
};

}  // namespace motion
