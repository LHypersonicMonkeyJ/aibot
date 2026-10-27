"""Joint definitions for the three-servo robot.

Angles are in degrees, measured from the neutral pose. Limits are
placeholders until the mechanical design fixes the real ranges.
"""

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class Joint:
    name: str
    min_deg: float
    max_deg: float
    max_vel: float  # deg/s
    max_acc: float  # deg/s^2

    def clamp(self, angle: float) -> float:
        return float(np.clip(angle, self.min_deg, self.max_deg))

    def in_limits(self, angle: float) -> bool:
        return self.min_deg <= angle <= self.max_deg


# Order matches the joint vector q = [body yaw, head yaw, head pitch].
# MG90S no-load speed is roughly 600 deg/s; these stay well below it.
JOINTS = (
    Joint("body_yaw", -90.0, 90.0, max_vel=120.0, max_acc=480.0),
    Joint("head_yaw", -60.0, 60.0, max_vel=180.0, max_acc=720.0),
    Joint("head_pitch", -20.0, 30.0, max_vel=150.0, max_acc=600.0),
)

NEUTRAL = np.zeros(len(JOINTS))
