# ESP32-S3 AI Desktop Robot

## Phase 1 — Bench Development and Software Architecture

### 1. Development philosophy

The project is divided into three major stages:

| Stage | Objective | Deliverable |
|---|---|---|
| 1. Electronics bench | Validate individual components | Working ESP32, servos, LCD, microphone, and speaker |
| 2. Software integration | Develop firmware, backend, and autonomy | Fully functioning robot software on the bench |
| 3. Mechanical integration | Design and print the robot | Complete desktop companion |

Our immediate focus is **Stages 1 and 2**.

The goal is a robot that can listen, speak, display expressions, and generate autonomous motion commands before it has a physical body.

### 2. Bench hardware architecture

We use the same electronics intended for the final robot.

```
                         INTERNET
                            │
                     ┌──────▼──────┐
                     │  OpenAI API │
                     │             │
                     │ LLM / Voice │
                     └──────┬──────┘
                            │
                     ┌──────▼──────┐
                     │ Python      │
                     │ Backend     │
                     │             │
                     │ AI Interface│
                     │ Behavior    │
                     │ Memory      │
                     │ Telemetry   │
                     └──────┬──────┘
                            │
                       Wi-Fi / USB
                            │
              ┌─────────────▼─────────────┐
              │         ESP32-S3          │
              │                           │
              │ FreeRTOS                  │
              │ State Machine             │
              │ Motion Controller         │
              │ Audio Driver              │
              │ Display Driver            │
              │ Communication Interface   │
              └─────────────┬─────────────┘
                            │
          ┌─────────────────┼─────────────────┐
          │                 │                 │
     ┌────▼────┐       ┌────▼────┐      ┌─────▼─────┐
     │ PCA9685 │       │ GC9A01  │      │ I2S Audio │
     │         │       │ LCD     │      │           │
     │ 3 Servos│       │ Eyes    │      │ Mic/Speak │
     └─────────┘       └─────────┘      └───────────┘
```

Communication starts over **USB serial**, then migrates to **Wi-Fi/WebSockets**. USB is easier to debug and removes network connectivity as a variable during early development.

### 3. Electronics bench setup

| Component | Quantity | Purpose |
|---|---|---|
| ESP32-S3 DevKitC-1-N8R8 | 1 | Main embedded computer |
| PCA9685 servo controller | 1 | Servo PWM generation |
| MG90S micro servo | 3 | Motion actuators |
| GC9A01 round LCD | 1 | Animated face |
| INMP441 microphone | 1 | Voice input |
| MAX98357A amplifier | 1 | Audio output |
| 4Ω 3W speaker | 1 | Voice playback |
| 5V 5A regulated power supply | 1 | Servo and peripheral power |
| Breadboard | 1 | Low-current signal prototyping |
| Jumper wires | Assorted | Connections |
| Multimeter | 1 | Electrical verification |

> **Important:** Do not distribute servo motor power through a solderless breadboard. Use suitable terminal blocks or a power distribution board, with a fuse and common ground.

Attach the servos to a small temporary wooden or printed test fixture rather than letting them move loose on the bench.

### 4. Firmware architecture

Use **ESP-IDF with C++ and FreeRTOS**, rather than Arduino. ESP-IDF gives greater control over task scheduling, memory allocation, peripheral drivers, and diagnostics.

Repository structure:

