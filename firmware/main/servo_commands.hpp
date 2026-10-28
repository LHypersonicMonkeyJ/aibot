#pragma once

// Sets up the I2C bus and PCA9685. Safe to call with nothing wired up.
void servo_init();
bool servo_ready();
void register_servo_commands();
