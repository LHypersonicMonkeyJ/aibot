#!/usr/bin/env python3
"""Speak text through the robot's speaker.

Turns text into speech on this PC with Piper, resamples it to the robot's
16 kHz, and streams it over the serial link. The robot's mouth moves with
the sound.

    python3 tools/say.py /dev/ttyACM0 "Hello, I'm aibot"
    python3 tools/say.py /dev/ttyACM0 --wav some.wav     # play a WAV file instead
    python3 tools/say.py /dev/ttyACM0 "Hi" --save hi.wav  # also keep what was sent

The first run downloads the voice (about 60 MB) into tools/voices/.
Close robot_cli.py and idf.py monitor first; only one program can hold the port.
"""

import argparse
import base64
import json
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np
import serial

RATE = 16000                 # robot sample rate
CHUNK = 320                  # samples per pcm line (20 ms); fits the 1024-char line limit
WINDOW = 8                   # pcm lines in flight before waiting for replies
TARGET_BUFFER = RATE * 3 // 4  # keep about 0.75 s queued on the robot
VOICES = Path(__file__).resolve().parent / "voices"
DEFAULT_VOICE = "en_US-lessac-medium"


def synthesize(text: str, voice_name: str) -> tuple[np.ndarray, int]:
    from piper import PiperVoice

    model = VOICES / f"{voice_name}.onnx"
    if not model.exists():
        print(f"downloading voice {voice_name} into {VOICES} ...", file=sys.stderr)
        VOICES.mkdir(exist_ok=True)
        subprocess.run([sys.executable, "-m", "piper.download_voices", "--data-dir", str(VOICES),
                        voice_name], check=True)
    voice = PiperVoice.load(str(model))
    chunks = list(voice.synthesize(text))
    if not chunks:
        return np.zeros(0, dtype=np.int16), RATE
    return np.concatenate([c.audio_int16_array for c in chunks]), chunks[0].sample_rate


def read_wav(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit("only 16-bit WAV files are supported")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        if w.getnchannels() > 1:
            data = data.reshape(-1, w.getnchannels()).mean(axis=1).astype(np.int16)
        return data, w.getframerate()


def resample(audio: np.ndarray, rate: int) -> np.ndarray:
    if rate == RATE or len(audio) == 0:
        return audio.astype(np.int16)
    x = audio.astype(np.float32)
    if rate > RATE:
        # Simple low-pass (moving average) before downsampling, to limit aliasing.
        k = int(round(rate / RATE)) + 1
        x = np.convolve(x, np.ones(k) / k, mode="same")
    n_out = int(len(x) * RATE / rate)
    t = np.arange(n_out) * rate / RATE
    return np.clip(np.interp(t, np.arange(len(x)), x), -32768, 32767).astype(np.int16)


def save_wav(path: str, audio: np.ndarray):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(audio.tobytes())


def read_msg(port: serial.Serial, timeout: float):
    """Next protocol message other than telemetry, or None on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw.startswith(b"{"):
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if isinstance(msg, dict) and msg.get("type") not in (None, "telemetry"):
            return msg
    return None


def open_port(path: str, baud: int) -> serial.Serial:
    port = serial.Serial(baudrate=baud, timeout=0.05)
    port.port = path
    port.dtr = False  # keep the board from resetting where the USB chip allows
    port.rts = False
    port.open()
    # Opening the port can reset the board. Ping until it answers, so audio
    # isn't sent while it is still booting.
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        port.write(b"\nping\n")
        msg = read_msg(port, 0.5)
        if msg and msg.get("type") == "pong":
            time.sleep(0.05)
            port.reset_input_buffer()
            return port
    sys.exit(f"no answer from the robot on {path}; is it running the latest firmware?")


def robot_dropouts(port: serial.Serial):
    """The robot's dropout count since boot, from a status request."""
    port.write(b"status\n")
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        raw = port.readline()
        if raw.startswith(b"{") and b'"telemetry"' in raw:
            try:
                return json.loads(raw).get("dropouts")
            except json.JSONDecodeError:
                pass
    return None


def stream(port: serial.Serial, audio: np.ndarray) -> int:
    """Sends the audio with flow control. Returns the number of dropped samples."""
    chunks = [audio[i:i + CHUNK] for i in range(0, len(audio), CHUNK)]
    sent = acked = dropped = 0
    robot_buffered = 0
    reply_time = time.monotonic()

    def estimate() -> float:
        # What the robot holds now: last report, minus what played since, plus in flight.
        played = (time.monotonic() - reply_time) * RATE
        return max(0.0, robot_buffered - played) + (sent - acked) * CHUNK

    while acked < len(chunks):
        while sent < len(chunks) and sent - acked < WINDOW and estimate() < TARGET_BUFFER:
            port.write(b"pcm " + base64.b64encode(chunks[sent].astype("<i2").tobytes()) + b"\n")
            sent += 1
        if sent == acked:  # buffer full and nothing in flight: let it play down
            time.sleep(0.01)
            continue
        msg = read_msg(port, 2.0)
        if msg is None:
            sys.exit("no reply from the robot; is the firmware up to date and the baud 921600?")
        if msg["type"] == "error":
            sys.exit(f"robot error: {msg.get('msg')}")
        if msg["type"] == "pcm":
            acked += 1
            robot_buffered = msg["buffered"]
            reply_time = time.monotonic()
            dropped += msg["dropped"]

    port.write(b"pcm end\n")
    read_msg(port, 2.0)
    time.sleep(estimate() / RATE + 0.1)  # let it finish playing
    return dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial device, e.g. /dev/ttyACM0")
    ap.add_argument("text", nargs="*", help="what to say")
    ap.add_argument("--wav", help="play this 16-bit WAV file instead of speaking text")
    ap.add_argument("--voice", default=DEFAULT_VOICE, help=f"Piper voice name (default {DEFAULT_VOICE})")
    ap.add_argument("--save", help="also write the 16 kHz audio that was sent to this WAV file")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    if args.wav:
        audio, rate = read_wav(args.wav)
    elif args.text:
        audio, rate = synthesize(" ".join(args.text), args.voice)
    else:
        ap.error("give some text to say, or --wav FILE")
    audio = resample(audio, rate)
    if args.save:
        save_wav(args.save, audio)

    port = open_port(args.port, args.baud)
    try:
        start = time.monotonic()
        dropped = stream(port, audio)
        elapsed = time.monotonic() - start
        dropouts = robot_dropouts(port)
    finally:
        port.close()
    print(f"played {len(audio) / RATE:.1f} s in {elapsed:.1f} s"
          + (f", {dropped} samples dropped (buffer full)" if dropped else "")
          + (f", robot dropouts since boot: {dropouts}" if dropouts is not None else ""))


if __name__ == "__main__":
    main()
