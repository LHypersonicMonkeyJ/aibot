#include "animator.hpp"

#include <algorithm>
#include <cmath>

namespace face {
namespace {

// Blink shape: eyelids close fast, hold, then open a little slower.
constexpr float kBlinkCloseMs = 70;
constexpr float kBlinkHoldMs = 30;
constexpr float kBlinkOpenMs = 110;
constexpr float kBlinkTotalMs = kBlinkCloseMs + kBlinkHoldMs + kBlinkOpenMs;
constexpr float kBlinkMinGapMs = 2000, kBlinkMaxGapMs = 6000;
constexpr float kDoubleBlinkChance = 0.15f;
constexpr float kDoubleBlinkGapMs = 120;

// Idle eye movement: quick darts (saccades) with rests between.
constexpr float kSaccadeMinGapMs = 1000, kSaccadeMaxGapMs = 4000;
constexpr float kSaccadeMs = 80;
constexpr float kIdleRangeX = 0.5f, kIdleRangeY = 0.4f;
constexpr float kReturnToCentreChance = 0.4f;
constexpr float kLookMs = 120;

// Talking: syllables of random length and openness, with short pauses.
constexpr float kSyllableMinMs = 110, kSyllableMaxMs = 220;
constexpr float kSyllableMinOpen = 0.3f, kSyllableMaxOpen = 1.0f;
constexpr float kPauseChance = 0.15f;
constexpr float kPauseMinMs = 150, kPauseMaxMs = 400;

// Lip sync smoothing time constants.
constexpr float kSpeechAttackMs = 25, kSpeechReleaseMs = 70;

// Large frame gaps (debugger, long hiccup) are capped so nothing jumps.
constexpr float kMaxStepMs = 100;

float ease_in_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
}

float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t);
}

float progress(float elapsed, float duration) {
    return duration <= 0 ? 1.0f : elapsed / duration;
}

}  // namespace

Animator::Animator(uint32_t seed) : rng_((seed * 2654435761u) | 1u) {
    // Scramble the seed so small seeds don't give a predictable start.
    for (int i = 0; i < 8; ++i) rand();
    from_ = to_ = kExpressions[0].face;
    schedule_blink();
    schedule_saccade();
}