```
aibot/
│
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── tools/              # host-side bring-up scripts (serial console)
│   │
│   ├── main/
│   │   ├── main.cpp
│   │   └── CMakeLists.txt
│   │
│   └── components/
│       │
│       ├── robot_core/
│       │   ├── robot_state.hpp
│       │   ├── robot_state.cpp
│       │   ├── behavior_manager.cpp
│       │   └── behavior_manager.hpp
│       │
│       ├── motion/
│       │   ├── servo_driver.cpp
│       │   ├── servo_driver.hpp
│       │   ├── trajectory.cpp
│       │   └── trajectory.hpp
│       │
│       ├── display/
│       │   ├── lcd_driver.cpp
│       │   ├── eye_animation.cpp
│       │   └── expressions.cpp
│       │
│       ├── audio/
│       │   ├── microphone.cpp
│       │   ├── speaker.cpp
│       │   └── audio_manager.cpp
│       │
│       └── communication/
│           ├── serial_protocol.cpp
│           ├── wifi_manager.cpp
│           └── websocket_client.cpp
│
├── backend/
│   ├── main.py
│   ├── ai/
│   │   ├── openai_client.py
│   │   ├── conversation.py
│   │   └── personality.py
│   │
│   ├── behavior/
│   │   ├── behavior_planner.py
│   │   └── motion_commands.py
│   │
│   └── communication/
│       ├── serial_interface.py
│       └── websocket_server.py
│
├── sim/                    # software-only actuator simulator (section 7)
│   ├── robot_sim/
│   └── tests/
│
├── tests/
│   ├── test_motion.py
│   ├── test_communication.py
│   └── test_behavior.py
│
└── docs/
    ├── architecture.md
    ├── hardware.md
    └── communication_protocol.md
```

This keeps the embedded control software separate from the AI backend.

### 5. FreeRTOS task architecture

Five initial firmware tasks:

| Task | Target rate | Priority |
|---|---|---|
| Motion controller | 50 Hz | High |
| Communication | Event-driven | Medium |
| Audio processing | Buffer-driven | High |
| Display animation | 30 Hz | Low |
| Behavior manager | 10 Hz | Medium |

These are initial scheduling targets, not guaranteed real-time performance figures.

- The motion controller must never wait for an LLM response or a network transaction.
- The communication task receives high-level commands and updates a thread-safe command buffer.
- The motion task reads those commands and independently generates the desired actuator positions.

Example command flow:

```
Python Backend
     │
     ▼
MOVE_HEAD(30°, -10°)
     │
     ▼
Communication Task
     │
     ▼
Command Queue
     │
     ▼
Motion Controller
     │
     ▼
Trajectory Generator
     │
     ▼
PCA9685
     │
     ▼
Servos
```

This is the same fundamental separation used in larger robotic control systems.

### 6. Bench development milestones

| Milestone | Objective | Verification | Status |
|---|---|---|---|
| M1 | ESP32 development environment | Flash firmware and read serial logs | ✅ Done. ESP-IDF v6.1 installed; firmware boots, streams telemetry and answers commands on the board |
| M2 | LCD interface | Animated eyes and expressions | ✅ Done. Animated face (eyes and mouth) at 30 fps: blinks, idle glances, eased expression changes, test talking mouth; `expr`/`look`/`blink`/`eyes`/`talk` commands. 80 MHz SPI, 15 ms/frame |
| M3 | Audio output | Play synthesized speech | 🟡 Firmware and `say.py` done and tested without a speaker (speech streams in real time, 0 dropouts). Waiting on MAX98357A + speaker wiring to hear it |
| M4 | Audio input | Record microphone audio | ⬜ Not started |
| M5 | Python communication | Send commands and receive telemetry | 🟡 Serial protocol v0 and `firmware/tools/robot_cli.py` working; backend not started |
| M6 | Initial AI integration | Hold a conversation with coordinated expressions | ⬜ Not started |
| M7 | Servo control | Independently command three servos | 🟡 Firmware written and flashed; waiting on PCA9685 wiring to test with real servos |
| M8 | Motion trajectories | Smooth, bounded servo movements | 🟡 Trajectory planner prototyped and tested in `sim/`; not yet ported to firmware |
| M9 | Final AI integration | Hold a conversation with coordinated expressions and motion | ⬜ Not started |

*Status as of 2026-10-27. Servo work (M7, M8) now comes after initial AI integration (M6), and M9 adds motion to the conversation; the servo firmware already written stays in the tree.*

**Next steps**

1. Finish M3: wire the MAX98357A and speaker (wiring below), then run the M3 checks.
2. Decide on 240 MHz CPU, and on 16 MB flash (the chip reports 16 MB).
3. For M7 later: wire the PCA9685 and servos (see `firmware/README.md`), check `i2cscan` finds the board, then command each servo.

#### M2 — LCD interface

