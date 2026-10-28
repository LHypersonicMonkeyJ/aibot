// M2 display: the animated face task, plus serial commands for expressions,
// gaze, blinks, LCD test patterns, orientation/colour fixes and backlight.

#include "display_commands.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "animator.hpp"
#include "audio.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "face.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.hpp"
#include "sdkconfig.h"
#include "serial_protocol.hpp"

namespace {

constexpr const char* TAG = "display";
constexpr uint32_t kFramePeriodMs = 33;  // ~30 fps

// Lip sync: speech loudness (RMS, 0..1) to mouth openness. Quieter than the
// floor is treated as silence; the gain puts normal speech near fully open.
constexpr float kSpeechFloor = 0.02f;
constexpr float kSpeechGain = 4.0f;

std::string_view next_token(std::string_view& args) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    const size_t end = args.find(' ');
    const std::string_view tok = args.substr(0, end);
    args.remove_prefix(end == std::string_view::npos ? args.size() : end);
    return tok;
}

bool parse_int(std::string_view tok, int* out) {
    if (tok.empty()) return false;
    const std::string s(tok);
    char* end = nullptr;
    *out = static_cast<int>(std::strtol(s.c_str(), &end, 10));
    return *end == '\0';
}

bool parse_float(std::string_view tok, float* out) {
    if (tok.empty()) return false;
    const std::string s(tok);
    char* end = nullptr;
    *out = std::strtof(s.c_str(), &end);
    return *end == '\0' && std::isfinite(*out);
}

// Formats a value with two decimals without needing float printf.
const char* fmt2(float v, char (&buf)[16]) {
    const long h = std::lround(v * 100.0f);
    const long mag = h < 0 ? -h : h;
    std::snprintf(buf, sizeof(buf), "%s%ld.%02ld", h < 0 ? "-" : "", mag / 100, mag % 100);
    return buf;
}

void reply_error(const char* msg) { comm::sendf("{\"type\":\"error\",\"msg\":\"%s\"}", msg); }

// --- Test images ---

struct Solid {
    uint16_t color;
};

void fill_solid(uint16_t* px, int, int rows, void* ctx) {
    const uint16_t c = static_cast<Solid*>(ctx)->color;
    for (int i = 0; i < display::kWidth * rows; ++i) px[i] = c;
}

