"""Synchronized trapezoidal-velocity trajectories.

Each move takes every joint from a start pose to a goal pose. The slowest
joint sets the duration, and the other joints are slowed down to finish at
the same time, so the motion looks coordinated. This is the algorithm the
firmware motion controller will implement in C++.
"""

from dataclasses import dataclass, field

import numpy as np

from .joints import JOINTS, NEUTRAL, Joint

CONTROL_RATE_HZ = 50.0  # matches the firmware motion task


def min_duration(distance: float, vmax: float, amax: float) -> float:
    """Shortest time to cover `distance` from rest to rest."""
    d = abs(distance)
    if d == 0.0:
        return 0.0
    if d >= vmax**2 / amax:  # reaches cruise speed
        return d / vmax + vmax / amax
    return 2.0 * np.sqrt(d / amax)  # triangular profile


def _cruise_speed(d: float, duration: float, amax: float) -> float:
    """Cruise speed so a trapezoid with accel `amax` covers `d` in exactly `duration`."""
    # d = v * (T - v / a)  ->  v^2 / a - v T + d = 0, take the smaller root.
    disc = max(amax**2 * duration**2 - 4.0 * amax * d, 0.0)
    return (amax * duration - np.sqrt(disc)) / 2.0


def _profile(t: np.ndarray, d: float, duration: float, amax: float):
    """Position, velocity and acceleration along a 1-D trapezoid starting at 0."""
    sign = np.sign(d)
    d = abs(d)
    pos = np.zeros_like(t)
    vel = np.zeros_like(t)
    acc = np.zeros_like(t)
    if d == 0.0 or duration == 0.0:
        return pos, vel, acc

    v = _cruise_speed(d, duration, amax)
    ta = v / amax
    t = np.clip(t, 0.0, duration)

    accel = t < ta
    decel = t > duration - ta
    cruise = ~accel & ~decel

    pos[accel] = 0.5 * amax * t[accel] ** 2
    vel[accel] = amax * t[accel]
    acc[accel] = amax

    pos[cruise] = 0.5 * amax * ta**2 + v * (t[cruise] - ta)
    vel[cruise] = v

    tr = duration - t[decel]
    pos[decel] = d - 0.5 * amax * tr**2
    vel[decel] = amax * tr
    acc[decel] = -amax

    return sign * pos, sign * vel, sign * acc


@dataclass
class Trajectory:
    t: np.ndarray    # (N,) seconds
    pos: np.ndarray  # (N, J) degrees
    vel: np.ndarray  # (N, J) deg/s
    acc: np.ndarray  # (N, J) deg/s^2
    cmd: np.ndarray  # (N, J) raw commanded goal, before clamping
    violations: list = field(default_factory=list)  # (time, joint name, requested angle)


def plan_move(start, goal, joints=JOINTS, rate_hz=CONTROL_RATE_HZ, t0=0.0):
    """Plan one synchronized move. Goals outside joint limits are clamped and reported."""
    start = np.asarray(start, dtype=float)
    goal = np.asarray(goal, dtype=float)
    clamped = np.array([j.clamp(g) for j, g in zip(joints, goal)])
    violations = [(t0, j.name, float(g)) for j, g in zip(joints, goal) if not j.in_limits(g)]

    delta = clamped - start
    duration = max(min_duration(d, j.max_vel, j.max_acc) for j, d in zip(joints, delta))

    n = int(np.ceil(duration * rate_hz)) + 1
    t = np.arange(n) / rate_hz

    pos = np.empty((n, len(joints)))
    vel = np.empty_like(pos)
    acc = np.empty_like(pos)
    for i, (j, d) in enumerate(zip(joints, delta)):
        p, v, a = _profile(t, d, duration, j.max_acc)
        pos[:, i] = start[i] + p
        vel[:, i] = v
        acc[:, i] = a

    cmd = np.tile(goal, (n, 1))
    return Trajectory(t + t0, pos, vel, acc, cmd, violations)


def plan_sequence(goals, hold_s=0.3, start=NEUTRAL, joints=JOINTS, rate_hz=CONTROL_RATE_HZ):
    """Chain several moves, holding each pose for `hold_s` seconds."""
    pieces = []
    pose = np.asarray(start, dtype=float)
    t0 = 0.0
    hold_n = int(round(hold_s * rate_hz))
    for goal in goals:
        move = plan_move(pose, goal, joints, rate_hz, t0)
        pieces.append(move)
        pose = move.pos[-1]
        t0 = move.t[-1] + 1.0 / rate_hz
        if hold_n:
            th = t0 + np.arange(hold_n) / rate_hz
            zeros = np.zeros((hold_n, len(joints)))
            pieces.append(Trajectory(th, np.tile(pose, (hold_n, 1)), zeros, zeros.copy(),
                                     np.tile(move.cmd[-1], (hold_n, 1))))
            t0 = th[-1] + 1.0 / rate_hz

    return Trajectory(
        t=np.concatenate([p.t for p in pieces]),
        pos=np.vstack([p.pos for p in pieces]),
        vel=np.vstack([p.vel for p in pieces]),
        acc=np.vstack([p.acc for p in pieces]),
        cmd=np.vstack([p.cmd for p in pieces]),
        violations=[v for p in pieces for v in p.violations],
    )
