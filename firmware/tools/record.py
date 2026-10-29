#!/usr/bin/env python3
"""Record from the robot's microphone into a WAV file.

Turns the robot's mic stream on, shows a live level bar, and saves 16 kHz
16-bit mono audio. Afterwards it reports gaps, levels and clipping.

    python3 tools/record.py /dev/ttyACM0                     # 5 s into mic.wav
    python3 tools/record.py /dev/ttyACM0 -s 10 -o hello.wav
    python3 tools/record.py /dev/ttyACM0 --gain 30           # set the mic gain first (0-40 dB)
    python3 tools/say.py /dev/ttyACM0 --wav hello.wav        # play it back on the robot

Close robot_cli.py and idf.py monitor first; only one program can hold the port.
"""

import argparse
import base64
import json
import sys
import time

import numpy as np

from say import RATE, open_port, read_msg, save_wav

BLOCK = RATE // 50  # samples per mic line (20 ms)
BAR = 40


def dbfs(x: float) -> float:
    return 20 * np.log10(max(x, 1e-9))


def level_bar(block: np.ndarray) -> str:
    peak = np.abs(block.astype(np.int32)).max() / 32768 if len(block) else 0
    if peak == 0:
        return f"[{' ' * BAR}]   silent"
    db = dbfs(peak)
    filled = int(np.clip((db + 60) / 60, 0, 1) * BAR)  # -60 dBFS .. 0 dBFS
    return f"[{'#' * filled}{' ' * (BAR - filled)}] {db:6.1f} dBFS"


def record(port, seconds: float) -> tuple[np.ndarray, int]:
    """Returns the audio and the number of blocks missing from the stream."""
    want = int(seconds * RATE)
    blocks, missing, last_seq = [], 0, None
    port.write(b"mic on\n")
    deadline = time.monotonic() + seconds + 3
    try:
        while sum(len(b) for b in blocks) < want:
            if time.monotonic() > deadline:
                sys.exit("\nno mic data from the robot; is the firmware up to date?")
            raw = port.readline()
            if not raw.startswith(b'{"type":"mic"'):
                if raw.startswith(b'{"type":"error"'):
                    sys.exit(f"\nrobot error: {json.loads(raw).get('msg')}")
                continue
            msg = json.loads(raw)
            seq = msg["seq"]
            if last_seq is not None and seq != last_seq + 1:
                gap = max(seq - last_seq - 1, 0)
                missing += gap
                blocks.extend(np.zeros(BLOCK, dtype=np.int16) for _ in range(gap))  # keep timing
            last_seq = seq
            block = np.frombuffer(base64.b64decode(msg["data"]), dtype="<i2")
            blocks.append(block)
            if len(blocks) % 5 == 0:  # refresh the bar every 100 ms
                print(f"\r{len(blocks) * BLOCK / RATE:5.1f} s  {level_bar(block)}", end="", flush=True)
    finally:
        port.write(b"mic off\n")
        print()
    audio = np.concatenate(blocks)[:want] if blocks else np.zeros(0, dtype=np.int16)
    return audio, missing


def report(audio: np.ndarray, missing: int):
    x = audio.astype(np.float64) / 32768
    peak = np.abs(x).max() if len(x) else 0
    rms = np.sqrt(np.mean(x ** 2)) if len(x) else 0
    clipped = int(np.sum(np.abs(audio.astype(np.int32)) >= 32767))
    print(f"recorded {len(audio) / RATE:.1f} s, {missing} blocks missing (gaps)")
    if peak > 0:
        print(f"peak {dbfs(peak):.1f} dBFS, average {dbfs(rms):.1f} dBFS, {clipped} clipped samples")
    if peak == 0:
        print("warning: all zeros. Is the mic wired (SD on GPIO 17, L/R to GND, VDD on 3V3)?")
    elif clipped:
        print("warning: clipping. Lower the gain (--gain) or speak further away.")
    elif dbfs(peak) < -30:
        print("warning: very quiet. Raise the gain (--gain) or speak closer.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyACM0")
    ap.add_argument("-s", "--seconds", type=float, default=5, help="how long to record (default 5)")
    ap.add_argument("-o", "--out", default="mic.wav", help="WAV file to write (default mic.wav)")
    ap.add_argument("--gain", type=int, help="set the mic gain in dB (0-40) before recording")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    port = open_port(args.port, args.baud)
    try:
        if args.gain is not None:
            port.write(f"mic gain {args.gain}\n".encode())
            msg = read_msg(port, 2.0)
            if not msg or msg.get("type") != "ack":
                sys.exit(f"robot refused the gain: {msg}")
        print(f"recording {args.seconds:g} s ...")
        audio, missing = record(port, args.seconds)
    finally:
        port.close()
    save_wav(args.out, audio)
    print(f"saved {args.out}")
    report(audio, missing)


if __name__ == "__main__":
    main()
