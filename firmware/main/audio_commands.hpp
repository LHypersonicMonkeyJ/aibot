#pragma once

// Sets up the I2S amplifier and the microphone. Safe with nothing wired up
// (neither can be detected; a missing mic just records silence).
void audio_init();
bool audio_ready();
bool mic_ready();
void register_audio_commands();

// Starts the playback and mic tasks. Call after audio_init().
void audio_start(int priority);
