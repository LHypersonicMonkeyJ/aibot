#include "face.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace face {
namespace {

// Vertical anti-aliasing: each screen row is sampled at this many sub-rows.
// Horizontal edges are covered exactly from the span end points.
constexpr int kSubRows = 4;
constexpr int kSubWeight = 256 / kSubRows;

// Scale of the eye on the side the face looks toward, for a hint of depth.
constexpr float kPerspective = 0.08f;

float mix(float a, float b, float t) { return a + (b - a) * t; }

uint8_t mix8(uint8_t a, uint8_t b, float t) {
    return static_cast<uint8_t>(std::lround(mix(a, b, t)));
}

Eye lerp_eye(const Eye& a, const Eye& b, float t) {
    return {mix(a.width, b.width, t), mix(a.height, b.height, t), mix(a.radius, b.radius, t),
            mix(a.open, b.open, t),   mix(a.lid, b.lid, t),       mix(a.tilt, b.tilt, t),
            mix(a.smile, b.smile, t)};
}

// Adds weight w over [a, b) in pixel coordinates, with partial end pixels.
void add_span(uint16_t* acc, float a, float b, int w) {
    a = std::max(a, 0.0f);
    b = std::min(b, static_cast<float>(kSize));
    if (b <= a) return;
    const int ia = static_cast<int>(a);
    const int ib = static_cast<int>(b);
    if (ia == ib) {
        acc[ia] += static_cast<uint16_t>((b - a) * w);
        return;
    }
    acc[ia] += static_cast<uint16_t>((ia + 1 - a) * w);
    for (int i = ia + 1; i < ib; ++i) acc[i] += w;
    if (ib < kSize) acc[ib] += static_cast<uint16_t>((b - ib) * w);
}

Eye eye(float w, float h, float r, float open = 1, float lid = 0, float tilt = 0, float smile = 0) {
    return {w, h, r, open, lid, tilt, smile};
}

Face both(const Eye& e) {
    Face f;
    f.left = e;
    f.right = e;
    return f;
}

Face make_thinking() {
    Face f;
    f.left = eye(62, 74, 20, 1, 0.30f, 0.10f);
    f.right = eye(66, 82, 22);
    f.gaze_x = 0.6f;
    f.gaze_y = -0.6f;
    return f;
}

Face make_listening() {
    Face f = both(eye(68, 88, 24));
    f.y = -4;
    return f;
}

}  // namespace

Face lerp(const Face& a, const Face& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    Face f;
    f.left = lerp_eye(a.left, b.left, t);
    f.right = lerp_eye(a.right, b.right, t);
    f.spacing = mix(a.spacing, b.spacing, t);
    f.y = mix(a.y, b.y, t);
    f.gaze_x = mix(a.gaze_x, b.gaze_x, t);
    f.gaze_y = mix(a.gaze_y, b.gaze_y, t);
    f.blink = mix(a.blink, b.blink, t);
    f.color = {mix8(a.color.r, b.color.r, t), mix8(a.color.g, b.color.g, t),
               mix8(a.color.b, b.color.b, t)};
    return f;
}

const Expression kExpressions[] = {
    {"neutral", both(eye(64, 80, 20))},
    {"happy", both(eye(66, 76, 30, 1, 0, 0, 0.55f))},
    {"sad", both(eye(62, 72, 18, 1, 0.22f, -0.40f))},
    {"angry", both(eye(64, 72, 14, 1, 0.28f, 0.50f))},
    {"surprised", both(eye(74, 94, 37))},
    {"sleepy", both(eye(66, 80, 16, 0.45f, 0.25f))},
    {"listening", make_listening()},
    {"thinking", make_thinking()},
};
const size_t kNumExpressions = sizeof(kExpressions) / sizeof(kExpressions[0]);

const Face* find_expression(const char* name, size_t len) {
    for (const auto& e : kExpressions) {
        if (std::strlen(e.name) == len && std::strncmp(e.name, name, len) == 0) return &e.face;
    }
    return nullptr;
}

void Renderer::set(const Face& f) {
    const float blink = std::clamp(f.blink, 0.0f, 1.0f);
    for (int i = 0; i < 2; ++i) {
        const Eye& e = i == 0 ? f.left : f.right;
        const float side = i == 0 ? -1.0f : 1.0f;
        const float scale = 1.0f + kPerspective * f.gaze_x * side;

        const float w = e.width * scale;
        const float h = std::max(e.height * scale * std::clamp(e.open, 0.0f, 1.0f) * blink, 2.0f);
        const float cx = kSize / 2.0f + side * f.spacing / 2.0f + f.gaze_x * kGazeX;
        const float cy = kSize / 2.0f + f.y + f.gaze_y * kGazeY;

        EyeGeom& g = eyes_[i];
        g.x0 = cx - w / 2;
        g.x1 = cx + w / 2;
        g.y0 = cy - h / 2;
        g.y1 = cy + h / 2;
        g.r = std::min({e.radius * scale, w / 2, h / 2});
        g.cx = cx;

        // Left eye's inner side is +x, so "inner corner lower" is a positive slope.
        g.has_lid = e.lid > 0 || e.tilt != 0;
        g.lid_y = g.y0 + e.lid * h;
        g.slope = e.tilt * -side;

        g.has_cut = e.smile > 0;
        g.cut_r = w * 0.9f;
        g.cut_cx = cx;
        g.cut_cy = g.y1 + g.cut_r - e.smile * h;
    }
}

void Renderer::row(int y, uint8_t* coverage) const {
    uint16_t acc[kSize];
    std::memset(acc, 0, sizeof(acc));
    bool any = false;

    for (const EyeGeom& g : eyes_) {
        if (y + 1 <= g.y0 || y >= g.y1) continue;
        any = true;
        for (int k = 0; k < kSubRows; ++k) {
            const float sy = y + (k + 0.5f) / kSubRows;
            if (sy < g.y0 || sy >= g.y1) continue;

            // Rounded rectangle span.
            float inset = 0;
            const float top = g.y0 + g.r - sy;
            const float bottom = sy - (g.y1 - g.r);
            const float t = std::max(top, bottom);
            if (t > 0) inset = g.r - std::sqrt(std::max(g.r * g.r - t * t, 0.0f));
            float a = g.x0 + inset;
            float b = g.x1 - inset;

            // Upper lid: keep points on or below the line y = lid_y + slope * (x - cx).
            if (g.has_lid) {
                if (g.slope == 0) {
                    if (sy < g.lid_y) continue;
                } else {
                    const float x_at = g.cx + (sy - g.lid_y) / g.slope;
                    if (g.slope > 0) b = std::min(b, x_at);
                    else a = std::max(a, x_at);
                }
            }
            if (b <= a) continue;

            // Lower cut-out circle removes the middle of the span.
            if (g.has_cut) {
                const float dy = sy - g.cut_cy;
                if (dy * dy < g.cut_r * g.cut_r) {
                    const float half = std::sqrt(g.cut_r * g.cut_r - dy * dy);
                    add_span(acc, a, std::min(b, g.cut_cx - half), kSubWeight);
                    add_span(acc, std::max(a, g.cut_cx + half), b, kSubWeight);
                    continue;
                }
            }
            add_span(acc, a, b, kSubWeight);
        }
    }

    if (!any) {
        std::memset(coverage, 0, kSize);
        return;
    }
    for (int x = 0; x < kSize; ++x) coverage[x] = static_cast<uint8_t>(std::min<int>(acc[x], 255));
}

}  // namespace face
