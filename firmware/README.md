# Firmware

ESP-IDF (C++, FreeRTOS) firmware for the ESP32-S3 DevKitC-1-N8R8. Built with ESP-IDF v6.1 (installed at `~/esp/esp-idf`).

## Current state: M2 LCD in progress

- Starts a command task and a telemetry task.
- Sends telemetry once a second: uptime, free heap, minimum free heap, free internal RAM, free PSRAM, task count, and display fps and frame time.
- Accepts commands from the host over USB serial.
- Drives the GC9A01 round LCD at 80 MHz SPI with an animated face (two eyes and a mouth) at 30 fps: random blinks, idle glances, smooth changes between expressions, and a talking mouth for testing.
- Servo control (M7) was written early and is not yet tested on hardware: drives three servos through a PCA9685 at 50 Hz. Each joint is clamped to its limits before any pulse is sent.
- Servos stay limp at boot until commanded. A `servo` command jumps straight to the target at full speed; smooth moves come in M8.

## Wiring: LCD (M2)

| LCD pin | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| DIN | GPIO 11 |
| CLK | GPIO 12 |
| CS | GPIO 10 |
| DC | GPIO 13 |
| RST | GPIO 14 |
| BL | GPIO 21 |

- **Changing pins, SPI speed, orientation or colour order:** use `idf.py menuconfig` → *aibot display*. The defaults (invert on, mirror X on, red/blue swapped) are right for the bench module.
- **Power:** the module runs bright from the ESP32's 3V3; no separate supply is needed. A dim, faintly glowing or blank screen usually means a poor ground. If you do power the LCD from a separate supply, its ground must also connect to the ESP32's GND, or the panel can't read the data lines.

## Face previews on the PC

The face renderer (`components/display/face.cpp`) has no ESP-IDF dependencies, so it also builds on the host:

```bash
cmake -S components/display/host -B build-host
cmake --build build-host
ctest --test-dir build-host          # unit tests
build-host/face_preview /tmp/faces   # one PNG per expression, plus sheet, sheet_eyes_only, talking, blink, gaze, transition and animation strips
```

## Wiring: servos (M7)

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

Type `ping`, `status`, `i2cscan`, `servo head_yaw 30`, `relax` and so on. It only prints replies to your commands, plus errors and boot messages. Add `--telemetry` to see the periodic telemetry, or `--logs` to see ESP_LOG lines. Close `idf.py monitor` first, because only one program can hold the port at a time.

## Serial protocol (v0)

- **Host → robot:** one plain-text command per line.
- **Robot → host:** one JSON object per line, always with a `"type"` field.
- **Logs:** ESP_LOG output shares the same UART. Any line that doesn't start with `{` is a log.

| Command | Reply |
|---|---|
| `ping` | `{"type":"pong","uptime_ms":…}` |
| `status` | one `telemetry` message immediately. Includes `fps` (animation rate), `drawn_fps` (frames actually redrawn) and `frame_us` (draw time) |
| `rate <ms>` | sets the telemetry period (100–60000 ms), replies `{"type":"ack","cmd":"rate","period_ms":…}` |
| `reboot` | `{"type":"ack","cmd":"reboot"}`, then restarts |
| `i2cscan` | `{"type":"i2cscan","found":[64,112]}`: decimal addresses (a PCA9685 shows as 64 = 0x40, plus 112 = 0x70 for its all-call address) |
| `servo <joint> <deg>` | joint is `body_yaw`, `head_yaw`, `head_pitch` or `0`–`2`. Replies `{"type":"ack","cmd":"servo","joint":…,"deg":…,"pulse_us":…,"clamped":…}` |
| `pulse <ch> <us>` | raw pulse (500–2500 µs) on any of the 16 channels, for calibration. Ignores joint limits |
| `relax [joint]` | stops pulses so the servo goes limp. Relaxes every joint if no joint is given |
| `expr <name>` | blends to an expression: `neutral`, `happy`, `sad`, `angry`, `surprised`, `sleepy`, `listening`, `thinking`. Replies `{"type":"ack","cmd":"expr","name":…}` |
| `look <x> <y>` | points the eyes, -1..1 each axis, and stops idle eye movement. Replies `{"type":"ack","cmd":"look","x":…,"y":…,"auto":false}` |
| `blink` | blinks once now |
| `eyes [auto on\|off]` | shows or sets idle blinking and eye movement. Replies `{"type":"ack","cmd":"eyes","auto":true\|false}` |
| `talk [on\|off]` | shows or sets the test talking mouth (random syllables). Replies `{"type":"ack","cmd":"talk","on":true\|false}` |
| `lcdtest [image]` | test image: `pattern` (default: red top, green right, blue bottom, white ring), or solid `red`, `green`, `blue`, `white`, `black`. Pauses the face until the next face command |
| `lcdcfg [setting 0\|1]` | shows or changes `invert`, `mirror_x`, `mirror_y`, `swap_rb` until reboot, then redraws the pattern |
| `bright [0-100]` | sets or shows backlight brightness (%) |
| anything else | `{"type":"error","msg":"unknown command","cmd":…}` |

The robot sends `{"type":"boot","reset_reason":…,"idf":…,"pca9685":true|false,"lcd":true|false}` on startup and streams `telemetry` messages periodically.
