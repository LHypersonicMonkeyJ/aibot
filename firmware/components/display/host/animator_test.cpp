// Unit tests for the face animator. Exit code 0 = all passed.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "animator.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

constexpr float kFrameMs = 1000.0f / 30;

const face::Face& expr(const char* name) {
    for (size_t i = 0; i < face::kNumExpressions; ++i) {
        if (std::string_view(face::kExpressions[i].name) == name) return face::kExpressions[i].face;
    }
    std::abort();
}

void test_auto_blinks_and_reopens() {
    face::Animator a(7);
    float min_blink = 1;
    int blinks = 0;
    bool closed = false;
    for (int i = 0; i < 30 * 20; ++i) {  // 20 s
        const float b = a.update(kFrameMs).blink;
        min_blink = std::min(min_blink, b);
        if (b < 0.1f && !closed) ++blinks;
        closed = b < 0.1f;
    }
    CHECK(min_blink < 0.1f);
    CHECK(blinks >= 3);   // gaps are 2-6 s
    CHECK(blinks <= 15);
    for (int i = 0; i < 10; ++i) a.update(kFrameMs);
}

void test_manual_blink_timing() {
    face::Animator a(3);
    a.set_auto(false);
    a.blink();
    CHECK(a.update(90).blink < 0.05f);   // shut after the close phase
    a.update(60);
    CHECK(a.update(60).blink == 1.0f);   // open again by ~210 ms
}

void test_auto_off_is_still() {
    face::Animator a(5);
    a.set_auto(false);
    const face::Face first = a.update(kFrameMs);
    for (int i = 0; i < 30 * 20; ++i) CHECK(a.update(kFrameMs) == first);
}

void test_transition_reaches_target() {
    face::Animator a(9);
    a.set_auto(false);
    a.update(kFrameMs);
    a.set_expression(expr("angry"), 200);
    const face::Face mid = a.update(100);
    CHECK(!(mid == expr("neutral")));
    CHECK(!(mid == expr("angry")));
    CHECK(a.update(150) == expr("angry"));
}

void test_change_mid_transition_is_continuous() {
    face::Animator a(11);
    a.set_auto(false);
    a.set_expression(expr("surprised"), 200);
    const face::Face before = a.update(100);
    a.set_expression(expr("sad"), 200);
    const face::Face after = a.update(0);
    CHECK(std::fabs(after.left.width - before.left.width) < 0.01f);
    CHECK(std::fabs(after.left.height - before.left.height) < 0.01f);
}

void test_gaze_in_range() {
    face::Animator a(13);
    float max_x = 0;
    for (int i = 0; i < 30 * 60; ++i) {
        const face::Face f = a.update(kFrameMs);
        CHECK(std::fabs(f.gaze_x) <= 1 && std::fabs(f.gaze_y) <= 1);
        max_x = std::max(max_x, std::fabs(f.gaze_x));
    }
    CHECK(max_x > 0.05f);  // the eyes did move
}

void test_look_holds_gaze() {
    face::Animator a(17);
    a.look(0.8f, -0.5f);
    CHECK(!a.auto_on());
    face::Face f{};
    for (int i = 0; i < 30 * 10; ++i) f = a.update(kFrameMs);
    CHECK(std::fabs(f.gaze_x - 0.8f) < 1e-4f);
    CHECK(std::fabs(f.gaze_y + 0.5f) < 1e-4f);
}

void test_talking_flaps_mouth() {
    face::Animator a(23);
    a.set_auto(false);
    float max_open = 0;
    int closed_frames = 0;
    a.set_talking(true);
    for (int i = 0; i < 60; ++i) {  // 2 s
        const float o = a.update(kFrameMs).mouth.open;
        max_open = std::max(max_open, o);
        if (o < 0.1f) ++closed_frames;
    }
    CHECK(max_open > 0.3f);
    CHECK(closed_frames > 0);  // it closes between syllables

    // Off: the current syllable finishes, then the mouth is back at rest.
    a.set_talking(false);
    for (int i = 0; i < 15; ++i) a.update(kFrameMs);  // 500 ms
    const float rest = expr("neutral").mouth.open;
    for (int i = 0; i < 30; ++i) CHECK(a.update(kFrameMs).mouth.open == rest);
}

void test_deterministic() {
    face::Animator a(21), b(21);
    for (int i = 0; i < 30 * 10; ++i) CHECK(a.update(kFrameMs) == b.update(kFrameMs));
}

}  // namespace

int main() {
    test_auto_blinks_and_reopens();
    test_manual_blink_timing();
    test_auto_off_is_still();
    test_transition_reaches_target();
    test_change_mid_transition_is_continuous();
    test_gaze_in_range();
    test_look_holds_gaze();
    test_talking_flaps_mouth();
    test_deterministic();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all animator tests passed\n");
    return 0;
}
