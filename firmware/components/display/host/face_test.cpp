// Unit tests for the face renderer. Exit code 0 = all passed.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "face.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

// Sum of coverage over the whole frame, and over each screen half.
struct Totals {
    long all, left, right, top, bottom;
};

Totals render(const face::Face& f) {
    face::Renderer r;
    r.set(f);
    Totals t{};
    uint8_t cov[face::kSize];
    for (int y = 0; y < face::kSize; ++y) {
        r.row(y, cov);
        for (int x = 0; x < face::kSize; ++x) {
            t.all += cov[x];
            (x < face::kSize / 2 ? t.left : t.right) += cov[x];
            (y < face::kSize / 2 ? t.top : t.bottom) += cov[x];
        }
    }
    return t;
}

void test_neutral_area() {
    // A rounded rectangle's area: w*h - (4 - pi) r^2. No mouth, so the eyes
    // keep their full size.
    face::Face f = *face::find_expression("neutral", 7);
    f.mouth.width = 0;
    const double expected = 64.0 * 80.0 - (4 - M_PI) * 20 * 20;
    const double area = render(f).all / 255.0 / 2;  // per eye
    CHECK(std::fabs(area - expected) / expected < 0.01);
}

// Coverage in rows [y0, y1).
long band(const face::Face& f, int y0, int y1, int x0 = 0, int x1 = face::kSize) {
    face::Renderer r;
    r.set(f);
    long sum = 0;
    uint8_t cov[face::kSize];
    for (int y = y0; y < y1; ++y) {
        r.row(y, cov);
        for (int x = x0; x < x1; ++x) sum += cov[x];
    }
    return sum;
}

void test_mouth_drawn_below_eyes() {
    face::Face f = *face::find_expression("neutral", 7);
    CHECK(band(f, 155, 200) > 0);
    f.mouth.width = 0;
    CHECK(band(f, 165, 240) == 0);  // no mouth: nothing below the eyes
}

void test_smile_and_frown_bend() {
    // A smile's centre sits below its corners; a frown's above.
    face::Face f = *face::find_expression("neutral", 7);
    f.mouth = {60, 1, 0, 7, 0};
    const int c = face::kSize / 2;
    const long smile_low = band(f, 178, 200, c - 4, c + 4);
    const long smile_high = band(f, 150, 172, c - 4, c + 4);
    CHECK(smile_low > smile_high);
    f.mouth.curve = -1;
    const long frown_low = band(f, 178, 200, c - 4, c + 4);
    const long frown_high = band(f, 150, 172, c - 4, c + 4);
    CHECK(frown_high > frown_low);
}

void test_mouth_opens() {
    face::Face f = *face::find_expression("neutral", 7);
    const long closed = band(f, 150, 240);
    f.mouth.open = 1;
    const long open = band(f, 150, 240);
    CHECK(open > closed * 2);
}

void test_mouth_symmetric() {
    face::Face f = *face::find_expression("happy", 5);
    const long left = band(f, 150, 240, 0, face::kSize / 2);
    const long right = band(f, 150, 240, face::kSize / 2, face::kSize);
    CHECK(std::labs(left - right) <= left / 200 + 1);
}

void test_symmetric() {
    const Totals t = render(*face::find_expression("neutral", 7));
    CHECK(std::labs(t.left - t.right) < t.all / 1000);
}

void test_blink_closes() {
    face::Face f = *face::find_expression("neutral", 7);
    f.mouth.width = 0;  // the mouth doesn't blink
    const long open = render(f).all;
    f.blink = 0;
    const long shut = render(f).all;
    CHECK(shut > 0);              // a thin line stays visible
    CHECK(shut < open / 20);
}

void test_gaze_moves_eyes() {
    face::Face f = *face::find_expression("neutral", 7);
    f.gaze_x = 1;
    const Totals r = render(f);
    CHECK(r.right > r.left);
    f.gaze_x = 0;
    f.gaze_y = -1;
    const Totals u = render(f);
    CHECK(u.top > u.bottom);
}

void test_expressions_smaller_than_neutral() {
    // Lids and smiles only ever remove area from the eye shape.
    const long neutral = render(*face::find_expression("neutral", 7)).all;
    for (const char* name : {"happy", "sad", "angry", "sleepy"}) {
        const long a = render(*face::find_expression(name, std::strlen(name))).all;
        CHECK(a > 0);
        CHECK(a < neutral);
    }
}

void test_coverage_in_range_and_clipped() {
    // Extreme gaze pushes eyes towards the edge without writing out of bounds.
    face::Face f = *face::find_expression("surprised", 9);
    f.gaze_x = -1;
    f.gaze_y = 1;
    f.spacing = 230;
    CHECK(render(f).all > 0);
}

void test_lerp_endpoints() {
    const face::Face a = *face::find_expression("neutral", 7);
    const face::Face b = *face::find_expression("angry", 5);
    CHECK(render(face::lerp(a, b, 0)).all == render(a).all);
    CHECK(render(face::lerp(a, b, 1)).all == render(b).all);
}

void test_find_expression() {
    CHECK(face::find_expression("happy", 5) != nullptr);
    CHECK(face::find_expression("happ", 4) == nullptr);
    CHECK(face::find_expression("nope", 4) == nullptr);
}

}  // namespace

int main() {
    test_neutral_area();
    test_symmetric();
    test_mouth_drawn_below_eyes();
    test_smile_and_frown_bend();
    test_mouth_opens();
    test_mouth_symmetric();
    test_blink_closes();
    test_gaze_moves_eyes();
    test_expressions_smaller_than_neutral();
    test_coverage_in_range_and_clipped();
    test_lerp_endpoints();
    test_find_expression();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all face tests passed\n");
    return 0;
}