Two flat, glowing eyes on the 240×240 GC9A01 round display: solid rounded shapes in one accent color on black, in the style of Vector or EMO. Expression comes from eye shape alone.

**Wiring** (SPI; all pins configurable in menuconfig):

| LCD pin | ESP32-S3 | Purpose |
|---|---|---|
| VCC | 3V3 | Power |
| GND | GND | Ground |
| DIN | GPIO 11 | SPI data (MOSI) |
| CLK | GPIO 12 | SPI clock |
| CS | GPIO 10 | Chip select |
| DC | GPIO 13 | Data/command select |
| RST | GPIO 14 | Reset |
| BL | GPIO 21 | Backlight (PWM dimming) |

GPIO 10–12 are the native SPI pins, so the bus can run at 80 MHz. These pins avoid I2C (8, 9), octal PSRAM (35–37), USB (19, 20) and strapping pins.

**Design**

- Draw the eyes procedurally rather than with LVGL. The eyes are a few shapes, and a GUI toolkit adds weight without helping.
- The drawing code is plain C++ with no ESP-IDF dependencies. It runs on the LCD and on a PC, where it produces PNG/GIF previews and unit tests, the same pattern as `sim/` for motion.
- Each eye is described by a small parameter set: openness, upper and lower lid angle, roundness, size, gaze offset and color. Expressions are named parameter sets, and transitions interpolate between them.
- Frames are drawn in horizontal bands into two small DMA buffers in internal RAM. One band is sent while the next is drawn, so there's no 115 KB full framebuffer. A full frame is about 12 ms of SPI time at 80 MHz, well inside the 33 ms budget at 30 fps.

**Steps**

1. **Bring-up:** `esp_lcd` with the `espressif/esp_lcd_gc9a01` component. Add an `lcdtest` command (solid colors, then a test pattern) to check color order, orientation and inversion.
2. **Eye drawing:** a new `display` component, plus a host preview tool that renders every expression to PNG for tuning without flashing.
3. **Display task:** 30 Hz, low priority. Frame time and fps go into telemetry. Backlight brightness by LEDC PWM.
4. **Animation:**
   - Randomized blinks.
   - Idle gaze: small random eye movements with pauses.
   - Eased 150–250 ms transitions between expressions.
   - Expressions: neutral, happy, sad, surprised, angry, sleepy, plus listening and thinking for M6.
5. **Commands and docs:**
   - Serial commands: `expr <name>`, `look <x> <y>`, `blink`, `eyes auto on|off`, `bright <0-100>`.
   - Add them to the protocol table and `robot_cli.py`, and add LCD wiring to `firmware/README.md`.

**Done when:**

- Every expression shows on the LCD.
- Animation holds a steady 30 fps and blinks look natural.
- Python can switch expressions and gaze over serial.
- Telemetry reports display frame time.

#### M3 — Audio output

