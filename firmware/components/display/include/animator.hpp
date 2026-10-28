#pragma once

// Brings the face to life: random blinks, idle eye movement, eased
// transitions between expressions, and a talking mouth.
//
// Plain C++ with no ESP-IDF dependencies, like face.hpp. Not thread-safe;
// the caller guards it if several tasks use it.

#include <cstdint>

#include "face.hpp"

namespace face {

class Animator {
public:
    explicit Animator(uint32_t seed = 1);

    // Blends from the current face to `target` over duration_ms.
    void set_expression(const Face& target, float duration_ms = 200);

    // Points the eyes (-1..1 each axis) and stops idle eye movement until
    // set_auto(true).
    void look(float x, float y);

    // Starts a blink now, unless one is already running.
    void blink();

    // Flaps the mouth in random syllables, as a stand-in until real speech
    // drives it. Turning it off lets the current syllable finish.
    void set_talking(bool on) { talking_ = on; }
    bool talking() const { return talking_; }

    // How open the mouth should be for the sound playing now, 0..1. The mouth
    // follows it smoothly: quick to open, a little slower to close.
    void set_speech(float open) { speech_target_ = open; }

    // Idle behaviour: random blinks and eye movement. On by default.
    void set_auto(bool on);
    bool auto_on() const { return auto_; }

    // Advances time by dt_ms and returns the face to draw.
    Face update(float dt_ms);

private:
    uint32_t rand();
    float uniform(float lo, float hi);
    void schedule_blink();
    void schedule_saccade();
    void start_gaze(float x, float y, float duration_ms);
    float blink_value() const;
    Face base() const;  // expression part of the face, before gaze and blink

    uint32_t rng_;
    bool auto_ = true;

    // Expression transition.
    Face from_, to_;
    float trans_ms_ = 0, trans_dur_ = 0;

    // Gaze offset added to the expression's own gaze.
    float gaze_from_x_ = 0, gaze_from_y_ = 0;
    float gaze_to_x_ = 0, gaze_to_y_ = 0;
    float gaze_ms_ = 0, gaze_dur_ = 0;
    float next_saccade_ms_ = 0;

    // Talking: one syllable at a time; peak 0 is a pause.
    bool talking_ = false;
    float syl_ms_ = 0, syl_dur_ = 0, syl_peak_ = 0;

    float speech_target_ = 0, speech_ = 0;

    // Blink: -1 when not blinking, else time since it started.
    float blink_ms_ = -1;
    float next_blink_ms_ = 0;
};

}  // namespace face
