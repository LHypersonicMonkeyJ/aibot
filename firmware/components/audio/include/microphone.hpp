#pragma once

// INMP441 I2S microphone: 16 kHz, delivered as 16-bit mono in 20 ms blocks.
//
// Runs on the second I2S controller, independent of the speaker. A mic task
// reads every block, removes the DC offset, applies the gain and hands the
// block to a callback while streaming is on. The first 300 ms after boot,
// while the mic starts up, come out as silence. With nothing wired up the data
// pin is pulled down, so blocks are silent rather than noise.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace mic {

inline constexpr int kSampleRate = 16000;
inline constexpr size_t kBlockSamples = kSampleRate / 50;  // 20 ms

// Called from the mic task for each block while streaming. seq counts blocks
// since boot, including ones lost to overruns, so a jump means a gap.
using BlockHandler = void (*)(const int16_t* samples, size_t n, uint32_t seq);

esp_err_t init();
bool ready();

// Starts the mic task. Call after init().
void start(int priority, BlockHandler handler);

void set_streaming(bool on);
bool streaming();

void set_gain_db(int db);  // clamped to 0..40
int gain_db();

float take_peak();     // loudest sample since the last call, 0..1
uint32_t overruns();   // blocks lost because the task fell behind

}  // namespace mic