Speech is made on the PC with [Piper](https://github.com/rhasspy/piper) (offline neural TTS, voice `en_US-lessac-medium`), resampled to 16 kHz and streamed to the robot over the USB serial link, which now runs at 921600 baud. The robot plays it through a MAX98357A I2S amplifier and moves its mouth with the loudness.

**Wiring** (I2S; pins configurable in menuconfig → *aibot audio*):

| MAX98357A pin | Connect to | Notes |
|---|---|---|
| VIN | ESP32 5V | 5 V gives more volume than 3.3 V |
| GND | ESP32 GND | |
| BCLK | GPIO 4 | Bit clock |
| LRC | GPIO 5 | Word select (left/right clock) |
| DIN | GPIO 6 | Audio data |
| GAIN | leave unconnected | 9 dB gain |
| SD | leave unconnected | Amp on, plays (left + right) / 2 |
| Speaker + / − | Amp's + / − speaker terminals | 4 Ω 3 W speaker. Never connect either speaker wire to GND |

GPIO 4–6 avoid I2C (8, 9), the LCD (10–14, 21), USB (19, 20), PSRAM (35–37) and strapping pins. The speaker can draw several hundred mA at full volume; if the board browns out or resets on loud sounds, lower `vol` or power the amp from a separate 5 V supply that shares GND with the ESP32.

**Design**

- `audio` component: I2S at 16 kHz, 16-bit mono, sent to both slots. A 2-second ring buffer in PSRAM feeds a high-priority playback task 20 ms at a time. New streams wait for 100 ms of audio before starting; when the buffer runs dry the speaker gets silence, and the dropout is counted.
- Serial link: 921600 baud (switched at startup; boot messages stay at 115200). Lines can be up to 1024 characters.
- Audio travels as `pcm <base64>` lines of 320 samples (20 ms). Each reply reports the robot's buffer, and `say.py` keeps about 0.75 s queued. This uses about half the link.
- Lip sync: the display task maps the playback loudness of each 20 ms block to mouth openness; the animator opens the mouth in about 25 ms and closes it in about 70 ms.

**Progress**

- [x] `audio` component, playback task, `beep`, `vol`, `pcm` commands
- [x] Serial link at 921600 baud with 1024-character lines
- [x] `tools/say.py`: Piper TTS → 16 kHz → streamed with flow control
- [x] Lip sync from playback loudness
- [x] Tested without a speaker: 3 s and 6 s sentences streamed in real time with 0 dropouts; all commands answer correctly
- [ ] Wire the MAX98357A and speaker
- [ ] `beep` is clearly audible; set a comfortable default `vol`
- [ ] `say.py` speech sounds clean, with no gaps or clicks
- [ ] Mouth moves in time with the voice (tune `kSpeechFloor` / `kSpeechGain` in `display_commands.cpp` if needed)

**Checks to run after wiring**

```bash
cd firmware
python3 tools/robot_cli.py /dev/ttyACM0     # then: beep, beep 440 1000, vol 30, beep
python3 tools/say.py /dev/ttyACM0 "Hello, I'm aibot."
```

After M9, we implement autonomous behaviors.

#### M10 — Autonomous behavior framework

The robot should be able to perform actions without explicit user commands.

```
IDLE
  │
  ├── Blink every few seconds
  │
  ├── Occasionally look left/right
  │
  └── Return to neutral
         │
         ▼
LISTENING
  │
  ├── Stop idle motion
  ├── Display listening expression
  └── Capture audio
         │
         ▼
THINKING
  │
  ├── Tilt head slightly
  └── Wait for AI response
         │
         ▼
SPEAKING
  │
  ├── Play generated speech
  ├── Animate expression
  └── Perform gestures
         │
         ▼
IDLE
```

All of this can be tested without a physical robot body.

### 7. Bench software testing strategy

Since software is developed before the mechanics, we add a software-only actuator simulator. The motion controller supports two output modes:

**Hardware mode**

```
Motion Controller
       │
       ▼
PCA9685
       │
       ▼
Physical Servos
```

**Simulation mode**

```
Motion Controller
       │
       ▼
Simulated Servos
       │
       ▼
Python Visualization
```

Commanded joint angles and trajectories are visualized in Python. The joint vector:

$$
q =
\begin{bmatrix}
\theta_{\text{body yaw}}\\
\theta_{\text{head yaw}}\\
\theta_{\text{head pitch}}
\end{bmatrix}
$$

The simulator can plot:

- Joint position versus time
- Joint velocity versus time
- Joint acceleration versus time
- Command tracking
- Joint-limit violations

Because standard hobby servos generally don't provide position feedback, initial hardware tests verify commanded motion rather than true closed-loop joint tracking. Feedback-capable actuators can be added later if necessary.

This also allows the autonomy framework to be developed and tested before all the electronics are installed.

### 8. First implementation step

**Milestone M1: ESP32-S3 firmware bring-up.**

Establish a reliable development environment and a minimal application that:

1. Initializes the ESP32-S3.
2. Starts FreeRTOS tasks.
3. Prints periodic telemetry over USB.
4. Accepts commands from a Python application.
5. Reports system uptime and available memory.

Servo integration (PCA9685) comes later, in M7.

Development tools:

- Ubuntu Linux
- VS Code
- ESP-IDF
- Python 3
- Git

A single Git repository holds firmware, backend, simulation, and documentation.
