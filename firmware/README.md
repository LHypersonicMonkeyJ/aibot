# Firmware

ESP-IDF (C++, FreeRTOS) firmware for the ESP32-S3 DevKitC-1-N8R8. Built with ESP-IDF v6.1 (installed at `~/esp/esp-idf`).

## Current state: M7 servo control (written early, not yet tested on hardware)

- Starts a command task and a telemetry task.
- Sends telemetry once a second: uptime, free heap, minimum free heap, free internal RAM, free PSRAM, task count.
- Accepts commands from the host over USB serial.
- Drives three servos through a PCA9685 at 50 Hz. Each joint is clamped to its limits before any pulse is sent.
- Servos stay limp at boot until commanded. A `servo` command jumps straight to the target at full speed; smooth moves come in M8.

## Wiring (M7)

| ESP32-S3 | PCA9685 |
|---|---|
| 3V3 | VCC (logic) |
| GND | GND |
| GPIO 8 | SDA |
| GPIO 9 | SCL |

- **Servo power:** connect the 5 V 5 A supply to the PCA9685's V+ screw terminal. Don't power the servos from the ESP32's 5V pin.
- **Ground:** the supply ground and the ESP32 ground must be connected.
- **Channels:** body yaw on 0, head yaw on 1, head pitch on 2.
- **Changing pins or address:** use `idf.py menuconfig` → *aibot motion*.

## Build and flash

Plug into the board's **COM** port (not the one marked USB). Its CH343 serial chip shows up as `/dev/ttyACM0`.

```bash
. $HOME/esp/esp-idf/export.sh       # wherever ESP-IDF is installed
cd firmware
idf.py set-target esp32s3           # first time only
idf.py -p /dev/ttyACM0 flash monitor   # Ctrl-] exits the monitor
```

## Talk to it from Python

```bash
pip install -r tools/requirements.txt
python3 tools/robot_cli.py /dev/ttyACM0
```

Type `ping`, `status`, `i2cscan`, `servo head_yaw 30`, `relax` and so on. Close `idf.py monitor` first, because only one program can hold the port at a time.

## Serial protocol (v0)

- **Host → robot:** one plain-text command per line.
- **Robot → host:** one JSON object per line, always with a `"type"` field.
- **Logs:** ESP_LOG output shares the same UART. Any line that doesn't start with `{` is a log.

| Command | Reply |
|---|---|
| `ping` | `{"type":"pong","uptime_ms":…}` |
| `status` | one `telemetry` message immediately |
| `rate <ms>` | sets the telemetry period (100–60000 ms), replies `{"type":"ack","cmd":"rate","period_ms":…}` |
| `reboot` | `{"type":"ack","cmd":"reboot"}`, then restarts |
| `i2cscan` | `{"type":"i2cscan","found":[64,112]}`: decimal addresses (a PCA9685 shows as 64 = 0x40, plus 112 = 0x70 for its all-call address) |
| `servo <joint> <deg>` | joint is `body_yaw`, `head_yaw`, `head_pitch` or `0`–`2`. Replies `{"type":"ack","cmd":"servo","joint":…,"deg":…,"pulse_us":…,"clamped":…}` |
| `pulse <ch> <us>` | raw pulse (500–2500 µs) on any of the 16 channels, for calibration. Ignores joint limits |
| `relax [joint]` | stops pulses so the servo goes limp. Relaxes every joint if no joint is given |
| anything else | `{"type":"error","msg":"unknown command","cmd":…}` |

The robot sends `{"type":"boot","reset_reason":…,"idf":…,"pca9685":true|false}` on startup and streams `telemetry` messages periodically.
