#!/usr/bin/env python3
"""Interactive serial console for the M1 firmware.

Sends whatever you type as a command (ping, status, expr happy, ...) and prints
the robot's replies. Periodic telemetry and ESP_LOG lines are hidden unless you
pass --telemetry or --logs. Ctrl-D or Ctrl-C to quit.

    python3 firmware/tools/robot_cli.py /dev/ttyUSB0
"""

import argparse
import json
import sys
import threading

import serial


def parse_line(raw: bytes):
    """Return (kind, payload): ("msg", dict) for protocol lines, ("log", str) otherwise."""
    text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
    if text.startswith("{"):
        try:
            msg = json.loads(text)
            if isinstance(msg, dict) and "type" in msg:
                return "msg", msg
        except json.JSONDecodeError:
            pass
    return "log", text


def format_msg(msg: dict) -> str:
    if msg["type"] == "telemetry":
        return (
            f"[telemetry] up {msg['uptime_ms'] / 1000:.1f}s  "
            f"heap {msg['free_heap']} (min {msg['min_free_heap']})  "
            f"internal {msg['free_internal']}  psram {msg['free_psram']}  "
            f"tasks {msg['tasks']}"
            + (f"  fps {msg['fps']} (drawn {msg['drawn_fps']})  frame {msg['frame_us'] / 1000:.1f}ms"
               if "fps" in msg else "")
            + (f"  audio {msg['audio_ms']}ms  dropouts {msg['dropouts']}" if "audio_ms" in msg else "")
            + (f"  mic peak {msg['mic_peak']:.3f}  overruns {msg['mic_overruns']}" if "mic_peak" in msg else "")
        )
    fields = {k: v for k, v in msg.items() if k != "type"}
    return f"[{msg['type']}] {json.dumps(fields)}"


def reader(port: serial.Serial, show_logs: bool, show_telemetry: bool,
           want_status: threading.Event, stop: threading.Event):
    while not stop.is_set():
        try:
            raw = port.readline()
        except serial.SerialException as exc:
            print(f"\n[serial error] {exc}", file=sys.stderr)
            stop.set()
            return
        if not raw:
            continue
        kind, payload = parse_line(raw)
        if kind == "msg":
            if payload["type"] == "mic":  # streamed audio after "mic on"; use record.py to save it
                continue
            if payload["type"] == "telemetry":
                # "status" replies with a telemetry message, so always show that one.
                if not (show_telemetry or want_status.is_set()):
                    continue
                want_status.clear()
            print(format_msg(payload))
        elif show_logs and payload:
            print(f"  log | {payload}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--logs", action="store_true", help="show ESP_LOG lines")
    ap.add_argument("--telemetry", action="store_true", help="show periodic telemetry")
    args = ap.parse_args()

    port = serial.Serial(baudrate=args.baud, timeout=0.2)
    port.port = args.port
    # Keep DTR/RTS low so opening the port doesn't reset the board.
    port.dtr = False
    port.rts = False
    port.open()

    stop = threading.Event()
    want_status = threading.Event()
    threading.Thread(target=reader, args=(port, args.logs, args.telemetry, want_status, stop),
                     daemon=True).start()

    try:
        for line in sys.stdin:
            if stop.is_set():
                break
            line = line.strip()
            if line:
                if line.split()[0] == "status":
                    want_status.set()
                port.write((line + "\n").encode())
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        port.close()


if __name__ == "__main__":
    main()
