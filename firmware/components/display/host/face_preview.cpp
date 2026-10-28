// Renders every expression, a blink and an expression transition to PNG,
// as they would look on the round LCD.
//
//   face_preview <out_dir>
//
// Writes <name>.png per expression, sheet.png (all expressions in a grid, in
// kExpressions order), sheet_eyes_only.png (the same without a mouth),
// talking.png (the mouth opening and closing), blink.png, gaze.png, transition.png (neutral -> angry)
// and animation.png (6 s of the animator, one frame per 100 ms, switching to
// happy at 3 s).

#include <cstdio>
#include <string>
#include <vector>

#include "animator.hpp"
#include "face.hpp"
#include "png.hpp"

namespace {

constexpr int kN = face::kSize;
constexpr int kGap = 8;
constexpr uint8_t kBezel = 40;  // grey outside the round glass

// Draws one face into an RGB canvas at (ox, oy), masked to the round screen.
void draw(const face::Face& f, std::vector<uint8_t>& rgb, int canvas_w, int ox, int oy) {
    face::Renderer r;
    r.set(f);
    uint8_t cov[kN];
    for (int y = 0; y < kN; ++y) {
        r.row(y, cov);
        for (int x = 0; x < kN; ++x) {
            const float dx = x + 0.5f - kN / 2.0f, dy = y + 0.5f - kN / 2.0f;
            uint8_t* p = &rgb[((oy + y) * canvas_w + ox + x) * 3];
            if (dx * dx + dy * dy > (kN / 2.0f) * (kN / 2.0f)) {
                p[0] = p[1] = p[2] = kBezel;
                continue;
            }
            p[0] = f.color.r * cov[x] / 255;
            p[1] = f.color.g * cov[x] / 255;
            p[2] = f.color.b * cov[x] / 255;
        }
    }
}

// Lays faces out in a grid and writes one PNG.
bool sheet(const std::string& path, const std::vector<face::Face>& faces, int cols) {
    const int rows = (static_cast<int>(faces.size()) + cols - 1) / cols;
    const int w = cols * kN + (cols + 1) * kGap;
    const int h = rows * kN + (rows + 1) * kGap;
    std::vector<uint8_t> rgb(w * h * 3, 20);
    for (size_t i = 0; i < faces.size(); ++i) {
        const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
        draw(faces[i], rgb, w, kGap + c * (kN + kGap), kGap + r * (kN + kGap));
    }
    if (!write_png(path, w, h, rgb)) {
        std::fprintf(stderr, "failed to write %s\n", path.c_str());
        return false;
    }
    std::printf("wrote %s\n", path.c_str());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out_dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/";
    bool ok = true;

    std::vector<face::Face> all;
    for (size_t i = 0; i < face::kNumExpressions; ++i) {
        const auto& e = face::kExpressions[i];
        all.push_back(e.face);
        ok &= sheet(dir + e.name + ".png", {e.face}, 1);
    }
    ok &= sheet(dir + "sheet.png", all, 4);

    // The same expressions without a mouth, for comparison.
    std::vector<face::Face> eyes_only = all;
    for (auto& f : eyes_only) f.mouth.width = 0;
    ok &= sheet(dir + "sheet_eyes_only.png", eyes_only, 4);

    std::vector<face::Face> talking;
    for (float o : {0.0f, 0.3f, 0.7f, 1.0f, 0.5f, 0.15f}) {
        face::Face f = face::kExpressions[0].face;
        f.mouth.open = o;
        talking.push_back(f);
    }
    ok &= sheet(dir + "talking.png", talking, 6);

    std::vector<face::Face> blink;
    for (float b : {1.0f, 0.6f, 0.25f, 0.0f, 0.4f, 1.0f}) {
        face::Face f = face::kExpressions[0].face;
        f.blink = b;
        blink.push_back(f);
    }
    ok &= sheet(dir + "blink.png", blink, 6);

    std::vector<face::Face> gaze;
    for (auto [gx, gy] : {std::pair{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}}) {
        face::Face f = face::kExpressions[0].face;
        f.gaze_x = gx;
        f.gaze_y = gy;
        gaze.push_back(f);
    }
    ok &= sheet(dir + "gaze.png", gaze, 4);

    std::vector<face::Face> trans;
    const face::Face* from = face::find_expression("neutral", 7);
    const face::Face* to = face::find_expression("angry", 5);
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) trans.push_back(face::lerp(*from, *to, t));
    ok &= sheet(dir + "transition.png", trans, 5);

    face::Animator anim(42);
    std::vector<face::Face> frames;
    for (int ms = 0; ms < 6000; ms += 10) {
        if (ms == 3000) anim.set_expression(*face::find_expression("happy", 5));
        const face::Face f = anim.update(10);
        if (ms % 100 == 0) frames.push_back(f);
    }
    ok &= sheet(dir + "animation.png", frames, 10);

    return ok ? 0 : 1;
}
