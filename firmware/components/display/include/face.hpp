#pragma once

// Procedural two-eye face for the 240x240 round LCD.
//
// Plain C++ with no ESP-IDF dependencies, so the same code renders on the
// robot and on a PC (see host/ for previews and tests).
//
// Each eye is a rounded rectangle, cut by an upper lid line (tilted for
// angry or sad) and by a circle pushing up from below (a happy crescent).
// The renderer outputs per-pixel coverage (0-255) one row at a time; the
// caller maps coverage to its own pixel format.

#include <cstddef>
#include <cstdint>

namespace face {

inline constexpr int kSize = 240;

// Max eye travel for gaze -1..1, in pixels.
inline constexpr float kGazeX = 20.0f;
inline constexpr float kGazeY = 18.0f;

struct Eye {
    float width = 64;   // px
    float height = 80;  // px, fully open
    float radius = 20;  // corner radius, px
    float open = 1;     // 0..1 vertical squash about the eye centre (sleepy, wink)
    float lid = 0;      // upper lid drop at the eye centre, 0..1 of eye height
    float tilt = 0;     // upper lid slope: + inner corner lower (angry), - outer lower (sad)
    float smile = 0;    // lower lid rise, 0..1 of eye height (happy crescent)
};

struct Color {
    uint8_t r, g, b;
};

struct Face {
    Eye left, right;      // as seen on screen
    float spacing = 96;   // distance between eye centres, px
    float y = 0;          // vertical offset of both eyes, px, + is down
    float gaze_x = 0;     // -1 screen left .. 1 screen right
    float gaze_y = 0;     // -1 up .. 1 down
    float blink = 1;      // animation multiplier on openness: 1 open, 0 shut
    Color color = {40, 210, 255};
};

// Blends every parameter, t in 0..1.
Face lerp(const Face& a, const Face& b, float t);

struct Expression {
    const char* name;
    Face face;
};

extern const Expression kExpressions[];
extern const size_t kNumExpressions;

// Returns the expression with this name, or nullptr.
const Face* find_expression(const char* name, size_t len);

class Renderer {
public:
    // Precomputes eye geometry for one frame.
    void set(const Face& f);

    // Fills kSize coverage values (0 background .. 255 eye) for screen row y.
    void row(int y, uint8_t* coverage) const;

private:
    struct EyeGeom {
        float x0, x1, y0, y1;  // bounding rectangle
        float r;               // corner radius
        float cx;              // centre x
        float lid_y, slope;    // upper lid line through (cx, lid_y)
        bool has_lid;
        float cut_cx, cut_cy, cut_r;  // lower cut-out circle
        bool has_cut;
    };
    EyeGeom eyes_[2] = {};
};

}  // namespace face
