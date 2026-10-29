"""Talks to the robot over USB serial (protocol v0).

Host to robot: one plain-text command per line. Robot to host: one JSON
object per line with a "type" field; anything else is a firmware log line.
Every command gets exactly one reply, in order, but replies carry no ID, so
pending commands are matched first-in, first-out and each reply is checked
against its command (see _belongs_to).

    with Robot.open("/dev/ttyACM0") as robot:
        robot.expr("happy")
        print(robot.status()["uptime_ms"])
"""

from __future__ import annotations

import base64
import collections
import json
import logging
import threading
import time
from typing import Callable, Optional, Protocol

import numpy as np

log = logging.getLogger(__name__)

RATE = 16000                  # robot audio sample rate, both directions
PCM_CHUNK = 320               # samples per pcm line (20 ms); fits the robot's 1024-char lines
PCM_WINDOW = 8                # pcm lines in flight before waiting for replies
PCM_TARGET = RATE * 3 // 4    # keep about 0.75 s queued on the robot
DEFAULT_TIMEOUT = 2.0

# Replies whose type, not an "ack" with a "cmd", identifies the command.
_REPLY_TYPE = {"ping": "pong", "status": "telemetry", "i2cscan": "i2cscan", "pcm": "pcm"}


class RobotError(Exception):
    """The robot replied with an error, or the reply never came."""


class RobotTimeout(RobotError):
    """No reply within the timeout."""


class RobotReset(RobotError):
    """The robot rebooted while the command was waiting."""


class Transport(Protocol):
    """What Robot needs from a serial port. pyserial's Serial fits."""

    def readline(self) -> bytes: ...        # b"" on timeout
    def write(self, data: bytes) -> int: ...
    def close(self) -> None: ...


class _Pending:
    def __init__(self, name: str):
        self.name = name
        self.done = threading.Event()
        self.reply: Optional[dict] = None
        self.error: Optional[Exception] = None

    def finish(self, reply: Optional[dict] = None, error: Optional[Exception] = None):
        self.reply, self.error = reply, error
        self.done.set()


def _belongs_to(msg: dict, name: str) -> bool:
    """Whether a reply message is the answer to command `name`."""
    if msg["type"] == "error":
        return msg.get("cmd", name) == name  # unknown-command errors name the command
    if name in _REPLY_TYPE:
        return msg["type"] == _REPLY_TYPE[name]
    return msg.get("cmd") == name


