#!/usr/bin/env python3
"""Interactive serial console for the M1 firmware.

Prints telemetry and replies from the robot, and sends whatever you type as a
command (ping, status, rate <ms>, reboot). Ctrl-D or Ctrl-C to quit.

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
        )
    fields = {k: v for k, v in msg.items() if k != "type"}
    return f"[{msg['type']}] {json.dumps(fields)}"


def reader(port: serial.Serial, show_logs: bool, stop: threading.Event):
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
            print(format_msg(payload))
        elif show_logs and payload:
            print(f"  log | {payload}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-logs", action="store_true", help="hide ESP_LOG lines")
    args = ap.parse_args()

    port = serial.Serial(baudrate=args.baud, timeout=0.2)
    port.port = args.port
    # Keep DTR/RTS low so opening the port doesn't reset the board.
    port.dtr = False
    port.rts = False
    port.open()

    stop = threading.Event()
    threading.Thread(target=reader, args=(port, not args.no_logs, stop), daemon=True).start()

    try:
        for line in sys.stdin:
            if stop.is_set():
                break
            line = line.strip()
            if line:
                port.write((line + "\n").encode())
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        port.close()


if __name__ == "__main__":
    main()