bool in_disc(int x, int y, int cx, int cy, int r) {
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

// Red disc at the top, green on the right, blue at the bottom, a white dot in
// the centre and a white ring just inside the edge. Each mistake shows up
// differently: red/blue swapped, green on the left (mirror x), red at the
// bottom (mirror y), white background (invert), ring off-centre (offset).
void fill_pattern(uint16_t* px, int y0, int rows, void*) {
    const uint16_t black = display::rgb565(0, 0, 0);
    const uint16_t white = display::rgb565(255, 255, 255);
    const uint16_t red = display::rgb565(255, 0, 0);
    const uint16_t green = display::rgb565(0, 255, 0);
    const uint16_t blue = display::rgb565(0, 0, 255);
    constexpr int c = display::kWidth / 2;

    for (int row = 0; row < rows; ++row) {
        const int y = y0 + row;
        for (int x = 0; x < display::kWidth; ++x) {
            uint16_t color = black;
            const int dx = x - c, dy = y - c;
            const int d2 = dx * dx + dy * dy;
            if (d2 >= 112 * 112 && d2 <= 119 * 119) color = white;
            else if (in_disc(x, y, c, 50, 38)) color = red;
            else if (in_disc(x, y, 190, c, 38)) color = green;
            else if (in_disc(x, y, c, 190, 38)) color = blue;
            else if (in_disc(x, y, c, c, 6)) color = white;
            px[row * display::kWidth + x] = color;
        }
    }
}

// Renders a face, mapping coverage to colour through a 256-entry table so
// each pixel is a single lookup.
struct FaceCtx {
    face::Renderer renderer;
    uint16_t lut[256];
};

void fill_face(uint16_t* px, int y0, int rows, void* ctx) {
    auto* f = static_cast<FaceCtx*>(ctx);
    uint8_t cov[face::kSize];
    for (int row = 0; row < rows; ++row) {
        f->renderer.row(y0 + row, cov);
        uint16_t* out = px + row * display::kWidth;
        for (int x = 0; x < display::kWidth; ++x) out[x] = f->lut[cov[x]];
    }
}

FaceCtx g_face_ctx;

bool draw_face(const face::Face& f, int64_t* us) {
    g_face_ctx.renderer.set(f);
    for (int a = 0; a < 256; ++a) {
        g_face_ctx.lut[a] = display::rgb565(f.color.r * a / 255, f.color.g * a / 255, f.color.b * a / 255);
    }
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = display::lcd_draw(fill_face, &g_face_ctx);
    *us = esp_timer_get_time() - t0;
    return err == ESP_OK;
}

bool g_lcd_ok = false;

// The animator is shared by the command task and the display task.
face::Animator g_anim;
SemaphoreHandle_t g_anim_lock = nullptr;

// Held while deciding whether to draw and drawing, so a test image can't be
// overwritten by a face frame that was already on its way.
SemaphoreHandle_t g_screen_lock = nullptr;
bool g_paused = false;  // test image on screen; guarded by g_screen_lock
std::atomic<bool> g_force_redraw{true};

std::atomic<uint32_t> g_fps{0}, g_drawn_fps{0}, g_frame_us{0};

template <typename Fn>
void with_anim(Fn fn) {
    xSemaphoreTake(g_anim_lock, portMAX_DELAY);
    fn(g_anim);
    xSemaphoreGive(g_anim_lock);
}

// Face commands take the screen back from a test image.
void resume_face() {
    xSemaphoreTake(g_screen_lock, portMAX_DELAY);
    g_paused = false;
    g_force_redraw = true;
    xSemaphoreGive(g_screen_lock);
}

void display_task(void*) {
    TickType_t wake = xTaskGetTickCount();
    int64_t last = esp_timer_get_time();
    int64_t window_start = last;
    uint32_t loops = 0, drawn = 0;
    int64_t draw_sum = 0;
    face::Face shown{};

    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(kFramePeriodMs));
        const int64_t now = esp_timer_get_time();
        const float speech =
            audio::playing() ? std::clamp((audio::level() - kSpeechFloor) * kSpeechGain, 0.0f, 1.0f) : 0.0f;
        face::Face f;
        with_anim([&](face::Animator& a) {
            a.set_speech(speech);
            f = a.update((now - last) / 1000.0f);
        });
        last = now;
        ++loops;

        xSemaphoreTake(g_screen_lock, portMAX_DELAY);
        // Skip the redraw when nothing moved, e.g. between blinks.
        if (!g_paused && (g_force_redraw.exchange(false) || !(f == shown))) {
            int64_t us = 0;
            if (draw_face(f, &us)) {
                shown = f;
                ++drawn;
                draw_sum += us;
            }
        }
        xSemaphoreGive(g_screen_lock);

        const int64_t elapsed = now - window_start;
        if (elapsed >= 1000000) {
            g_fps = static_cast<uint32_t>((loops * 1000000LL + elapsed / 2) / elapsed);
            g_drawn_fps = static_cast<uint32_t>((drawn * 1000000LL + elapsed / 2) / elapsed);
            if (drawn) g_frame_us = static_cast<uint32_t>(draw_sum / drawn);
            loops = drawn = 0;
            draw_sum = 0;
            window_start = now;
        }
    }
}

bool require_lcd() {
    if (g_lcd_ok) return true;
    reply_error("lcd init failed, check the boot log");
    return false;
}

// Draws a test image and reports how long it took, including rendering.
// The face stays paused until a face command.
bool draw_timed(display::BandFn fn, void* ctx, int64_t* us) {
    xSemaphoreTake(g_screen_lock, portMAX_DELAY);
    g_paused = true;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = display::lcd_draw(fn, ctx);
    *us = esp_timer_get_time() - t0;
    xSemaphoreGive(g_screen_lock);
    if (err != ESP_OK) {
        reply_error("lcd draw failed");
        return false;
    }
    return true;
}

