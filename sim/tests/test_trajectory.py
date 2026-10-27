import numpy as np
import pytest

from robot_sim.joints import JOINTS, NEUTRAL
from robot_sim.trajectory import CONTROL_RATE_HZ, min_duration, plan_move, plan_sequence

TOL = 1e-6


@pytest.mark.parametrize("goal", [(0, 40, 0), (30, -50, 20), (-90, 60, -20), (1, 0.5, 0.2)])
def test_move_reaches_goal_within_limits(goal):
    traj = plan_move(NEUTRAL, goal)
    np.testing.assert_allclose(traj.pos[0], NEUTRAL, atol=TOL)
    np.testing.assert_allclose(traj.pos[-1], goal, atol=TOL)
    np.testing.assert_allclose(traj.vel[-1], 0.0, atol=TOL)
    for i, j in enumerate(JOINTS):
        assert np.all(np.abs(traj.vel[:, i]) <= j.max_vel + TOL)
        assert np.all(np.abs(traj.acc[:, i]) <= j.max_acc + TOL)
    assert traj.violations == []


def test_joints_finish_together():
    traj = plan_move(NEUTRAL, (80, 5, 2))
    # every moving joint is still moving one step before the end
    assert np.all(np.abs(traj.vel[-2]) > 0)


def test_samples_at_control_rate():
    traj = plan_move(NEUTRAL, (0, 40, 0))
    np.testing.assert_allclose(np.diff(traj.t), 1.0 / CONTROL_RATE_HZ)


def test_out_of_range_goal_is_clamped_and_reported():
    traj = plan_move(NEUTRAL, (0, 0, 45))
    pitch = JOINTS[2]
    assert traj.pos[-1, 2] == pytest.approx(pitch.max_deg)
    assert traj.violations == [(0.0, "head_pitch", 45.0)]


def test_zero_move():
    traj = plan_move(NEUTRAL, NEUTRAL)
    assert len(traj.t) == 1
    assert min_duration(0.0, 100, 100) == 0.0


def test_triangular_and_trapezoid_durations():
    # short move never reaches vmax: t = 2*sqrt(d/a)
    assert min_duration(1.0, 100.0, 100.0) == pytest.approx(0.2)
    # long move cruises: t = d/v + v/a
    assert min_duration(300.0, 100.0, 100.0) == pytest.approx(4.0)


def test_sequence_is_continuous():
    traj = plan_sequence([(0, 40, 0), (0, -40, 0), (0, 0, 0)])
    assert np.all(np.diff(traj.t) > 0)
    # no jumps larger than max speed allows in one step
    step = np.abs(np.diff(traj.pos, axis=0))
    max_step = np.array([j.max_vel for j in JOINTS]) / CONTROL_RATE_HZ
    assert np.all(step <= max_step + TOL)
    np.testing.assert_allclose(traj.pos[-1], NEUTRAL, atol=TOL)
