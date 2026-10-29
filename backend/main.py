"""Demo and soak test for the backend on the real robot.

    python3 -m backend.main /dev/ttyACM0                # demo: face, eyes, sound, telemetry
    python3 -m backend.main /dev/ttyACM0 --wav hi.wav   # play a WAV in the demo
    python3 -m backend.main /dev/ttyACM0 --soak 60      # 60 s of rapid commands from 3 threads

Close robot_cli.py and idf.py monitor first; only one program can hold the port.
"""

import argparse
import logging
import random
import statistics
import threading
import time
import wave

import numpy as np

from backend.communication import RATE, Robot, RobotError, RobotTimeout

EXPRESSIONS = ["happy", "sad", "angry", "surprised", "sleepy", "listening", "thinking", "neutral"]


def summary(t: dict) -> str:
    return (f"up {t['uptime_ms'] / 1000:.1f}s  heap {t.get('free_heap', '?')}  fps {t.get('fps', '?')}  "
            f"audio {t.get('audio_ms', '?')}ms  dropouts {t.get('dropouts', '?')}  "
            f"mic overruns {t.get('mic_overruns', '?')}")


def chime() -> np.ndarray:
    """C-E-G arpeggio, 0.9 s, with short fades."""
    out = []
    for hz in (523.25, 659.25, 783.99):
        t = np.arange(int(0.3 * RATE)) / RATE
        env = np.minimum(1, np.minimum(t, t[::-1]) / 0.01)
        out.append(np.sin(2 * np.pi * hz * t) * env * 0.4)
    return (np.concatenate(out) * 32767).astype(np.int16)


def read_wav(path: str) -> np.ndarray:
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit("only 16-bit WAV files are supported")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)
        if w.getnchannels() > 1:
            data = data.reshape(-1, w.getnchannels()).mean(axis=1)
        rate = w.getframerate()
    if rate != RATE:  # linear resample; fine for a demo
        n = int(len(data) * RATE / rate)
        data = np.interp(np.arange(n) * rate / RATE, np.arange(len(data)), data)
    return np.clip(data, -32768, 32767).astype(np.int16)


def demo(robot: Robot, wav: str | None):
    print("ping x20 ...", end=" ", flush=True)
    times = []
    for _ in range(20):
        t0 = time.perf_counter()
        robot.ping()
        times.append((time.perf_counter() - t0) * 1000)
    print(f"round trip min {min(times):.1f} / avg {statistics.mean(times):.1f} / max {max(times):.1f} ms")
    print("status:", summary(robot.status()))

    print("expressions:", end=" ", flush=True)
    for name in EXPRESSIONS:
        robot.expr(name)
        print(name, end=" ", flush=True)
        time.sleep(0.8)
    print()

    print("eyes: look around, blink")
    for x, y in [(-0.8, 0), (0.8, 0), (0, -0.8), (0, 0.8), (0, 0)]:
        robot.look(x, y)
        time.sleep(0.5)
    robot.blink()
    time.sleep(0.4)
    robot.eyes_auto(True)

    print("sound: beep, then", wav or "a chime")
    robot.beep(660, 200)
    time.sleep(0.4)
    audio = read_wav(wav) if wav else chime()
    t0 = time.monotonic()
    dropped = robot.play(audio)
    print(f"  played {len(audio) / RATE:.1f} s in {time.monotonic() - t0:.1f} s, {dropped} samples dropped")

    print("status:", summary(robot.status()))


def soak(robot: Robot, seconds: float):
    robot.rate(100)  # 10 telemetry messages a second alongside the commands
    boots_before = robot.boot_count
    stop = time.monotonic() + seconds
    lock = threading.Lock()
    stats = {"ok": 0, "errors": 0, "timeouts": 0, "wrong": 0}
    latencies: list[float] = []
    telemetry = [0]
    robot.on_telemetry(lambda _msg: telemetry.__setitem__(0, telemetry[0] + 1))

    def check(fn, expect):
        t0 = time.perf_counter()
        try:
            reply = fn()
            ok = expect(reply)
            key = "ok" if ok else "wrong"
            if not ok:
                print(f"\n  wrong reply: {reply}")
        except RobotTimeout:
            key = "timeouts"
        except RobotError as exc:
            key = "errors"
            print(f"\n  error: {exc}")
        with lock:
            stats[key] += 1
            latencies.append((time.perf_counter() - t0) * 1000)

    def pings():
        while time.monotonic() < stop:
            check(robot.ping, lambda r: r["type"] == "pong")
            check(robot.status, lambda r: r["type"] == "telemetry")

    def faces():
        while time.monotonic() < stop:
            name = random.choice(EXPRESSIONS)
            check(lambda: robot.expr(name), lambda r, n=name: r.get("name") == n)
            x, y = random.uniform(-1, 1), random.uniform(-1, 1)
            check(lambda: robot.look(x, y), lambda r, x=x: abs(r.get("x", 9) - round(x, 2)) < 0.011)

    def volumes():
        while time.monotonic() < stop:
            pct = random.randint(40, 60)
            check(lambda: robot.volume(pct), lambda r, p=pct: r.get("percent") == p)

    threads = [threading.Thread(target=f) for f in (pings, faces, volumes)]
    print(f"soak: {seconds:g} s, 3 threads, telemetry every 100 ms ...")
    t0 = time.monotonic()
    for t in threads:
        t.start()
    while any(t.is_alive() for t in threads):
        time.sleep(1)
        with lock:
            print(f"\r  {time.monotonic() - t0:5.0f} s  {stats}", end="", flush=True)
    print()

    robot.rate(1000)
    robot.volume(50)
    robot.expr("neutral")
    robot.eyes_auto(True)
    total = sum(stats.values())
    latencies.sort()
    print(f"{total} commands ({total / seconds:.0f}/s): {stats['ok']} ok, {stats['wrong']} wrong replies, "
          f"{stats['errors']} errors, {stats['timeouts']} timeouts")
    print(f"latency median {latencies[len(latencies) // 2]:.1f} ms, 99th percentile "
          f"{latencies[int(len(latencies) * 0.99)]:.1f} ms, max {latencies[-1]:.1f} ms")
    print(f"telemetry messages (periodic plus status replies): {telemetry[0]} ({telemetry[0] / seconds:.1f}/s), "
          f"robot reboots during the soak: {robot.boot_count - boots_before}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyACM0")
    ap.add_argument("--wav", help="16-bit WAV file to play in the demo")
    ap.add_argument("--soak", type=float, metavar="SECONDS", help="run the soak test instead of the demo")
    ap.add_argument("--logs", action="store_true", help="show firmware log lines")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.logs else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s")

    with Robot.open(args.port) as robot:
        if robot.boot:
            print("boot:", {k: v for k, v in robot.boot.items() if k != "type"})
        if args.soak:
            soak(robot, args.soak)
        else:
            demo(robot, args.wav)


if __name__ == "__main__":
    main()
