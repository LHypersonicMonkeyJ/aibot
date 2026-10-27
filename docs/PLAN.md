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
robot/
│
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
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

| Milestone | Objective | Verification |
|---|---|---|
| M1 | ESP32 development environment | Flash firmware and read serial logs |
| M2 | Servo control | Independently command three servos |
| M3 | Motion trajectories | Smooth, bounded servo movements |
| M4 | LCD interface | Animated eyes and expressions |
| M5 | Audio output | Play synthesized speech |
| M6 | Audio input | Record microphone audio |
| M7 | Python communication | Send commands and receive telemetry |
| M8 | AI integration | Hold a conversation with coordinated expressions and motion |

After M8, we implement autonomous behaviors.

#### M9 — Autonomous behavior framework

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

Then integrate the PCA9685 and one servo.

Development tools:

- Ubuntu Linux
- VS Code
- ESP-IDF
- Python 3
- Git

A single Git repository holds firmware, backend, simulation, and documentation.
