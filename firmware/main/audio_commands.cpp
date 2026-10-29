// Audio serial commands: test tones, volume and streaming speech from the PC
// (M3), and streaming the microphone to the PC (M4).

#include "audio_commands.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

#include "audio.hpp"
#include "microphone.hpp"
#include "esp_log.h"
#include "serial_protocol.hpp"

namespace {

constexpr const char* TAG = "audio_cmd";
bool g_audio_ok = false;
bool g_mic_ok = false;

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

bool require_audio() {
    if (g_audio_ok) return true;
    reply_error("audio init failed, check the boot log");
    return false;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes standard base64 into out. Returns the byte count, or -1 if invalid.
int b64_decode(std::string_view in, uint8_t* out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    return static_cast<int>(n);
}

// beep [hz] [ms]
void cmd_beep(std::string_view args) {
    if (!require_audio()) return;
    int hz = 880, ms = 300;
    const std::string_view hz_tok = next_token(args), ms_tok = next_token(args);
    if ((!hz_tok.empty() && !parse_int(hz_tok, &hz)) || (!ms_tok.empty() && !parse_int(ms_tok, &ms)) ||
        hz < 50 || hz > 8000 || ms < 10 || ms > 2000) {
        reply_error("usage: beep [hz 50-8000] [ms 10-2000]");
        return;
    }
    audio::tone(static_cast<float>(hz), static_cast<uint32_t>(ms));
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"beep\",\"hz\":%d,\"ms\":%d}", hz, ms);
}

// vol [0-100]
void cmd_vol(std::string_view args) {
    if (!require_audio()) return;
    const std::string_view tok = next_token(args);
    if (!tok.empty()) {
        int pct = 0;
        if (!parse_int(tok, &pct) || pct < 0 || pct > 100) {
            reply_error("usage: vol [0-100]");
            return;
        }
        audio::set_volume(pct);
    }
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"vol\",\"percent\":%d}", audio::volume());
}

// pcm <base64>   queue 16 kHz 16-bit little-endian mono samples
// pcm end        the stream is complete; let the buffer drain
// pcm stop       drop everything queued
// Every reply reports the buffer, so the sender can pace itself.
void cmd_pcm(std::string_view args) {
    if (!require_audio()) return;
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    uint32_t dropped = 0;
    if (args == "end") {
        audio::end_stream();
    } else if (args == "stop") {
        audio::clear();
    } else {
        static int16_t samples[380];  // a 1024-char line holds at most ~380 samples
        const int bytes = b64_decode(args, reinterpret_cast<uint8_t*>(samples), sizeof(samples));
        if (bytes < 0 || bytes % 2 != 0) {
            reply_error("usage: pcm <base64 of 16-bit samples, max 380>|end|stop");
            return;
        }
        const size_t n = static_cast<size_t>(bytes) / 2;
        dropped = static_cast<uint32_t>(n - audio::write(samples, n));
    }
    comm::sendf("{\"type\":\"pcm\",\"buffered\":%u,\"capacity\":%u,\"dropped\":%lu}",
                static_cast<unsigned>(audio::buffered()), static_cast<unsigned>(audio::kBufferSamples),
                static_cast<unsigned long>(dropped));
}

// --- Microphone (M4) ---

void b64_encode(const uint8_t* in, size_t n, char* out) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t b = (in[i] << 16) | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = kAlphabet[(b >> 18) & 63];
        out[o++] = kAlphabet[(b >> 12) & 63];
        out[o++] = i + 1 < n ? kAlphabet[(b >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? kAlphabet[b & 63] : '=';
    }
    out[o] = '\0';
}

// Runs in the mic task: one line per 20 ms block while streaming.
void send_mic_block(const int16_t* samples, size_t n, uint32_t seq) {
    static char text[(mic::kBlockSamples * 2 + 2) / 3 * 4 + 1];
    b64_encode(reinterpret_cast<const uint8_t*>(samples), n * 2, text);
    comm::sendf("{\"type\":\"mic\",\"seq\":%lu,\"data\":\"%s\"}", static_cast<unsigned long>(seq), text);
}

void reply_mic() {
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"mic\",\"on\":%s,\"gain_db\":%d}",
                mic::streaming() ? "true" : "false", mic::gain_db());
}

// mic [on|off]   start or stop streaming
// mic gain <dB>  0-40
void cmd_mic(std::string_view args) {
    if (!g_mic_ok) {
        reply_error("mic init failed, check the boot log");
        return;
    }
    const std::string_view tok = next_token(args);
    if (tok == "on" || tok == "off") {
        mic::set_streaming(tok == "on");
    } else if (tok == "gain") {
        int db = 0;
        if (!parse_int(next_token(args), &db) || db < 0 || db > 40) {
            reply_error("usage: mic gain <0-40>");
            return;
        }
        mic::set_gain_db(db);
    } else if (!tok.empty()) {
        reply_error("usage: mic [on|off|gain <0-40>]");
        return;
    }
    reply_mic();
}

}  // namespace

void audio_init() {
    esp_err_t err = audio::init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
    } else {
        g_audio_ok = true;
    }
    err = mic::init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed: %s", esp_err_to_name(err));
    } else {
        g_mic_ok = true;
    }
}

bool audio_ready() { return g_audio_ok; }
bool mic_ready() { return g_mic_ok; }

void audio_start(int priority) {
    if (g_audio_ok) audio::start(priority);
    if (g_mic_ok) mic::start(priority, send_mic_block);
}

void register_audio_commands() {
    comm::register_command("beep", cmd_beep);
    comm::register_command("vol", cmd_vol);
    comm::register_command("pcm", cmd_pcm);
    comm::register_command("mic", cmd_mic);
}
