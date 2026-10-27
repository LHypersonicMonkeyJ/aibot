# Simulator

A software-only stand-in for the servos, so motion and behavior code can be developed before the robot has a body.

It models the joint vector `q = [body_yaw, head_yaw, head_pitch]` in degrees from neutral. It plans synchronized trapezoidal-velocity moves at the firmware's 50 Hz control rate, then plots position, velocity and acceleration with joint limits marked.

The joint limits and speed caps in `robot_sim/joints.py` are placeholders until the mechanical design is done.

```bash
cd sim
pip install -r requirements.txt
python3 -m pytest tests
python3 -m robot_sim.demo            # plot in a window
python3 -m robot_sim.demo demo.png   # or save it
```

Planned next steps:

- A servo response model, so the plots can compare commanded and actual motion.
- Feeding the simulator from the firmware's simulation mode over serial.