class Robot:
    def __init__(self, transport: Transport, timeout: float = DEFAULT_TIMEOUT):
        self._io = transport
        self.timeout = timeout
        self._send_lock = threading.Lock()   # keeps lines whole and in pending order
        self._lock = threading.Lock()        # guards _pending
        self._pending: collections.deque[_Pending] = collections.deque()
        self._closed = threading.Event()

        self.telemetry: Optional[dict] = None   # latest telemetry message
        self.boot: Optional[dict] = None        # latest boot message
        self.boot_count = 0                     # boots seen since connecting
        self._telemetry_listeners: list[Callable[[dict], None]] = []
        self._boot_listeners: list[Callable[[dict], None]] = []

        self._mic_callback: Optional[Callable[[np.ndarray, int], None]] = None
        self._mic_last_seq: Optional[int] = None
        self.mic_gaps = 0                       # mic blocks missing since mic_on()

        self._reader = threading.Thread(target=self._read_loop, name="robot-reader", daemon=True)
        self._reader.start()

    # --- Connection ---

    @classmethod
    def open(cls, path: str, baud: int = 921600, timeout: float = DEFAULT_TIMEOUT,
             ready_timeout: float = 8.0) -> "Robot":
        """Opens the serial port and waits until the robot answers."""
        import serial

        port = serial.Serial(baudrate=baud, timeout=0.05)
        port.port = path
        port.dtr = False  # avoids a reset where the USB chip allows; this board resets anyway
        port.rts = False
        port.open()
        robot = cls(port, timeout)
        try:
            robot.wait_ready(ready_timeout)
        except Exception:
            robot.close()
            raise
        return robot

    def wait_ready(self, timeout: float = 8.0):
        """Pings until the robot answers, e.g. while it boots after the port opened."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.command("ping", timeout=0.5)
                return
            except RobotError:
                continue
        raise RobotTimeout(f"no answer from the robot within {timeout:g} s")

    def close(self):
        self._closed.set()
        self._fail_all(RobotError("connection closed"))
        self._io.close()
        self._reader.join(timeout=1)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # --- Sending ---

    def send(self, line: str) -> _Pending:
        """Sends a command without waiting. Wait with result()."""
        name = line.split(maxsplit=1)[0] if line.strip() else ""
        if not name or "\n" in line:
            raise ValueError(f"bad command line: {line!r}")
        pending = _Pending(name)
        with self._send_lock:
            if self._closed.is_set():
                raise RobotError("connection closed")
            with self._lock:
                self._pending.append(pending)
            self._io.write(line.encode() + b"\n")
        return pending

    def result(self, pending: _Pending, timeout: Optional[float] = None) -> dict:
        if not pending.done.wait(self.timeout if timeout is None else timeout):
            with self._lock:
                if pending in self._pending:
                    self._pending.remove(pending)
            if not pending.done.is_set():
                raise RobotTimeout(f"no reply to {pending.name!r}")
        if pending.error:
            raise pending.error
        return pending.reply

    def command(self, line: str, timeout: Optional[float] = None) -> dict:
        """Sends a command and returns its reply."""
        return self.result(self.send(line), timeout)

    # --- Commands ---

    def ping(self) -> dict:
        return self.command("ping")

    def status(self) -> dict:
        return self.command("status")

    def rate(self, ms: int) -> dict:
        return self.command(f"rate {int(ms)}")

    def reboot(self) -> dict:
        return self.command("reboot")

    def expr(self, name: str) -> dict:
        return self.command(f"expr {name}")

    def look(self, x: float, y: float) -> dict:
        return self.command(f"look {x:.2f} {y:.2f}")

    def blink(self) -> dict:
        return self.command("blink")

    def eyes_auto(self, on: Optional[bool] = None) -> dict:
        return self.command("eyes" if on is None else f"eyes auto {'on' if on else 'off'}")

    def talk(self, on: Optional[bool] = None) -> dict:
        return self.command("talk" if on is None else f"talk {'on' if on else 'off'}")

    def brightness(self, percent: Optional[int] = None) -> dict:
        return self.command("bright" if percent is None else f"bright {int(percent)}")

    def beep(self, hz: int = 880, ms: int = 300) -> dict:
        return self.command(f"beep {int(hz)} {int(ms)}")

    def volume(self, percent: Optional[int] = None) -> dict:
        return self.command("vol" if percent is None else f"vol {int(percent)}")

    def mic_gain(self, db: Optional[int] = None) -> dict:
        return self.command("mic" if db is None else f"mic gain {int(db)}")

    def servo(self, joint: str | int, deg: float) -> dict:
        return self.command(f"servo {joint} {deg:g}")

    def relax(self, joint: str | int | None = None) -> dict:
        return self.command("relax" if joint is None else f"relax {joint}")

    def i2cscan(self) -> dict:
        return self.command("i2cscan")

    # --- Audio out ---

    def play(self, samples: np.ndarray) -> int:
        """Streams 16 kHz mono int16 audio to the speaker and waits until it has
        played. Returns the number of samples the robot had to drop."""
        audio = np.asarray(samples, dtype=np.int16)
        chunks = [audio[i:i + PCM_CHUNK] for i in range(0, len(audio), PCM_CHUNK)]
        in_flight: collections.deque[_Pending] = collections.deque()
        robot_buffered, reply_time, dropped = 0, time.monotonic(), 0

        def queued() -> float:
            # What the robot holds now: last report, minus what played since, plus in flight.
            played = (time.monotonic() - reply_time) * RATE
            return max(0.0, robot_buffered - played) + len(in_flight) * PCM_CHUNK

        sent = 0
        while sent < len(chunks) or in_flight:
            while sent < len(chunks) and len(in_flight) < PCM_WINDOW and queued() < PCM_TARGET:
                data = base64.b64encode(chunks[sent].astype("<i2").tobytes()).decode()
                in_flight.append(self.send(f"pcm {data}"))
                sent += 1
            if not in_flight:  # robot buffer full: let it play down
                time.sleep(0.01)
                continue
            reply = self.result(in_flight.popleft())
            robot_buffered, reply_time = reply["buffered"], time.monotonic()
            dropped += reply["dropped"]

        reply = self.command("pcm end")
        time.sleep(reply["buffered"] / RATE + 0.05)  # let it finish playing
        return dropped

    def stop_audio(self) -> dict:
        """Drops everything queued on the robot."""
        return self.command("pcm stop")

    # --- Audio in ---

    def mic_on(self, callback: Callable[[np.ndarray, int], None]) -> dict:
        """Streams the mic. callback(samples, seq) runs on the reader thread for
        each 20 ms block of 16 kHz int16 audio, so it must return quickly."""
        self._mic_last_seq = None
        self.mic_gaps = 0
        self._mic_callback = callback
        return self.command("mic on")

    def mic_off(self) -> dict:
        reply = self.command("mic off")
        self._mic_callback = None
        return reply

    # --- Listeners ---

    def on_telemetry(self, fn: Callable[[dict], None]):
        self._telemetry_listeners.append(fn)

    def on_boot(self, fn: Callable[[dict], None]):
        self._boot_listeners.append(fn)

    # --- Receiving ---

    def _read_loop(self):
        while not self._closed.is_set():
            try:
                raw = self._io.readline()
            except Exception as exc:  # port unplugged or closed under us
                if not self._closed.is_set():
                    log.error("serial read failed: %s", exc)
                    self._fail_all(RobotError(f"serial read failed: {exc}"))
                return
            if raw:
                try:
                    self._handle_line(raw)
                except Exception:
                    log.exception("error handling line from robot: %r", raw[:120])

    def _handle_line(self, raw: bytes):
        text = raw.decode("utf-8", errors="replace").strip()
        msg = None
        if text.startswith("{"):
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                pass
        if not isinstance(msg, dict) or "type" not in msg:
            if text:
                log.debug("robot: %s", text)
            return

        kind = msg["type"]
        if kind == "mic":
            self._handle_mic(msg)
        elif kind == "boot":
            self.boot = msg
            self.boot_count += 1
            self._mic_callback = None  # a rebooted robot isn't streaming
            self._fail_all(RobotReset("robot rebooted"))
            for fn in list(self._boot_listeners):
                fn(msg)
        elif kind == "telemetry":
            self.telemetry = msg
            with self._lock:
                head = self._pending[0] if self._pending else None
                if head and head.name == "status":
                    self._pending.popleft()
                else:
                    head = None
            if head:
                head.finish(msg)
            for fn in list(self._telemetry_listeners):
                fn(msg)
        else:
            self._handle_reply(msg)

    def _handle_reply(self, msg: dict):
        with self._lock:
            # The robot answers in order, so a reply for a later command means
            # everything before it lost its reply.
            match = next((i for i, p in enumerate(self._pending) if _belongs_to(msg, p.name)), None)
            if match is None:
                log.warning("reply matches no pending command: %s", msg)
                return
            lost = [self._pending.popleft() for _ in range(match)]
            pending = self._pending.popleft()
        for p in lost:
            p.finish(error=RobotError(f"no reply to {p.name!r}"))
        if msg["type"] == "error":
            pending.finish(error=RobotError(msg.get("msg", "robot error")))
        else:
            pending.finish(msg)

    def _handle_mic(self, msg: dict):
        callback = self._mic_callback
        if callback is None:
            return
        seq = msg["seq"]
        if self._mic_last_seq is not None and seq != self._mic_last_seq + 1:
            self.mic_gaps += max(seq - self._mic_last_seq - 1, 0)
        self._mic_last_seq = seq
        callback(np.frombuffer(base64.b64decode(msg["data"]), dtype="<i2"), seq)

    def _fail_all(self, error: Exception):
        with self._lock:
            pending, self._pending = list(self._pending), collections.deque()
        for p in pending:
            p.finish(error=error)
