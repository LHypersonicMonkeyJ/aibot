#include "serial_protocol.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace comm {
namespace {

constexpr const char* TAG = "serial";
constexpr uart_port_t kPort = UART_NUM_0;
constexpr size_t kMaxLine = 128;
constexpr size_t kMaxCommands = 16;

struct Command {
    const char* name;
    CommandHandler handler;
};

Command g_commands[kMaxCommands];
size_t g_num_commands = 0;
SemaphoreHandle_t g_tx_mutex = nullptr;

void dispatch(std::string_view line) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
    if (line.empty()) return;

    const size_t space = line.find(' ');
    const std::string_view name = line.substr(0, space);
    std::string_view args = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);

    for (size_t i = 0; i < g_num_commands; ++i) {
        if (name == g_commands[i].name) {
            g_commands[i].handler(args);
            return;
        }
    }

    // Echo the name back, keeping only characters that are safe inside a JSON string.
    char safe[32];
    size_t n = 0;
    for (char c : name) {
        if (n + 1 >= sizeof(safe)) break;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') safe[n++] = c;
    }
    safe[n] = '\0';
    sendf("{\"type\":\"error\",\"msg\":\"unknown command\",\"cmd\":\"%s\"}", safe);
}

void rx_task(void*) {
    char line[kMaxLine];
    size_t len = 0;
    bool overflow = false;
    uint8_t buf[32];

    for (;;) {
        const int n = uart_read_bytes(kPort, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; ++i) {
            const char c = static_cast<char>(buf[i]);
            if (c == '\n' || c == '\r') {
                if (overflow) {
                    sendf("{\"type\":\"error\",\"msg\":\"line too long\"}");
                } else if (len > 0) {
                    dispatch(std::string_view(line, len));
                }
                len = 0;
                overflow = false;
            } else if (len < kMaxLine) {
                line[len++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

}  // namespace

void serial_init() {
    g_tx_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(uart_driver_install(kPort, 1024, 0, 0, nullptr, 0));
    // Route stdout through the driver too, so logs and protocol lines don't
    // fight over the UART FIFO.
    uart_vfs_dev_use_driver(kPort);
}

bool register_command(const char* name, CommandHandler handler) {
    if (g_num_commands >= kMaxCommands) {
        ESP_LOGE(TAG, "command table full, dropping '%s'", name);
        return false;
    }
    g_commands[g_num_commands++] = {name, handler};
    return true;
}

void serial_start(int priority) {
    xTaskCreate(rx_task, "comm_rx", 4096, nullptr, priority, nullptr);
}

void sendf(const char* fmt, ...) {
    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    xSemaphoreGive(g_tx_mutex);
}

}  // namespace comm
