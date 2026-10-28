// M1 bring-up: start FreeRTOS tasks, stream telemetry over USB serial, and
// accept simple commands from the host.

#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <string>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio.hpp"
#include "audio_commands.hpp"
#include "display_commands.hpp"
#include "serial_protocol.hpp"
#include "servo_commands.hpp"

namespace {

constexpr const char* TAG = "main";
constexpr uint32_t kMinPeriodMs = 100;
constexpr uint32_t kMaxPeriodMs = 60000;

// Task priorities, highest first. Motion will sit above these.
constexpr int kAudioPriority = 8;
constexpr int kCommPriority = 5;
constexpr int kTelemetryPriority = 2;
constexpr int kDisplayPriority = 1;

std::atomic<uint32_t> g_telemetry_period_ms{1000};

int64_t uptime_ms() { return esp_timer_get_time() / 1000; }

void send_telemetry() {
    const DisplayStats d = display_stats();
    comm::sendf(
        "{\"type\":\"telemetry\",\"uptime_ms\":%" PRId64 ",\"free_heap\":%" PRIu32
        ",\"min_free_heap\":%" PRIu32 ",\"free_internal\":%zu,\"free_psram\":%zu,\"tasks\":%u,\"fps\":%" PRIu32 ",\"drawn_fps\":%" PRIu32 ",\"frame_us\":%" PRIu32 ",\"audio_ms\":%u,\"dropouts\":%" PRIu32 "}",
        uptime_ms(), esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        static_cast<unsigned>(uxTaskGetNumberOfTasks()), d.fps, d.drawn_fps, d.frame_us,
        static_cast<unsigned>(audio::buffered() * 1000 / audio::kSampleRate), audio::dropouts());
}

void telemetry_task(void*) {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        send_telemetry();
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(g_telemetry_period_ms.load()));
    }
}

// --- Commands ---

void cmd_ping(std::string_view) {
    comm::sendf("{\"type\":\"pong\",\"uptime_ms\":%" PRId64 "}", uptime_ms());
}

void cmd_status(std::string_view) { send_telemetry(); }

void cmd_rate(std::string_view args) {
    const std::string s(args);
    char* end = nullptr;
    const unsigned long ms = std::strtoul(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || ms < kMinPeriodMs || ms > kMaxPeriodMs) {
        comm::sendf("{\"type\":\"error\",\"msg\":\"rate expects %" PRIu32 "-%" PRIu32 " ms\"}",
                    kMinPeriodMs, kMaxPeriodMs);
        return;
    }
    g_telemetry_period_ms.store(static_cast<uint32_t>(ms));
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"rate\",\"period_ms\":%lu}", ms);
}

void cmd_reboot(std::string_view) {
    comm::sendf("{\"type\":\"ack\",\"cmd\":\"reboot\"}");
    vTaskDelay(pdMS_TO_TICKS(100));  // let the ack drain
    esp_restart();
}

}  // namespace

extern "C" void app_main() {
    comm::serial_init();

    comm::register_command("ping", cmd_ping);
    comm::register_command("status", cmd_status);
    comm::register_command("rate", cmd_rate);
    comm::register_command("reboot", cmd_reboot);
    register_servo_commands();
    register_display_commands();
    register_audio_commands();

    servo_init();
    display_init();
    audio_init();

    comm::sendf("{\"type\":\"boot\",\"reset_reason\":%d,\"idf\":\"%s\",\"pca9685\":%s,\"lcd\":%s,\"audio\":%s}",
                static_cast<int>(esp_reset_reason()), esp_get_idf_version(),
                servo_ready() ? "true" : "false", display_ready() ? "true" : "false",
                audio_ready() ? "true" : "false");

    comm::serial_start(kCommPriority);
    audio_start(kAudioPriority);
    display_start(kDisplayPriority);
    xTaskCreate(telemetry_task, "telemetry", 4096, nullptr, kTelemetryPriority, nullptr);

    ESP_LOGI(TAG, "M1 bring-up running");
}
