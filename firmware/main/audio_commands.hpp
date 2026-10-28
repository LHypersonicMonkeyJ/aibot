#pragma once

// Sets up the I2S amplifier. Safe with nothing wired up (the amp is
// write-only, so this can't tell whether it is connected).
void audio_init();
bool audio_ready();
void register_audio_commands();

// Starts the playback task. Call after audio_init().
void audio_start(int priority);
