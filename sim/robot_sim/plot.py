"""Plot joint position, velocity and acceleration against time."""

import matplotlib.pyplot as plt

from .joints import JOINTS


def plot_trajectory(traj, joints=JOINTS, title="Joint trajectory"):
    fig, axes = plt.subplots(3, len(joints), figsize=(4 * len(joints), 8), sharex=True, squeeze=False)
    for i, j in enumerate(joints):
        ax_p, ax_v, ax_a = axes[:, i]

        ax_p.plot(traj.t, traj.pos[:, i], label="planned")
        ax_p.plot(traj.t, traj.cmd[:, i], "--", alpha=0.6, label="commanded")
        for lim in (j.min_deg, j.max_deg):
            ax_p.axhline(lim, color="red", linewidth=0.8, alpha=0.5)
        for t, name, angle in traj.violations:
            if name == j.name:
                ax_p.plot(t, angle, "rx", markersize=10)
        ax_p.set_title(j.name)

        ax_v.plot(traj.t, traj.vel[:, i])
        for lim in (-j.max_vel, j.max_vel):
            ax_v.axhline(lim, color="gray", linewidth=0.8, linestyle=":")

        ax_a.plot(traj.t, traj.acc[:, i])
        for lim in (-j.max_acc, j.max_acc):
            ax_a.axhline(lim, color="gray", linewidth=0.8, linestyle=":")
        ax_a.set_xlabel("time (s)")

    axes[0, 0].set_ylabel("position (deg)")
    axes[1, 0].set_ylabel("velocity (deg/s)")
    axes[2, 0].set_ylabel("acceleration (deg/s²)")
    axes[0, 0].legend(loc="best", fontsize="small")
    fig.suptitle(title)
    fig.tight_layout()
    return fig
