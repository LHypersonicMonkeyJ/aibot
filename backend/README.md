# Backend

Python code that runs on the PC and talks to the robot. In M5 that is the serial link; from M6 it also runs the conversation.

```bash
pip install -r backend/requirements.txt
python3 -m pytest backend/tests                     # no hardware needed
python3 -m backend.main /dev/ttyACM0                # demo on the robot
python3 -m backend.main /dev/ttyACM0 --soak 60      # 60 s of rapid commands from 3 threads
```

Run these from the repository root. Close `robot_cli.py` and `idf.py monitor` first, because only one program can hold the port.

## Using the robot from Python

```python
from backend.communication import Robot, RobotError

with Robot.open("/dev/ttyACM0") as robot:   # waits until the robot answers
    robot.expr("happy")
    robot.look(0.5, 0)
    print(robot.status()["uptime_ms"])
    robot.play(samples)                      # 16 kHz mono int16 numpy array; returns when played
    robot.mic_on(lambda block, seq: ...)     # 20 ms blocks of 16 kHz int16, on the reader thread
```

- **Commands:** `ping`, `status`, `rate`, `reboot`, `expr`, `look`, `blink`, `eyes_auto`, `talk`, `brightness`, `beep`, `volume`, `mic_gain`, `servo`, `relax`, `i2cscan`, plus `command("any line")` for the rest. Each returns the robot's reply as a dict.
- **Errors:** an `error` reply raises `RobotError` with the robot's message. No reply in time (2 s by default) raises `RobotTimeout`. If the robot reboots while a command waits, it raises `RobotReset`.
- **Threads:** commands can be sent from several threads at once. Use `send()` and `result()` to have several in flight from one thread.
- **Telemetry:** `robot.telemetry` holds the latest message. `robot.on_telemetry(fn)` and `robot.on_boot(fn)` register listeners, which run on the reader thread.
- **Audio out:** `play()` keeps about 0.75 s queued on the robot. `stop_audio()` drops whatever is queued.
- **Audio in:** `mic_on(callback)` and `mic_off()`. `robot.mic_gaps` counts blocks lost since `mic_on`. The callback must return quickly, because it blocks the reader thread.
- **Logs:** firmware log lines go to the `backend.communication.serial_interface` logger at DEBUG level. `main.py --logs` shows them.

## How replies are matched

Protocol v0 has no message IDs, but the robot answers every command exactly once and in order. The backend keeps pending commands in a queue and checks each reply against its command:

- an `ack` names its `cmd`
- `pong`, `telemetry`, `i2cscan` and `pcm` belong to `ping`, `status`, `i2cscan` and `pcm`

If a reply belongs to a later command, the earlier ones lost their replies, so they fail straight away instead of waiting for the timeout. `status` replies with an ordinary telemetry message, so telemetry answers a `status` only when that `status` is next in line.

## Layout

| Path | What it is |
|---|---|
| `communication/serial_interface.py` | `Robot`: connection, commands, reply matching, telemetry, audio in and out |
| `main.py` | Demo and soak test on the real robot |
| `tests/fake_robot.py` | A fake robot that speaks protocol v0, for tests |
| `tests/test_serial_interface.py` | Tests for `Robot` |
