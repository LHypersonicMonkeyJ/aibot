#pragma once

#include <cstdint>

// Brings up the LCD and shows the neutral face. Safe with nothing wired up.
void display_init();
bool display_ready();
void register_display_commands();

// Starts the ~30 fps task that animates the face. Call after display_init().
void display_start(int priority);

struct DisplayStats {
    uint32_t fps;        // display task loops per second (target 30)
    uint32_t drawn_fps;  // frames actually redrawn; lower while the face is still
    uint32_t frame_us;   // average time to draw one frame
};
DisplayStats display_stats();