uint32_t Animator::rand() {
    // xorshift32: small, fast and deterministic for tests.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

float Animator::uniform(float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(rand() >> 8) / static_cast<float>(1u << 24);
}

Face Animator::base() const {
    // Return the target itself once done, so a finished face compares equal.
    const float t = progress(trans_ms_, trans_dur_);
    return t >= 1 ? to_ : lerp(from_, to_, ease_in_out(t));
}

void Animator::schedule_blink() { next_blink_ms_ = uniform(kBlinkMinGapMs, kBlinkMaxGapMs); }

void Animator::schedule_saccade() {
    next_saccade_ms_ = uniform(kSaccadeMinGapMs, kSaccadeMaxGapMs);
}

void Animator::set_expression(const Face& target, float duration_ms) {
    from_ = base();
    to_ = target;
    trans_ms_ = 0;
    trans_dur_ = duration_ms;
}

void Animator::start_gaze(float x, float y, float duration_ms) {
    const float t = ease_out(progress(gaze_ms_, gaze_dur_));
    gaze_from_x_ = gaze_from_x_ + (gaze_to_x_ - gaze_from_x_) * t;
    gaze_from_y_ = gaze_from_y_ + (gaze_to_y_ - gaze_from_y_) * t;
    gaze_to_x_ = std::clamp(x, -1.0f, 1.0f);
    gaze_to_y_ = std::clamp(y, -1.0f, 1.0f);
    gaze_ms_ = 0;
    gaze_dur_ = duration_ms;
}

void Animator::look(float x, float y) {
    auto_ = false;
    start_gaze(x, y, kLookMs);
}

void Animator::blink() {
    if (blink_ms_ < 0) blink_ms_ = 0;
}

void Animator::set_auto(bool on) {
    if (on && !auto_) {
        schedule_blink();
        schedule_saccade();
    }
    auto_ = on;
}

float Animator::blink_value() const {
    if (blink_ms_ < 0) return 1;
    if (blink_ms_ < kBlinkCloseMs) return 1 - ease_in_out(blink_ms_ / kBlinkCloseMs);
    if (blink_ms_ < kBlinkCloseMs + kBlinkHoldMs) return 0;
    return ease_out((blink_ms_ - kBlinkCloseMs - kBlinkHoldMs) / kBlinkOpenMs);
}

Face Animator::update(float dt_ms) {
    dt_ms = std::clamp(dt_ms, 0.0f, kMaxStepMs);

    trans_ms_ = std::min(trans_ms_ + dt_ms, std::max(trans_dur_, 0.0f));
    gaze_ms_ = std::min(gaze_ms_ + dt_ms, std::max(gaze_dur_, 0.0f));

    if (blink_ms_ >= 0) {
        blink_ms_ += dt_ms;
        if (blink_ms_ >= kBlinkTotalMs) {
            blink_ms_ = -1;
            if (auto_) {
                if (uniform(0, 1) < kDoubleBlinkChance) next_blink_ms_ = kDoubleBlinkGapMs;
                else schedule_blink();
            }
        }
    }

    if (auto_) {
        next_blink_ms_ -= dt_ms;
        if (next_blink_ms_ <= 0 && blink_ms_ < 0) {
            blink_ms_ = 0;
            next_blink_ms_ = kBlinkMaxGapMs;  // rescheduled when this blink ends
        }
        next_saccade_ms_ -= dt_ms;
        if (next_saccade_ms_ <= 0) {
            // Only "return to centre" when away from it, so every dart is visible.
            const bool off_centre = gaze_to_x_ != 0 || gaze_to_y_ != 0;
            if (off_centre && uniform(0, 1) < kReturnToCentreChance) {
                start_gaze(0, 0, kSaccadeMs);
            } else {
                start_gaze(uniform(-kIdleRangeX, kIdleRangeX), uniform(-kIdleRangeY, kIdleRangeY),
                           kSaccadeMs);
            }
            schedule_saccade();
        }
    }

    // Talking: each syllable opens and closes the mouth along a sine hump.
    float talk_open = 0;
    if (syl_ms_ < syl_dur_) syl_ms_ += dt_ms;
    if (syl_ms_ >= syl_dur_ && talking_) {
        syl_ms_ = 0;
        if (uniform(0, 1) < kPauseChance) {
            syl_dur_ = uniform(kPauseMinMs, kPauseMaxMs);
            syl_peak_ = 0;
        } else {
            syl_dur_ = uniform(kSyllableMinMs, kSyllableMaxMs);
            syl_peak_ = uniform(kSyllableMinOpen, kSyllableMaxOpen);
        }
    }
    if (syl_ms_ < syl_dur_) {
        talk_open = syl_peak_ * std::sin(3.14159265f * syl_ms_ / syl_dur_);
    }

    // Lip sync: ease toward the level of the sound playing now.
    const float target = std::clamp(speech_target_, 0.0f, 1.0f);
    const float tau = target > speech_ ? kSpeechAttackMs : kSpeechReleaseMs;
    speech_ += (target - speech_) * (1 - std::exp(-dt_ms / tau));
    if (speech_ < 0.01f && target == 0) speech_ = 0;  // settle, so a still face stays still

    Face f = base();
    f.mouth.open = std::max({f.mouth.open, talk_open, speech_});
    const float g = ease_out(progress(gaze_ms_, gaze_dur_));
    f.gaze_x = std::clamp(f.gaze_x + gaze_from_x_ + (gaze_to_x_ - gaze_from_x_) * g, -1.0f, 1.0f);
    f.gaze_y = std::clamp(f.gaze_y + gaze_from_y_ + (gaze_to_y_ - gaze_from_y_) * g, -1.0f, 1.0f);
    f.blink = blink_value();
    return f;
}

}  // namespace face
