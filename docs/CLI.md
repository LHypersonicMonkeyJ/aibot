# aibot robot CLI reference

*As of 2026-10-27.*

## Getting started

Run the CLI from `firmware/`, type a command at the prompt and press Enter. It prints only the robot's replies.

```bash
cd firmware
pip install -r tools/requirements.txt   # first time only
python3 tools/robot_cli.py /dev/ttyACM0
```

- Plug into the board's **COM** port, not the one marked USB. It shows up as `/dev/ttyACM0`.
- Close `idf.py monitor` first. Only one program can hold the port at a time.
- Ctrl-D or Ctrl-C quits.

| Option          | Default  | What it does                         |
| --------------- | -------- | ------------------------------------ |
| `port`        | required | Serial device, e.g. `/dev/ttyACM0` |
| `--baud`      | 115200   | Serial speed                         |
| `--logs`      | off      | Show ESP_LOG lines from the firmware |
| `--telemetry` | off      | Show the periodic telemetry stream   |

Options go before the port: `python3 tools/robot_cli.py --telemetry /dev/ttyACM0`.

## Reading the output

Every command gets exactly one reply line, shown as `[type] {fields}`. An `ack` means it worked; an `error` says what went wrong.

```
expr happy
[ack] {"cmd": "expr", "name": "happy", "frame_us": 25805}
expr hapy
[error] {"msg": "usage: expr <neutral|happy|sad|angry|surprised|sleepy|listening|thinking>"}
```

| Type          | When you see it                                                                                              |
| ------------- | ------------------------------------------------------------------------------------------------------------ |
| `ack`       | A command succeeded. `cmd` names it; other fields are its result                                           |
| `error`     | Bad arguments, unknown command, or hardware not ready                                                        |
| `pong`      | Reply to `ping`                                                                                            |
| `telemetry` | Reply to `status`, or every second with `--telemetry`                                                    |
| `boot`      | The board just started: `reset_reason`, `idf` version, and whether the `pca9685` and `lcd` are ready |
| `i2cscan`   | Reply to `i2cscan`                                                                                         |

Under the hood the robot sends one JSON object per line. Commands are plain text, one per line.

## System commands

| Command       | What it does                                                               | Reply                                                             |
| ------------- | -------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| `ping`      | Checks the board is alive                                                  | `[pong] {"uptime_ms": …}`                                      |
| `status`    | Prints one telemetry line now                                              | `[telemetry] up 61.3s  heap …  internal …  psram …  tasks 6` |
| `rate <ms>` | Sets the telemetry period, 100 to 60000 ms. Back to 1000 ms after a reboot | `[ack] {"cmd": "rate", "period_ms": …}`                        |
| `reboot`    | Restarts the board                                                         | `[ack] {"cmd": "reboot"}`, then a `boot` line                 |

Telemetry fields: uptime, free heap, lowest free heap since boot, free internal RAM, free PSRAM, and task count.

## Face and display commands

The board shows the neutral face at boot. Faces are static for now; blinking, eye movement and smooth changes come with the M2 display task.

### Expressions: `expr <name>`

| Command            | Look                                  |
| ------------------ | ------------------------------------- |
| `expr neutral`   | Rounded rectangles, the boot face     |
| `expr happy`     | Upward crescents                      |
| `expr sad`       | Lids droop toward the outer corners   |
| `expr angry`     | Lids slant down toward the center     |
| `expr surprised` | Bigger, rounder eyes                  |
| `expr sleepy`    | Half-closed, flat top                 |
| `expr listening` | Slightly larger and raised            |
| `expr thinking`  | One eye squints, looking up and right |
| `expr`           | Lists the names                       |

Reply: `[ack] {"cmd": "expr", "name": "happy", "frame_us": 25805}`. `frame_us` is the time to draw one frame, about 26 ms.

### Screen controls

| Command                    | What it does                                                                                                |
| -------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `bright <0-100>`         | Sets backlight brightness in %. `bright` alone shows it. Boot default is 80                               |
| `lcdtest [image]`        | Test image: `pattern` (default), or solid `red`, `green`, `blue`, `white`, `black`              |
| `lcdcfg`                 | Shows the orientation and color settings                                                                    |
| `lcdcfg <setting> <0\|1>` | Changes `invert`, `mirror_x`, `mirror_y` or `swap_rb`, then redraws the pattern. Lasts until reboot |

The test pattern should show a red dot at the top, green on the right, blue at the bottom, a white dot in the center and a white ring at the edge, on black. Permanent orientation and color changes go in `idf.py menuconfig` under *aibot display*.

## Servo commands (M7, not yet tested)

These work once a PCA9685 is wired up. Without one, every command except `i2cscan` replies `pca9685 not found, check wiring and run i2cscan`.

| Command                  | What it does                                                                                           |
| ------------------------ | ------------------------------------------------------------------------------------------------------ |
| `i2cscan`              | Lists I2C devices in decimal. A PCA9685 shows as 64 (0x40), plus 112 (0x70)                            |
| `servo <joint> <deg>`  | Moves a joint straight to an angle, clamped to its limits. Reply includes `pulse_us` and `clamped` |
| `pulse <channel> <us>` | Raw pulse, 500 to 2500 µs, on any of the 16 channels. For calibration; ignores joint limits           |
| `relax [joint]`        | Stops pulses so the servo goes limp. No joint relaxes all                                              |

| Joint          | Index | Channel | Limits (placeholder) |
| -------------- | ----- | ------- | -------------------- |
| `body_yaw`   | 0     | 0       | −90° to 90°       |
| `head_yaw`   | 1     | 1       | −60° to 60°       |
| `head_pitch` | 2     | 2       | −20° to 30°       |

A joint can be given by name or index: `servo head_yaw 30` and `servo 1 30` do the same thing.

## Troubleshooting

| Symptom                                          | Likely cause                       | Fix                                                                                 |
| ------------------------------------------------ | ---------------------------------- | ----------------------------------------------------------------------------------- |
| No reply, or garbled replies                     | Another program holds the port     | Close `idf.py monitor` and other CLIs. `fuser -v /dev/ttyACM0` shows who has it |
| `[error] {"msg": "unknown command", …}`       | Typo, or a dash before the command | Type the bare command, e.g. `expr happy`, not `-expr happy`                     |
| Settings like `bright` or `lcdcfg` were lost | The board rebooted                 | Retype them, or set defaults in `idf.py menuconfig`                               |
| Screen dark, faint glow, or blank              | Poor ground, or a separate supply without a shared ground | Check GND is firmly connected. Any separate LCD supply must share ground with the ESP32. Then check BL (GPIO 21) and VCC |
| Backlight on but no image                        | SPI data wiring                    | Check DIN (GPIO 11), CLK (12), CS (10), DC (13)                                     |
| Wrong colors or position                         | Panel settings                     | Run `lcdtest`, adjust with `lcdcfg` (see Face and display commands)             |
