import logging
import threading
import time

import numpy as np
import pytest

from backend.communication import RATE, Robot, RobotError, RobotReset, RobotTimeout
from backend.tests.fake_robot import FakeRobot


@pytest.fixture
def fake():
    return FakeRobot()


@pytest.fixture
def robot(fake):
    r = Robot(fake, timeout=1.0)
    yield r
    r.close()


def test_ping_and_commands(robot, fake):
    assert robot.ping()["type"] == "pong"
    assert robot.expr("happy") == {"type": "ack", "cmd": "expr", "name": "happy"}
    assert robot.volume(30)["percent"] == 30
    assert robot.volume()["percent"] == 30
    robot.look(0.5, -0.25)
    assert fake.received[-1] == "look 0.50 -0.25"


def test_error_reply_raises(robot):
    with pytest.raises(RobotError, match="usage: expr"):
        robot.expr("grumpy")
    assert robot.ping()["type"] == "pong"  # still in sync afterwards


def test_unknown_command_raises(robot):
    with pytest.raises(RobotError, match="unknown command"):
        robot.command("dance")


def test_timeout(robot, fake):
    fake.drop_replies.add("blink")
    t0 = time.monotonic()
    with pytest.raises(RobotTimeout):
        robot.command("blink", timeout=0.2)
    assert time.monotonic() - t0 < 0.5
    assert robot.ping()["type"] == "pong"


def test_lost_reply_fails_fast(robot, fake):
    """A reply for a later command means the earlier one will never get its reply."""
    fake.drop_replies.add("blink")
    first = robot.send("blink")
    t0 = time.monotonic()
    assert robot.expr("sad")["name"] == "sad"
    with pytest.raises(RobotError, match="no reply to 'blink'"):
        robot.result(first)
    assert time.monotonic() - t0 < 0.5  # no waiting for the timeout


def test_stray_reply_is_ignored(robot, fake, caplog):
    fake.emit({"type": "ack", "cmd": "servo"})  # nobody asked
    assert robot.ping()["type"] == "pong"
    time.sleep(0.1)
    assert "matches no pending command" in caplog.text


def test_status_versus_periodic_telemetry():
    fake = FakeRobot(telemetry_period=0.005)  # a flood of unrequested telemetry
    robot = Robot(fake, timeout=1.0)
    try:
        seen = []
        robot.on_telemetry(seen.append)
        errors = []

        def worker(n):
            try:
                for i in range(50):
                    assert robot.status()["type"] == "telemetry"
                    assert robot.expr("happy" if (i + n) % 2 else "sad")["cmd"] == "expr"
                    assert robot.ping()["type"] == "pong"
            except Exception as exc:  # noqa: BLE001 - surface in the main thread
                errors.append(exc)

        threads = [threading.Thread(target=worker, args=(n,)) for n in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert not errors
        assert len(seen) > 50
        assert robot.telemetry["type"] == "telemetry"
    finally:
        robot.close()


def test_reboot_fails_pending(robot, fake):
    fake.drop_replies.add("blink")
    pending = robot.send("blink")
    booted = []
    robot.on_boot(booted.append)
    fake.reboot()
    with pytest.raises(RobotReset):
        robot.result(pending)
    assert robot.boot_count == 1 and booted[0]["type"] == "boot"
    assert robot.ping()["type"] == "pong"


def test_reboot_command(robot):
    assert robot.reboot()["cmd"] == "reboot"
    time.sleep(0.1)
    assert robot.boot_count == 1


def test_wait_ready_through_boot(fake):
    fake.drop_replies.add("ping")
    robot = Robot(fake, timeout=1.0)
    try:
        threading.Timer(0.7, lambda: (fake.drop_replies.discard("ping"), fake.reboot())).start()
        t0 = time.monotonic()
        robot.wait_ready(timeout=3)
        assert 0.6 < time.monotonic() - t0 < 2.0
    finally:
        robot.close()


def test_log_lines_go_to_logging(robot, fake, caplog):
    with caplog.at_level(logging.DEBUG, logger="backend.communication.serial_interface"):
        fake.emit("I (947) mic: INMP441 on SCK=15")
        fake.emit("{not json")
        assert robot.ping()["type"] == "pong"
        time.sleep(0.1)
    assert "INMP441 on SCK=15" in caplog.text
    assert "{not json" in caplog.text


def test_mic_blocks_and_gaps(robot, fake):
    fake.mic_skip = {5, 6, 9}
    blocks = []
    robot.mic_on(lambda samples, seq: blocks.append((seq, samples)))
    time.sleep(0.4)
    robot.mic_off()
    seqs = [s for s, _ in blocks]
    assert seqs == sorted(seqs) and seqs[0] == 0 and len(seqs) >= 10
    assert all(len(b) == 320 and b.dtype == np.dtype("<i2") and b[0] == s for s, b in blocks)
    assert robot.mic_gaps == 3
    n = len(blocks)
    time.sleep(0.1)
    assert len(blocks) == n  # nothing after mic_off


def test_play_paces_the_stream(robot, fake):
    seconds = 1.5
    audio = (np.sin(np.arange(int(seconds * RATE)) / 10) * 8000).astype(np.int16)
    t0 = time.monotonic()
    assert robot.play(audio) == 0
    elapsed = time.monotonic() - t0
    assert seconds * 0.95 < elapsed < seconds + 0.5  # real time, not faster or much slower
    assert fake.dropped_total == 0
    assert fake.max_buffered <= RATE  # kept near the 0.75 s target, never near capacity
    pcm_lines = [line for line in fake.received if line.startswith("pcm ")]
    assert pcm_lines[-1] == "pcm end" and len(pcm_lines) == int(seconds * RATE) // 320 + 1


def test_closed_robot_rejects_commands(fake):
    robot = Robot(fake)
    robot.close()
    with pytest.raises(RobotError):
        robot.ping()