// lcdtest [pattern|red|green|blue|white|black]
void cmd_lcdtest(std::string_view args) {
    if (!require_lcd()) return;
    std::string_view what = next_token(args);
    if (what.empty()) what = "pattern";

    struct Named {
        const char* name;
        uint8_t r, g, b;
    };
    static constexpr Named kSolids[] = {
        {"red", 255, 0, 0},     {"green", 0, 255, 0},     {"blue", 0, 0, 255},
        {"white", 255, 255, 255}, {"black", 0, 0, 0},
    };

    int64_t us = 0;
    if (what == "pattern") {
        if (!draw_timed(fill_pattern, nullptr, &us)) return;
    } else {
        const Named* found = nullptr;
        for (const auto& s : kSolids) {
            if (what == s.name) found = &s;
        }
        if (!found) {
            reply_error("usage: lcdtest [pattern|red|green|blue|white|black]");
            return;
        }
        Solid solid{display::rgb565(found->r, found->g, found->b)};
        if (!draw_timed(fill_solid, &solid, &us)) return;
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"lcdtest\",\"image\":\"%.*s\",\"frame_us\":%" PRId64 "}",
                static_cast<int>(what.size()), what.data(), us);
}

void send_flags(const char* type) {
    const display::PanelFlags f = display::lcd_flags();
    comm::sendf("{\"type\":\"%s\",\"cmd\":\"lcdcfg\",\"invert\":%d,\"mirror_x\":%d,\"mirror_y\":%d,"
                "\"swap_rb\":%d}",
                type, f.invert, f.mirror_x, f.mirror_y, f.swap_rb);
}

// lcdcfg                         show the current settings
// lcdcfg <setting> <0|1>         change one, then redraw the test pattern
void cmd_lcdcfg(std::string_view args) {
    if (!require_lcd()) return;
    const std::string_view key = next_token(args);
    if (key.empty()) {
        send_flags("lcdcfg");
        return;
    }
    int value = 0;
    display::PanelFlags f = display::lcd_flags();
    bool* field = key == "invert"     ? &f.invert
                  : key == "mirror_x" ? &f.mirror_x
                  : key == "mirror_y" ? &f.mirror_y
                  : key == "swap_rb"  ? &f.swap_rb
                                      : nullptr;
    if (!field || !parse_int(next_token(args), &value) || (value != 0 && value != 1)) {
        reply_error("usage: lcdcfg [invert|mirror_x|mirror_y|swap_rb <0|1>]");
        return;
    }
    *field = value == 1;
    if (display::lcd_set_flags(f) != ESP_OK) {
        reply_error("lcdcfg failed");
        return;
    }
    int64_t us = 0;
    if (!draw_timed(fill_pattern, nullptr, &us)) return;
    send_flags("ack");
}

// expr <name>   blend to an expression
void cmd_expr(std::string_view args) {
    if (!require_lcd()) return;
    const std::string_view name = next_token(args);
    const face::Face* f = face::find_expression(name.data(), name.size());
    if (!f) {
        char names[160] = "";
        size_t pos = 0;
        for (size_t i = 0; i < face::kNumExpressions && pos < sizeof(names); ++i) {
            pos += std::snprintf(names + pos, sizeof(names) - pos, "%s%s", i ? "|" : "",
                                 face::kExpressions[i].name);
        }
        comm::sendf("{\"type\":\"error\",\"msg\":\"usage: expr <%s>\"}", names);
        return;
    }
    with_anim([&](face::Animator& a) { a.set_expression(*f); });
    resume_face();
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"expr\",\"name\":\"%.*s\"}",
                static_cast<int>(name.size()), name.data());
}

