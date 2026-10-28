#include "face.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace face {
namespace {

// Vertical anti-aliasing: each screen row is sampled at this many sub-rows.
// Horizontal edges are covered exactly from the span end points.
constexpr int kSubRows = 4;
constexpr int kSubWeight = 256 / kSubRows;

// Scale of the eye on the side the face looks toward, for a hint of depth.
constexpr float kPerspective = 0.08f;

// Mouth layout. With a mouth, the eyes move up and shrink to make room.
constexpr float kMouthY = 54;          // mouth centre below the face centre, px
constexpr float kMouthBend = 14;       // corner-to-centre height at curve +-1, px
constexpr float kMouthOpen = 26;       // extra height at open = 1, px
constexpr float kMouthEyeShift = 18;   // eyes move up by this much, px
constexpr float kMouthEyeScale = 0.9f;
// The mouth follows the gaze a little less than the eyes, for depth.
constexpr float kMouthGazeX = 0.6f, kMouthGazeY = 0.5f;

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

struct Span {
    float a, b;
};

// The s range in [0, 1] where A + B*s <= v.
bool solve_le(float A, float B, float v, float* lo, float* hi) {
    *lo = 0;
    *hi = 1;
    if (B == 0) return A <= v;
    const float s = (v - A) / B;
    if (B > 0) *hi = std::min(1.0f, s);
    else *lo = std::max(0.0f, s);
    return *lo <= *hi;
}

Mouth mouth(float width, float curve, float open = 0, float x = 0, float thickness = 7) {
    return {width, curve, open, thickness, x};
}

Face with_mouth(Face f, const Mouth& m) {
    f.mouth = m;
    return f;
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
    f.mouth = {mix(a.mouth.width, b.mouth.width, t), mix(a.mouth.curve, b.mouth.curve, t),
               mix(a.mouth.open, b.mouth.open, t), mix(a.mouth.thickness, b.mouth.thickness, t),
               mix(a.mouth.x, b.mouth.x, t)};
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
    {"neutral", with_mouth(both(eye(64, 80, 20)), mouth(34, 0.15f))},
    {"happy", with_mouth(both(eye(66, 76, 30, 1, 0, 0, 0.55f)), mouth(58, 1.0f))},
    {"sad", with_mouth(both(eye(62, 72, 18, 1, 0.22f, -0.40f)), mouth(42, -0.8f))},
    {"angry", with_mouth(both(eye(64, 72, 14, 1, 0.28f, 0.50f)), mouth(38, -0.35f, 0, 0, 8))},
    {"surprised", with_mouth(both(eye(74, 94, 37)), mouth(14, 0, 0.75f))},
    {"sleepy", with_mouth(both(eye(66, 80, 16, 0.45f, 0.25f)), mouth(22, 0))},
    {"listening", with_mouth(make_listening(), mouth(26, 0.2f, 0.2f))},
    {"thinking", with_mouth(make_thinking(), mouth(24, -0.25f, 0, 16))},
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
    const bool has_mouth = f.mouth.width > 0;
    const float eye_scale = has_mouth ? kMouthEyeScale : 1.0f;
    const float eye_shift = has_mouth ? -kMouthEyeShift : 0.0f;

    for (int i = 0; i < 2; ++i) {
        const Eye& e = i == 0 ? f.left : f.right;
        const float side = i == 0 ? -1.0f : 1.0f;
        const float scale = (1.0f + kPerspective * f.gaze_x * side) * eye_scale;

        const float w = e.width * scale;
        const float h = std::max(e.height * scale * std::clamp(e.open, 0.0f, 1.0f) * blink, 2.0f);
        const float cx = kSize / 2.0f + side * f.spacing / 2.0f + f.gaze_x * kGazeX;
        const float cy = kSize / 2.0f + eye_shift + f.y + f.gaze_y * kGazeY;

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

    // Mouth: a band around the centre line y = my + bend * (0.5 - u^2), so a
    // smile has its centre low and corners high. Opening adds an oval over the
    // middle of the line, hanging slightly below it.
    MouthGeom& m = mouth_;
    m.present = has_mouth;
    if (!has_mouth) return;
    const Mouth& mo = f.mouth;
    const float t = std::max(mo.thickness, 1.0f);
    const float bend = std::clamp(mo.curve, -1.0f, 1.0f) * kMouthBend;
    const float open = std::clamp(mo.open, 0.0f, 1.0f) * kMouthOpen;
    const float my = kSize / 2.0f + kMouthY + f.y + f.gaze_y * kGazeY * kMouthGazeY;
    m.cx = kSize / 2.0f + mo.x + f.gaze_x * kGazeX * kMouthGazeX;
    m.half_w = mo.width / 2;
    m.top_a = my + bend / 2 - t / 2;
    m.top_b = -bend;
    m.bot_a = my + bend / 2 + t / 2;
    m.bot_b = -bend;
    m.cap_r = t / 2;
    m.cap_ly = m.cap_ry = my - bend / 2;
    m.has_oval = open > 0;
    m.oval_rx = m.half_w + t / 2;
    m.oval_ry = t / 2 + open / 2;
    m.oval_cy = my + bend / 2 + open * 0.2f;
    m.y0 = std::min({m.top_a, m.top_a + m.top_b, m.cap_ly - m.cap_r});
    m.y1 = std::max({m.bot_a, m.bot_a + m.bot_b, m.cap_ly + m.cap_r});
    if (m.has_oval) {
        m.y0 = std::min(m.y0, m.oval_cy - m.oval_ry);
        m.y1 = std::max(m.y1, m.oval_cy + m.oval_ry);
    }
}

void Renderer::mouth_row(float sy, uint16_t* acc) const {
    const MouthGeom& m = mouth_;
    Span spans[5];
    int n = 0;

    // Band: s = u^2 where the upper edge is above sy and the lower edge below.
    float lo1, hi1, lo2, hi2;
    if (solve_le(m.top_a, m.top_b, sy, &lo1, &hi1) &&
        solve_le(-m.bot_a, -m.bot_b, -sy, &lo2, &hi2)) {
        const float lo = std::max(lo1, lo2), hi = std::min(hi1, hi2);
        if (lo <= hi) {
            const float ul = std::sqrt(lo) * m.half_w, uh = std::sqrt(hi) * m.half_w;
            if (ul == 0) {
                spans[n++] = {m.cx - uh, m.cx + uh};
            } else {
                spans[n++] = {m.cx - uh, m.cx - ul};
                spans[n++] = {m.cx + ul, m.cx + uh};
            }
        }
    }
    // Round end caps.
    for (const float cap_x : {m.cx - m.half_w, m.cx + m.half_w}) {
        const float dy = sy - m.cap_ly;
        if (dy * dy < m.cap_r * m.cap_r) {
            const float half = std::sqrt(m.cap_r * m.cap_r - dy * dy);
            spans[n++] = {cap_x - half, cap_x + half};
        }
    }
    // Open mouth oval.
    if (m.has_oval) {
        const float dy = (sy - m.oval_cy) / m.oval_ry;
        if (dy * dy < 1) {
            const float half = m.oval_rx * std::sqrt(1 - dy * dy);
            spans[n++] = {m.cx - half, m.cx + half};
        }
    }
    // Merge overlaps so no pixel is counted twice.
    for (int i = 1; i < n; ++i) {  // insertion sort: at most 5 spans
        for (int j = i; j > 0 && spans[j].a < spans[j - 1].a; --j) std::swap(spans[j], spans[j - 1]);
    }
    for (int i = 0; i < n;) {
        Span cur = spans[i++];
        while (i < n && spans[i].a <= cur.b) cur.b = std::max(cur.b, spans[i++].b);
        add_span(acc, cur.a, cur.b, kSubWeight);
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

    if (mouth_.present && y + 1 > mouth_.y0 && y < mouth_.y1) {
        any = true;
        for (int k = 0; k < kSubRows; ++k) mouth_row(y + (k + 0.5f) / kSubRows, acc);
    }

    if (!any) {
        std::memset(coverage, 0, kSize);
        return;
    }
    for (int x = 0; x < kSize; ++x) coverage[x] = static_cast<uint8_t>(std::min<int>(acc[x], 255));
}

}  // namespace face
