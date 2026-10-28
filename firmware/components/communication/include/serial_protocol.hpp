#pragma once

// Line-based host protocol over the console UART.
//
// Host -> robot: one plain-text command per line, e.g. "ping" or "rate 500".
// Robot -> host: one JSON object per line, always with a "type" field.
// ESP_LOG output shares the same UART; hosts should treat any line that
// does not start with '{' as a log message.

#include <string_view>

namespace comm {

using CommandHandler = void (*)(std::string_view args);

// Installs the UART driver and switches the port to 921600 baud. Call once
// before anything else in this module.
void serial_init();

// Registers a command by name. Call before serial_start(). Returns false if
// the table is full.
bool register_command(const char* name, CommandHandler handler);

// Starts the task that reads lines and dispatches registered commands.
void serial_start(int priority);

// Sends one protocol line (a JSON object, without the trailing newline).
// Safe to call from any task.
void sendf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace comm
