// M2 serial commands: LCD test patterns, orientation/color fixes, backlight.

#include "display_commands.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "face.hpp"
#include "lcd.hpp"
#include "sdkconfig.h"
#include "serial_protocol.hpp"

namespace {

constexpr const char* TAG = "display_cmd";

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

bool require_lcd() {
    if (g_lcd_ok) return true;
    reply_error("lcd init failed, check the boot log");
    return false;
}

// Draws a frame and reports how long it took, including rendering.
bool draw_timed(display::BandFn fn, void* ctx, int64_t* us) {
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = display::lcd_draw(fn, ctx);
    *us = esp_timer_get_time() - t0;
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

// expr <name>   draw one expression (static until the display task lands)
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
    int64_t us = 0;
    if (!draw_face(*f, &us)) {
        reply_error("lcd draw failed");
        return;
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"expr\",\"name\":\"%.*s\",\"frame_us\":%" PRId64 "}",
                static_cast<int>(name.size()), name.data(), us);
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
    g_lcd_ok = true;
    // Draw before turning the backlight on, so power-up garbage isn't shown.
    int64_t us = 0;
    draw_face(face::kExpressions[0].face, &us);
    ESP_LOGI(TAG, "first face frame in %lld us", static_cast<long long>(us));
    display::lcd_set_brightness(CONFIG_AIBOT_LCD_BRIGHTNESS);
}

bool display_ready() { return g_lcd_ok; }

void register_display_commands() {
    comm::register_command("lcdtest", cmd_lcdtest);
    comm::register_command("lcdcfg", cmd_lcdcfg);
    comm::register_command("bright", cmd_bright);
    comm::register_command("expr", cmd_expr);
}
