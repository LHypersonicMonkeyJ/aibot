#pragma once

// GC9A01 240x240 round LCD over SPI, plus PWM backlight.
//
// Frames are drawn in horizontal bands into two small DMA buffers, so there is
// no full framebuffer: one band is sent while the next is rendered.

#include <cstdint>

#include "esp_err.h"

namespace display {

inline constexpr int kWidth = 240;
inline constexpr int kHeight = 240;
inline constexpr int kBandRows = 24;  // 10 bands per frame

// Fills `rows` full-width rows starting at screen row y0. px holds
// kWidth * rows pixels, already in panel byte order (use rgb565()).
using BandFn = void (*)(uint16_t* px, int y0, int rows, void* ctx);

struct PanelFlags {
    bool invert;
    bool mirror_x;
    bool mirror_y;
    bool swap_rb;
};

esp_err_t lcd_init();
bool lcd_ready();

// Renders and sends one full frame. Blocks until the last band is sent.
// Safe to call from any task; frames are serialized.
esp_err_t lcd_draw(BandFn fn, void* ctx);

esp_err_t lcd_set_brightness(int percent);
int lcd_brightness();

// Orientation and color fixes for bring-up. Redraw afterwards.
esp_err_t lcd_set_flags(const PanelFlags& flags);
PanelFlags lcd_flags();

// Packs a color for the panel: RGB565, byte-swapped for SPI, and red/blue
// swapped if the panel needs it.
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

}  // namespace display