// look <x> <y>   point the eyes, -1..1 each; stops idle eye movement
void cmd_look(std::string_view args) {
    if (!require_lcd()) return;
    float x = 0, y = 0;
    if (!parse_float(next_token(args), &x) || !parse_float(next_token(args), &y) ||
        std::fabs(x) > 1 || std::fabs(y) > 1) {
        reply_error("usage: look <x -1..1> <y -1..1>");
        return;
    }
    with_anim([&](face::Animator& a) { a.look(x, y); });
    resume_face();
    char bx[16], by[16];
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"look\",\"x\":%s,\"y\":%s,\"auto\":false}",
                fmt2(x, bx), fmt2(y, by));
}

// blink   blink once now
void cmd_blink(std::string_view) {
    if (!require_lcd()) return;
    with_anim([](face::Animator& a) { a.blink(); });
    resume_face();
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"blink\"}");
}

// eyes [auto on|off]   show or set idle blinking and eye movement
void cmd_eyes(std::string_view args) {
    if (!require_lcd()) return;
    const std::string_view what = next_token(args);
    const std::string_view value = next_token(args);
    if (!what.empty()) {
        if (what != "auto" || (value != "on" && value != "off")) {
            reply_error("usage: eyes [auto on|off]");
            return;
        }
        with_anim([&](face::Animator& a) { a.set_auto(value == "on"); });
        resume_face();
    }
    bool on = false;
    with_anim([&](face::Animator& a) { on = a.auto_on(); });
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"eyes\",\"auto\":%s}", on ? "true" : "false");
}

// talk [on|off]   flap the mouth as if speaking (stand-in for real speech)
void cmd_talk(std::string_view args) {
    if (!require_lcd()) return;
    const std::string_view value = next_token(args);
    if (!value.empty()) {
        if (value != "on" && value != "off") {
            reply_error("usage: talk [on|off]");
            return;
        }
        with_anim([&](face::Animator& a) { a.set_talking(value == "on"); });
        resume_face();
    }
    bool on = false;
    with_anim([&](face::Animator& a) { on = a.talking(); });
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"talk\",\"on\":%s}", on ? "true" : "false");
}

// bright [0-100]
void cmd_bright(std::string_view args) {
    if (!require_lcd()) return;
    const std::string_view tok = next_token(args);
    int pct = 0;
    if (!tok.empty()) {
        if (!parse_int(tok, &pct) || pct < 0 || pct > 100) {
            reply_error("usage: bright [0-100]");
            return;
        }
        if (display::lcd_set_brightness(pct) != ESP_OK) {
            reply_error("backlight failed");
            return;
        }
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"bright\",\"percent\":%d}", display::lcd_brightness());
}

}  // namespace

void display_init() {
    const esp_err_t err = display::lcd_init();
    if (err != ESP_OK) {
        // Keep running: the rest of the firmware doesn't need the screen.
        ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(err));
        return;
    }
    g_anim_lock = xSemaphoreCreateMutex();
    g_screen_lock = xSemaphoreCreateMutex();
    if (!g_anim_lock || !g_screen_lock) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    g_anim = face::Animator(esp_random());  // different idle behaviour each boot
    g_lcd_ok = true;
    // Draw before turning the backlight on, so power-up garbage isn't shown.
    int64_t us = 0;
    draw_face(face::kExpressions[0].face, &us);
    ESP_LOGI(TAG, "first face frame in %lld us", static_cast<long long>(us));
    display::lcd_set_brightness(CONFIG_AIBOT_LCD_BRIGHTNESS);
}

void display_start(int priority) {
    if (!g_lcd_ok) return;
    xTaskCreate(display_task, "display", 4096, nullptr, priority, nullptr);
}

DisplayStats display_stats() { return {g_fps.load(), g_drawn_fps.load(), g_frame_us.load()}; }

bool display_ready() { return g_lcd_ok; }

void register_display_commands() {
    comm::register_command("lcdtest", cmd_lcdtest);
    comm::register_command("lcdcfg", cmd_lcdcfg);
    comm::register_command("bright", cmd_bright);
    comm::register_command("expr", cmd_expr);
    comm::register_command("look", cmd_look);
    comm::register_command("blink", cmd_blink);
    comm::register_command("eyes", cmd_eyes);
    comm::register_command("talk", cmd_talk);
}
