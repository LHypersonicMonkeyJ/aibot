"""Run a short idle-style motion sequence and plot it.

    python3 -m robot_sim.demo            # opens a window
    python3 -m robot_sim.demo out.png    # saves to a file instead
"""

import sys

import matplotlib

from .trajectory import plan_sequence

# q = [body yaw, head yaw, head pitch], degrees
SEQUENCE = [
    (0, 40, 0),     # look left
    (0, -40, 0),    # look right
    (0, 0, 15),     # tilt head up ("thinking")
    (30, 0, -10),   # turn body, look down
    (0, 0, 45),     # out of range on purpose: head pitch gets clamped
    (0, 0, 0),      # back to neutral
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else None
    if out:
        matplotlib.use("Agg")
    from .plot import plot_trajectory

    traj = plan_sequence(SEQUENCE)
    for t, name, angle in traj.violations:
        print(f"limit violation at {t:.2f}s: {name} requested {angle:.1f} deg (clamped)")

    fig = plot_trajectory(traj, title="Demo sequence")
    if out:
        fig.savefig(out, dpi=120)
        print(f"saved {out}")
    else:
        import matplotlib.pyplot as plt
        plt.show()


if __name__ == "__main__":
    main()
