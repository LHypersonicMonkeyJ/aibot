#pragma once

// Speaker output through a MAX98357A I2S amplifier: 16 kHz, 16-bit mono.
//
// Samples go into a 2-second ring buffer in PSRAM; a playback task feeds
// I2S from it 20 ms at a time. When the buffer runs dry the speaker gets
// silence, never stale data. One writer task and the playback task may use
// it at the same time.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace audio {

inline constexpr int kSampleRate = 16000;
// Power of two, so the running read/write counters index it correctly
// even after they wrap.
inline constexpr size_t kBufferSamples = 32768;  // about 2 s

esp_err_t init();
bool ready();

// Starts the playback task. Call after init().
void start(int priority);

// Queues samples. Returns how many fit; the rest are dropped.
size_t write(const int16_t* samples, size_t n);

// Marks the end of a stream, so the buffer emptying afterwards is not
// counted as a dropout. Writing again starts a new stream.
void end_stream();

// Drops everything queued.
void clear();

// Queues a sine tone with short fades, then ends the stream.
size_t tone(float hz, uint32_t ms);

size_t buffered();          // samples waiting to play
bool playing();             // sound is coming out now
float level();              // loudness of the last 20 ms, 0..1 (for lip sync)
uint32_t dropouts();        // times the buffer ran dry mid-stream

void set_volume(int percent);
int volume();

}  // namespace audio
